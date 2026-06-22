// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#pragma once
// ALSA audio for the CarPlay link: playback of the dongle's PCM AudioData, and
// on-demand microphone capture (for Siri / phone calls) sent back to the
// dongle.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// NOLINTNEXTLINE(bugprone-reserved-identifier) — ALSA's own opaque type name.
typedef struct _snd_pcm snd_pcm_t;

namespace ck {

struct AudioFrame;  // dongle.h

// PCM shape for a dongle AudioData decodeType (all S16LE). See node-CarPlay
// decodeTypeMap.
struct PcmFormat {
  unsigned rate;
  unsigned channels;
};
PcmFormat decode_type_format(uint32_t decode_type);

// Mixing playback. The dongle keys audio by (decodeType, audioType): concurrent
// same-format streams (e.g. music ducked under a navigation prompt) are summed
// with per-stream gain, and a volumeDuration frame ramps a stream's gain (the
// duck). A different-format stream (e.g. Siri, which the dongle serializes by
// stopping music) becomes the active format and triggers a device reconfigure —
// no resampling, because concurrent streams always share a format. PCM is fed
// from the dongle RX thread; a dedicated thread mixes and writes so ALSA
// backpressure never stalls USB reception.
class AudioMixer {
 public:
  explicit AudioMixer(const char* device = "default");
  ~AudioMixer();

  void start();
  void stop();

  // Route a decoded AudioData frame: PCM is appended to its lane; a
  // volumeDuration frame ramps its lane's gain. Commands are ignored here.
  void submit(const AudioFrame& f);

 private:
  void run();
  bool ensure_params(unsigned rate, unsigned channels);

  // One audio stream: native-format S16 ring + a gain envelope.
  struct Lane {
    unsigned rate = 0;
    unsigned channels = 0;
    std::deque<int16_t> ring;
    double gain = 1.0;
    double target = 1.0;
    double step = 0.0;  // per-output-frame gain increment toward target
    double pos = 0.0;   // fractional source-frame read position (resampling)
    std::chrono::steady_clock::time_point last{};
  };
  Lane& lane_for(uint32_t decode_type, uint32_t audio_type);  // holds m_
  // Linear-resample `block` output frames from a lane into the mix accumulator
  // (mono upmixed to stereo), applying the lane's gain. Holds m_.
  void mix_lane(Lane& l, std::vector<int32_t>& acc, int block) const;

  std::string device_;
  snd_pcm_t* pcm_ = nullptr;
  // Fixed output format: the device opens once and never reconfigures, so a
  // different-rate stream (e.g. Siri 16000/1) is resampled in rather than
  // forcing a device reconfigure (which clicks at the stream boundaries). 44100
  // is the media rate, so music/navigation pass through untouched.
  unsigned out_rate_ = 44100;
  unsigned out_channels_ = 2;
  unsigned cur_rate_ = 0;
  unsigned cur_channels_ = 0;
  // Target buffer latency; larger absorbs more jitter at the cost of delay.
  unsigned latency_us_ = 120000;
  int xruns_ = 0;  // playback-thread underrun count (diagnostic)

  std::mutex m_;
  std::condition_variable cv_;
  std::unordered_map<uint64_t, Lane>
      lanes_;  // key = decodeType<<32 | audioType
  std::thread thread_;
  std::atomic<bool> running_{false};
};

// Microphone capture (16 kHz mono S16LE), started/stopped on demand. Each block
// is delivered to the callback (which forwards it to the dongle via
// send_audio).
class AudioInput {
 public:
  using Cb = std::function<void(const int16_t* pcm, size_t samples)>;
  AudioInput(const char* device, Cb cb);
  ~AudioInput();

  void start();  // begin capturing (idempotent)
  void stop();   // stop capturing (idempotent)
  [[nodiscard]] bool active() const { return running_; }

 private:
  void run();

  std::string device_;
  Cb cb_;
  std::thread thread_;
  std::atomic<bool> running_{false};
};

}  // namespace ck
