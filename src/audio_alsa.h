// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#pragma once
// ALSA audio for the CarPlay link: playback of the dongle's PCM AudioData, and
// on-demand microphone capture (for Siri / phone calls) sent back to the
// dongle.
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// NOLINTNEXTLINE(bugprone-reserved-identifier) — ALSA's own opaque type name.
typedef struct _snd_pcm snd_pcm_t;

namespace ck {

// PCM shape for a dongle AudioData decodeType (all S16LE). See node-CarPlay
// decodeTypeMap.
struct PcmFormat {
  unsigned rate;
  unsigned channels;
};
PcmFormat decode_type_format(uint32_t decode_type);

// Playback. PCM is queued from the dongle RX thread and written by a dedicated
// thread so ALSA backpressure never stalls USB reception. Reconfigures the PCM
// device when the stream's rate/channels change.
class AudioOutput {
 public:
  explicit AudioOutput(const char* device = "default");
  ~AudioOutput();

  void start();
  void stop();

  // Queue S16LE samples (count is total int16s, i.e. frames * channels).
  void submit(uint32_t decode_type, const int16_t* pcm, size_t samples);

 private:
  void run();
  bool ensure_params(uint32_t decode_type);

  struct Chunk {
    uint32_t decode_type;
    std::vector<int16_t> pcm;
  };

  std::string device_;
  snd_pcm_t* pcm_ = nullptr;
  // The PCM's currently-configured shape. Reconfigure (which clicks on HDMI) is
  // driven by these, not the raw decodeType, so equivalent types don't reopen.
  unsigned cur_rate_ = 0;
  unsigned cur_channels_ = 0;
  // Target buffer latency; larger absorbs more USB/scheduling jitter (fewer
  // underrun clicks) at the cost of audio delay. Override via env at
  // construction.
  unsigned latency_us_ = 120000;
  int xruns_ = 0;  // playback-thread underrun count (diagnostic)

  std::mutex m_;
  std::condition_variable cv_;
  std::deque<Chunk> q_;
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
