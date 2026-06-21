// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
// carlinkit-audio — headless audio test (no display): connect the dongle, play
// its PCM through ALSA, and capture the mic on demand. Verifies the audio path
// over SSH (you'll hear CarPlay audio on the ALSA device once a phone pairs).
//
//   ./carlinkit-audio [alsa-playback-device] [alsa-capture-device]
#include <atomic>
#include <csignal>
#include <cstdio>
#include <thread>

#include "audio_alsa.h"
#include "dongle.h"
#include "protocol.h"
#include "usb_device.h"

namespace {
std::atomic<bool> g_stop{false};
void on_sigint(int) {
  g_stop = true;
}
}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, on_sigint);
  std::signal(SIGTERM, on_sigint);
  const char* play_dev = argc > 1 ? argv[1] : "default";
  const char* cap_dev = argc > 2 ? argv[2] : "default";

  auto usb = ck::UsbDevice::open();
  if (!usb || !usb->claim())
    return 1;

  ck::AudioOutput out(play_dev);
  out.start();

  ck::Dongle dongle(*usb);
  ck::AudioInput mic(cap_dev, [&](const int16_t* pcm, size_t n) {
    dongle.send(ck::send_audio(pcm, n));
  });

  uint64_t a_samples = 0;
  uint32_t a_frames = 0;
  ck::DongleSink sink;
  sink.on_audio = [&](const ck::AudioFrame& f) {
    if (f.pcm) {
      out.submit(f.decodeType, f.pcm, f.samples);
      a_samples += f.samples;
      if (++a_frames % 50 == 0)
        std::fprintf(stderr, "[audio] type=%u %llu samples played\n",
                     f.decodeType, static_cast<unsigned long long>(a_samples));
    } else if (f.command >= 0) {
      switch (static_cast<ck::AudioCommand>(f.command)) {
        case ck::AudioCommand::InputConfig:
        case ck::AudioCommand::PhonecallStart:
        case ck::AudioCommand::SiriStart:
          std::fprintf(stderr, "[audio] mic start (cmd %d)\n", f.command);
          mic.start();
          break;
        case ck::AudioCommand::PhonecallStop:
        case ck::AudioCommand::SiriStop:
          std::fprintf(stderr, "[audio] mic stop (cmd %d)\n", f.command);
          mic.stop();
          break;
        default:
          break;
      }
    }
  };
  sink.on_plugged = [&](ck::PhoneType t, bool wifi) {
    std::fprintf(stderr, "[plugged] type=%d wifi=%d\n", static_cast<int>(t),
                 wifi);
  };
  dongle.set_sink(sink);

  std::fprintf(
      stderr,
      "audio test running (play=%s cap=%s); pair phone, play audio...\n",
      play_dev, cap_dev);
  dongle.start();

  while (!g_stop)
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

  std::fprintf(stderr, "stopping\n");
  dongle.stop();
  mic.stop();
  out.stop();
  return 0;
}
