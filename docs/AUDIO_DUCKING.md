# Audio Ducking and Mixing

How the receiver plays the dongle's audio when more than one source is active —
music, navigation prompts, Siri, phone calls. The design follows what the dongle
actually does on the wire, captured by tracing music with Siri and turn-by-turn
navigation over it (`CARLINKIT_AUDIO_TRACE=1`).

## What the dongle does

Each `AudioData` stream is keyed by `(decodeType, audioType)`. Secondary audio
arrives in one of two ways:

- **Siri and phone calls — serialized.** When the voice session starts, the phone
  **stops** the music stream (`MediaStop`/`OutputStop`), plays the voice stream,
  then **restarts** music (`MediaStart`/`OutputStart`). The two streams never
  overlap in time, and no volume change is sent.
- **Navigation prompts — concurrent and ducked.** The dongle sends a
  `volumeDuration` frame on the music stream (target ≈ 0.2 over ≈ 0.5 s); the
  music keeps playing **under** the prompt, and the prompt arrives as a second
  stream at the **same** 44100/stereo format.

Three facts shaped the implementation:

- Concurrent streams always share a format (music and navigation are both
  44100/2), so mixing them needs no sample-rate conversion.
- The per-PCM `volume` field is unused (always 0.0); ducking arrives only as the
  4-byte `volumeDuration` payload variant.
- Voice streams (Siri 16000/1, etc.) are a different format but are serialized, so
  they never need to be mixed with music.

## AudioData parsing

A 12-byte header (`decodeType` u32, `volume` f32, `audioType` u32) followed by a
payload whose length selects its meaning: 1 byte = `AudioCommand`; **4 bytes =
`volumeDuration` f32** (the duck — ramp the stream to `volume` over this many
seconds); otherwise S16LE PCM. `AudioFrame` carries `volumeDuration` and
`hasVolumeDuration`.

## The mixer

`AudioMixer` replaces the old single-stream playback:

- **Lanes.** One lane per `(decodeType, audioType)`: a native-format S16 ring, a
  gain envelope (`gain`/`target`/`step`), and a fractional resample position.
- **Ducking.** A `volumeDuration` frame ramps the targeted lane's gain toward
  `volume` over its duration; the envelope advances once per output frame.
- **Fixed output (44100/2), opened once, never reconfigured.** Every lane mixes
  into it:
  - Same-rate lanes (music, navigation) pass through unchanged; off-rate voice
    (e.g. Siri 16000/1) is **linearly resampled** in and mono is upmixed to
    stereo. 44100 is the media rate, so the continuous music stream is never
    resampled in our code — only short voice prompts are — and ALSA's `plug`
    layer converts the final mix to the device's hardware rate.
  - Lanes are summed with per-lane gain and saturated to S16.
- **Keep-alive.** While audio is playing or was playing within the last few
  seconds, the device is fed silence through brief inter-stream gaps (e.g. music
  stopping while Siri "thinks") so the PCM never stops and restarts — that restart
  was the click heard at Siri's boundaries. The device only goes idle after a
  longer silence.

## Why no resampling library

Mixing only ever combines **same-format** streams (music + navigation, both
44100/2). The only different-format streams are voice prompts, which the dongle
**serializes**, so they play alone — where a small built-in linear resampler into
the fixed output is more than adequate for speech. `libswresample` and the
per-lane resampling an early design assumed are therefore unnecessary.

## Microphone

Unchanged. `AudioInput` captures 16 kHz mono for Siri and phone calls; the host
starts it on the `AudioCommand` lifecycle (`SiriStart`/`PhonecallStart`/
`InputConfig`) and stops it on the matching stop.

## Scenarios

- **Music + navigation:** music ducks to ≈ 0.2 under the prompt (both 44100/2,
  summed), and ramps back when the prompt ends.
- **Music + Siri:** music stops, Siri plays (resampled to 44100/2), music resumes;
  the device stays open across the gap, so there is no click at either boundary.
- **Incoming call:** like Siri (serialized), with the microphone active for the
  call's duration.
- **Secondary with no music:** the single active stream plays on its own.

## Diagnostics and tuning

- `CARLINKIT_AUDIO_TRACE=1` logs every `AudioData` (timestamp, `decodeType`,
  `audioType`, and kind: PCM / command / duck) — the tool used to characterize the
  behavior above, and to settle any future audio-routing question.
- `CARLINKIT_AUDIO_LATENCY_MS` sets the ALSA playback buffer (default 120).
