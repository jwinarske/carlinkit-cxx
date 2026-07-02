// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#include "audio_alsa.h"

#include <alsa/asoundlib.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "dongle.h"  // AudioFrame

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

// ── AudioMixer ──────────────────────────────────────────────────────────────
AudioMixer::AudioMixer(const char* device) : device_(device) {
  if (const char* e = std::getenv("CARLINKIT_AUDIO_LATENCY_MS"); e != nullptr) {
    const auto ms = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    if (ms >= 20 && ms <= 1000)
      latency_us_ = ms * 1000;
  }
}

AudioMixer::~AudioMixer() {
  stop();
}

void AudioMixer::start() {
  if (running_.exchange(true))
    return;
  thread_ = std::thread([this] { run(); });
}

void AudioMixer::stop() {
  if (!running_.exchange(false))
    return;
  cv_.notify_all();
  if (thread_.joinable())
    thread_.join();
  if (pcm_ != nullptr) {
    snd_pcm_close(pcm_);
    pcm_ = nullptr;
  }
}

AudioMixer::Lane& AudioMixer::lane_for(uint32_t decode_type,
                                       uint32_t audio_type) {
  const uint64_t key = (static_cast<uint64_t>(decode_type) << 32) | audio_type;
  if (auto it = lanes_.find(key); it != lanes_.end())
    return it->second;
  const PcmFormat fmt = decode_type_format(decode_type);
  Lane l;
  l.rate = fmt.rate;
  l.channels = fmt.channels;
  return lanes_.emplace(key, std::move(l)).first->second;
}

void AudioMixer::submit(const AudioFrame& f) {
  if (!running_)
    return;
  std::lock_guard<std::mutex> lk(m_);
  Lane& l = lane_for(f.decodeType, f.audioType);
  l.last = std::chrono::steady_clock::now();
  if (f.hasVolumeDuration) {
    // Duck: ramp this lane's gain toward `volume` over `volumeDuration`
    // seconds.
    const double target =
        f.volume < 0.0F ? 0.0 : (f.volume > 1.0F ? 1.0 : f.volume);
    const double secs = f.volumeDuration > 0.0F ? f.volumeDuration : 0.02;
    l.target = target;
    l.step = (l.target - l.gain) / std::max(1.0, secs * l.rate);
  } else if (f.pcm != nullptr && f.samples != 0) {
    l.ring.insert(l.ring.end(), f.pcm, f.pcm + f.samples);
    const size_t cap = static_cast<size_t>(l.rate) * l.channels;  // ~1s backlog
    if (l.ring.size() > cap) {  // safety: drop oldest, keep pos consistent
      const size_t drop = l.ring.size() - cap;
      l.ring.erase(l.ring.begin(), l.ring.begin() + static_cast<long>(drop));
      const size_t drop_frames = drop / l.channels;
      const auto df = static_cast<double>(drop_frames);
      l.pos = l.pos > df ? l.pos - df : 0.0;
    }
  }
  cv_.notify_one();
}

bool AudioMixer::ensure_params(unsigned rate, unsigned channels) {
  if (pcm_ != nullptr && rate == cur_rate_ && channels == cur_channels_)
    return true;
  if (pcm_ != nullptr) {  // format change — drain, then reopen
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
                                 SND_PCM_ACCESS_RW_INTERLEAVED, channels, rate,
                                 1 /*resample*/, latency_us_);
      r < 0) {
    std::fprintf(stderr, "alsa: set_params(%uHz/%uch) failed: %s\n", rate,
                 channels, snd_strerror(r));
    snd_pcm_close(pcm_);
    pcm_ = nullptr;
    return false;
  }
  unmute_pcm_card(pcm_);
  cur_rate_ = rate;
  cur_channels_ = channels;
  return true;
}

void AudioMixer::mix_lane(Lane& l, std::vector<int32_t>& acc, int block) const {
  const double ratio = static_cast<double>(l.rate) / out_rate_;  // src/out
  const size_t avail = l.ring.size() / l.channels;  // available source frames
  for (int i = 0; i < block; ++i) {
    l.gain += l.step;  // advance the gain envelope once per output frame
    if ((l.step > 0 && l.gain > l.target) || (l.step < 0 && l.gain < l.target))
      l.gain = l.target;
    const auto i0 = static_cast<size_t>(l.pos);
    if (i0 >= avail)
      break;  // out of source data — silence for the rest of the block
    const size_t i1 = (i0 + 1 < avail) ? i0 + 1 : i0;  // clamp at the tail
    const double frac = l.pos - static_cast<double>(i0);
    const size_t o = static_cast<size_t>(i) * out_channels_;
    if (l.channels == 1) {
      const double s = (l.ring[i0] * (1.0 - frac) + l.ring[i1] * frac) * l.gain;
      const auto v = static_cast<int32_t>(std::lround(s));
      acc[o] += v;
      acc[o + 1] += v;  // mono upmixed to stereo
    } else {            // stereo
      const double sl =
          (l.ring[i0 * 2] * (1.0 - frac) + l.ring[i1 * 2] * frac) * l.gain;
      const double sr =
          (l.ring[i0 * 2 + 1] * (1.0 - frac) + l.ring[i1 * 2 + 1] * frac) *
          l.gain;
      acc[o] += static_cast<int32_t>(std::lround(sl));
      acc[o + 1] += static_cast<int32_t>(std::lround(sr));
    }
    l.pos += ratio;
  }
  // Drop the source frames we advanced past; keep the fractional remainder.
  const size_t consumed = std::min(static_cast<size_t>(l.pos), avail);
  if (consumed > 0) {
    l.ring.erase(l.ring.begin(),
                 l.ring.begin() + static_cast<long>(consumed * l.channels));
    l.pos -= static_cast<double>(consumed);
  }
}

void AudioMixer::run() {
  const int block = static_cast<int>(out_rate_ / 100);  // 10 ms
  std::vector<int32_t> acc;
  std::vector<int16_t> out;
  auto last_audio = std::chrono::steady_clock::now() - std::chrono::hours(1);
  while (running_) {
    {
      std::unique_lock<std::mutex> lk(m_);
      const auto now = std::chrono::steady_clock::now();
      // Drop lanes with no audio for a while so stale ones don't linger.
      for (auto it = lanes_.begin(); it != lanes_.end();) {
        if (it->second.ring.empty() &&
            now - it->second.last > std::chrono::seconds(3))
          it = lanes_.erase(it);
        else
          ++it;
      }
      const bool data =
          std::any_of(lanes_.begin(), lanes_.end(),
                      [](const auto& kv) { return !kv.second.ring.empty(); });
      if (data)
        last_audio = now;
      // Keep the device running through brief gaps between streams (e.g. music
      // stopping while Siri "thinks") by writing silence, so the next stream
      // doesn't restart the PCM and click. Only go idle after a longer silence.
      constexpr auto kKeepAlive = std::chrono::milliseconds(4000);
      if (!data && now - last_audio > kKeepAlive) {  // truly idle — wait
        cv_.wait_for(lk, std::chrono::milliseconds(50), [this] {
          return !running_ ||
                 std::any_of(lanes_.begin(), lanes_.end(), [](const auto& kv) {
                   return !kv.second.ring.empty();
                 });
        });
        continue;
      }
      // Mix every lane into the fixed-rate output (silence if a lane is empty),
      // resampled as needed. The device never reconfigures, so stream
      // boundaries don't click.
      acc.assign(static_cast<size_t>(block) * out_channels_, 0);
      for (auto& kv : lanes_)
        mix_lane(kv.second, acc, block);
    }  // release the lock before the blocking ALSA write

    if (!ensure_params(out_rate_, out_channels_))
      continue;
    out.resize(acc.size());
    for (size_t i = 0; i < acc.size(); ++i) {
      const int32_t v = acc[i];
      out[i] =
          static_cast<int16_t>(v < -32768 ? -32768 : (v > 32767 ? 32767 : v));
    }
    const int16_t* p = out.data();
    auto frames = static_cast<snd_pcm_uframes_t>(block);
    while (frames > 0 && running_) {
      snd_pcm_sframes_t n = snd_pcm_writei(pcm_, p, frames);
      if (n < 0) {
        if (n == -EPIPE && (xruns_++ % 50) == 0)
          std::fprintf(stderr, "alsa: playback underrun (xrun #%d)\n", xruns_);
        n = snd_pcm_recover(pcm_, static_cast<int>(n), 1);
        if (n < 0) {
          std::fprintf(stderr, "alsa: writei: %s\n",
                       snd_strerror(static_cast<int>(n)));
          break;
        }
        continue;
      }
      p += static_cast<size_t>(n) * out_channels_;
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
  std::vector<int16_t> buf(320);  // ~20 ms
  bool open_warned = false;
  const auto retry_nap = [this] {
    for (int i = 0; i < 5 && running_; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
  };
  // (Re)open the mic and capture until a hard error, retrying while running_. A
  // busy/not-yet-ready device, a mid-call glitch, or a USB-mic replug must not
  // mute Siri for the rest of the session -- so we reopen rather than give up.
  while (running_) {
    snd_pcm_t* pcm = nullptr;
    if (int r = snd_pcm_open(&pcm, device_.c_str(), SND_PCM_STREAM_CAPTURE, 0);
        r < 0) {
      if (!open_warned) {  // log once per failure streak, not every retry
        std::fprintf(stderr, "alsa: capture open(%s): %s; retrying\n",
                     device_.c_str(), snd_strerror(r));
        open_warned = true;
      }
      retry_nap();
      continue;
    }
    // 16 kHz mono S16LE to match send_audio()'s decodeType 5.
    if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
                           SND_PCM_ACCESS_RW_INTERLEAVED, 1, 16000, 1,
                           100000) < 0) {
      snd_pcm_close(pcm);
      retry_nap();
      continue;
    }
    open_warned = false;  // opened cleanly; reset the streak
    while (running_) {
      snd_pcm_sframes_t n = snd_pcm_readi(pcm, buf.data(), buf.size());
      if (n < 0) {
        n = snd_pcm_recover(pcm, static_cast<int>(n), 1);
        if (n < 0) {  // unrecoverable (e.g. the USB mic vanished) -- reopen
          std::fprintf(stderr, "alsa: capture error: %s; reopening\n",
                       snd_strerror(static_cast<int>(n)));
          break;
        }
        continue;
      }
      if (n > 0 && cb_)
        cb_(buf.data(), static_cast<size_t>(n));
    }
    snd_pcm_close(pcm);
  }
}

}  // namespace ck
