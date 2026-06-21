// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#include "audio_alsa.h"

#include <alsa/asoundlib.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>

namespace ck {

namespace {
// Unmute every playback element on the card the PCM is on. USB DACs (and
// others) often boot with the PCM/Master switch muted, which silences playback
// even though writes succeed — so we clear it on open.
void unmute_pcm_card(snd_pcm_t* pcm) {
  snd_pcm_info_t* info = nullptr;
  if (snd_pcm_info_malloc(&info) < 0)
    return;
  const int card =
      (snd_pcm_info(pcm, info) == 0) ? snd_pcm_info_get_card(info) : -1;
  snd_pcm_info_free(info);
  if (card < 0)
    return;

  char hw[16];
  std::snprintf(hw, sizeof hw, "hw:%d", card);
  snd_mixer_t* mixer = nullptr;
  if (snd_mixer_open(&mixer, 0) < 0)
    return;
  if (snd_mixer_attach(mixer, hw) == 0 &&
      snd_mixer_selem_register(mixer, nullptr, nullptr) == 0 &&
      snd_mixer_load(mixer) == 0) {
    for (snd_mixer_elem_t* e = snd_mixer_first_elem(mixer); e != nullptr;
         e = snd_mixer_elem_next(e))
      if (snd_mixer_selem_is_active(e) != 0 &&
          snd_mixer_selem_has_playback_switch(e) != 0)
        snd_mixer_selem_set_playback_switch_all(e, 1);  // 1 = unmuted
  }
  snd_mixer_close(mixer);
}
}  // namespace

PcmFormat decode_type_format(uint32_t decode_type) {
  switch (decode_type) {
    case 1:
    case 2:
      return {44100, 2};
    case 3:
      return {8000, 1};
    case 4:
      return {48000, 2};
    case 5:
      return {16000, 1};
    case 6:
      return {24000, 1};
    case 7:
      return {16000, 2};
    default:
      return {44100, 2};
  }
}

// ── AudioOutput ─────────────────────────────────────────────────────────────
AudioOutput::AudioOutput(const char* device) : device_(device) {
  if (const char* e = std::getenv("CARLINKIT_AUDIO_LATENCY_MS"); e != nullptr) {
    const auto ms = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    if (ms >= 20 && ms <= 1000)
      latency_us_ = ms * 1000;
  }
}

AudioOutput::~AudioOutput() {
  stop();
}

void AudioOutput::start() {
  if (running_.exchange(true))
    return;
  thread_ = std::thread([this] { run(); });
}

void AudioOutput::stop() {
  if (!running_.exchange(false))
    return;
  cv_.notify_all();
  if (thread_.joinable())
    thread_.join();
  if (pcm_) {
    snd_pcm_close(pcm_);
    pcm_ = nullptr;
  }
}

void AudioOutput::submit(uint32_t decode_type,
                         const int16_t* pcm,
                         size_t samples) {
  if (!running_ || samples == 0)
    return;
  Chunk c;
  c.decode_type = decode_type;
  c.pcm.assign(pcm, pcm + samples);
  {
    std::lock_guard<std::mutex> lk(m_);
    if (q_.size() > 64)  // bound latency on a slow/stalled sink
      q_.pop_front();
    q_.push_back(std::move(c));
  }
  cv_.notify_one();
}

bool AudioOutput::ensure_params(uint32_t decode_type) {
  const PcmFormat want = decode_type_format(decode_type);
  // Only reopen on a real rate/channel change. CarPlay alternates decodeTypes
  // that map to the same shape (e.g. 1 and 2 are both 44100/2); reopening for
  // those would click on HDMI for no reason.
  if (pcm_ && want.rate == cur_rate_ && want.channels == cur_channels_)
    return true;
  if (pcm_) {  // genuine format change — drain what's buffered, then reopen
    snd_pcm_drain(pcm_);
    snd_pcm_close(pcm_);
    pcm_ = nullptr;
  }
  if (int r = snd_pcm_open(&pcm_, device_.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
      r < 0) {
    std::fprintf(stderr, "alsa: open(%s) failed: %s\n", device_.c_str(),
                 snd_strerror(r));
    pcm_ = nullptr;
    return false;
  }
  if (int r = snd_pcm_set_params(pcm_, SND_PCM_FORMAT_S16_LE,
                                 SND_PCM_ACCESS_RW_INTERLEAVED, want.channels,
                                 want.rate, 1 /*resample*/, latency_us_);
      r < 0) {
    std::fprintf(stderr, "alsa: set_params(%uHz/%uch) failed: %s\n", want.rate,
                 want.channels, snd_strerror(r));
    snd_pcm_close(pcm_);
    pcm_ = nullptr;
    return false;
  }
  unmute_pcm_card(pcm_);  // clear a muted mixer so playback is actually audible
  cur_rate_ = want.rate;
  cur_channels_ = want.channels;
  return true;
}

void AudioOutput::run() {
  while (running_) {
    Chunk c;
    {
      std::unique_lock<std::mutex> lk(m_);
      cv_.wait(lk, [this] { return !running_ || !q_.empty(); });
      if (!running_)
        break;
      c = std::move(q_.front());
      q_.pop_front();
    }
    if (!ensure_params(c.decode_type))
      continue;
    const unsigned ch = decode_type_format(c.decode_type).channels;
    const int16_t* p = c.pcm.data();
    snd_pcm_uframes_t frames = c.pcm.size() / ch;
    while (frames > 0 && running_) {
      snd_pcm_sframes_t n = snd_pcm_writei(pcm_, p, frames);
      if (n < 0) {
        if (n == -EPIPE && (xruns_++ % 50) == 0)
          std::fprintf(stderr,
                       "alsa: playback underrun (xrun #%d) — audible click; a "
                       "larger buffer helps if frequent\n",
                       xruns_);
        n = snd_pcm_recover(pcm_, static_cast<int>(n), 1);
        if (n < 0) {
          std::fprintf(stderr, "alsa: writei: %s\n",
                       snd_strerror(static_cast<int>(n)));
          break;
        }
        continue;
      }
      p += static_cast<size_t>(n) * ch;
      frames -= static_cast<snd_pcm_uframes_t>(n);
    }
  }
}

// ── AudioInput (microphone) ─────────────────────────────────────────────────
AudioInput::AudioInput(const char* device, Cb cb)
    : device_(device), cb_(std::move(cb)) {}

AudioInput::~AudioInput() {
  stop();
}

void AudioInput::start() {
  if (running_.exchange(true))
    return;
  thread_ = std::thread([this] { run(); });
}

void AudioInput::stop() {
  if (!running_.exchange(false))
    return;
  if (thread_.joinable())
    thread_.join();
}

void AudioInput::run() {
  snd_pcm_t* pcm = nullptr;
  if (int r = snd_pcm_open(&pcm, device_.c_str(), SND_PCM_STREAM_CAPTURE, 0);
      r < 0) {
    std::fprintf(stderr, "alsa: capture open(%s): %s\n", device_.c_str(),
                 snd_strerror(r));
    running_ = false;
    return;
  }
  // 16 kHz mono S16LE to match send_audio()'s decodeType 5.
  if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
                         SND_PCM_ACCESS_RW_INTERLEAVED, 1, 16000, 1,
                         100000) < 0) {
    snd_pcm_close(pcm);
    running_ = false;
    return;
  }
  std::vector<int16_t> buf(320);  // ~20 ms
  while (running_) {
    snd_pcm_sframes_t n = snd_pcm_readi(pcm, buf.data(), buf.size());
    if (n < 0) {
      n = snd_pcm_recover(pcm, static_cast<int>(n), 1);
      if (n < 0)
        break;
      continue;
    }
    if (n > 0 && cb_)
      cb_(buf.data(), static_cast<size_t>(n));
  }
  snd_pcm_close(pcm);
}

}  // namespace ck
