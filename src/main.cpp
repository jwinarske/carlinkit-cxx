// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
// carlinkit-probe: headless verification. Open the Carlinkit dongle, run the
// init handshake, and dump received H.264 to out.h264 and PCM to out.pcm while
// logging session events. Confirms the dongle streams; the drm-cxx zero-copy
// video sink lives in carlinkit-kms.
//
//   ./carlinkit-probe            # connect, stream until Ctrl-C
//   ffplay -f h264 out.h264      # play back the captured video
#include <atomic>
#include <csignal>
#include <cstdio>

#include "dongle.h"
#include "protocol.h"
#include "usb_device.h"

namespace {
std::atomic<bool> g_stop{false};
void on_sigint(int) {
  g_stop = true;
}

const char* phone_name(ck::PhoneType t) {
  switch (t) {
    case ck::PhoneType::CarPlay:
      return "CarPlay";
    case ck::PhoneType::AndroidAuto:
      return "AndroidAuto";
    case ck::PhoneType::iPhoneMirror:
      return "iPhoneMirror";
    case ck::PhoneType::AndroidMirror:
      return "AndroidMirror";
    case ck::PhoneType::HiCar:
      return "HiCar";
  }
  return "?";
}
}  // namespace

int main() {
  std::signal(SIGINT, on_sigint);

  auto usb = ck::UsbDevice::open();
  if (!usb)
    return 1;
  if (!usb->claim())
    return 1;

  FILE* vout = std::fopen("out.h264", "wb");
  FILE* aout = std::fopen("out.pcm", "wb");
  std::atomic<uint64_t> vbytes{0}, abytes{0}, vframes{0};

  ck::DongleConfig cfg;  // defaults: 800x640@20, dpi160, format5
  ck::Dongle dongle(*usb, cfg);

  ck::DongleSink sink;
  sink.on_video = [&](const ck::VideoFrame& f) {
    if (vout)
      std::fwrite(f.data, 1, f.size, vout);
    vbytes += f.size;
    if (++vframes % 60 == 0)
      std::fprintf(stderr, "\rvideo: %llu frames, %llu KB   ",
                   (unsigned long long)vframes,
                   (unsigned long long)(vbytes / 1024));
  };
  sink.on_audio = [&](const ck::AudioFrame& f) {
    if (f.pcm && aout)
      std::fwrite(f.pcm, sizeof(int16_t), f.samples, aout);
    if (f.pcm)
      abytes += f.samples * 2;
  };
  sink.on_plugged = [&](ck::PhoneType t, bool wifi) {
    std::fprintf(stderr, "\n[plugged] %s (wifi=%d)\n", phone_name(t), wifi);
  };
  sink.on_unplugged = [&] { std::fprintf(stderr, "\n[unplugged]\n"); };
  sink.on_command = [&](ck::Command c) {
    std::fprintf(stderr, "\n[command] %u\n", (unsigned)c);
  };
  sink.on_other = [&](ck::MessageType t, const uint8_t* p, size_t n) {
    // Surface text-ish info messages (BT/Wifi names, versions) for visibility.
    if (t == ck::MessageType::WifiDeviceName ||
        t == ck::MessageType::BluetoothDeviceName ||
        t == ck::MessageType::SoftwareVersion ||
        t == ck::MessageType::HiCarLink) {
      std::fprintf(stderr, "\n[%#x] %.*s\n", (unsigned)t, (int)n,
                   (const char*)p);
    }
  };
  dongle.set_sink(sink);

  std::fprintf(stderr,
               "starting dongle; pair your phone (wifi+BT on, nearby)...\n");
  if (!dongle.start())
    return 1;

  while (!g_stop) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  std::fprintf(stderr, "\nstopping...\n");
  dongle.stop();
  if (vout)
    std::fclose(vout);
  if (aout)
    std::fclose(aout);
  std::fprintf(stderr, "wrote out.h264 (%llu KB), out.pcm (%llu KB)\n",
               (unsigned long long)(vbytes / 1024),
               (unsigned long long)(abytes / 1024));
  return 0;
}
