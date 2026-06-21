// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#pragma once
// Supervises the (often flaky) Carlinkit dongle: connects when it appears,
// tears the session down cleanly on surprise removal, and reconnects — all on a
// background thread. The app's sink is forwarded to each session; sends route
// to the current session (no-op while disconnected). The app's video/audio
// sinks persist across reconnects, so the last frame stays on screen until
// video resumes.
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "dongle.h"
#include "protocol.h"
#include "usb_device.h"

namespace ck {

class DongleManager {
 public:
  DongleManager(DongleConfig cfg, DongleSink sink)
      : cfg_(std::move(cfg)), sink_(std::move(sink)) {}
  ~DongleManager() { stop(); }

  DongleManager(const DongleManager&) = delete;
  DongleManager& operator=(const DongleManager&) = delete;

  void set_on_connected(std::function<void()> cb) {
    on_connected_ = std::move(cb);
  }
  void set_on_disconnected(std::function<void()> cb) {
    on_disconnected_ = std::move(cb);
  }

  void start();
  void stop();

  // Thread-safe sends; return false while disconnected.
  bool send(std::vector<uint8_t> frame);
  bool send_touch(float x, float y, TouchAction a) {
    return send(ck::send_touch(x, y, a));
  }
  bool send_command(Command c) { return send(ck::send_command(c)); }

  [[nodiscard]] bool connected() const { return connected_; }

 private:
  void supervise();

  DongleConfig cfg_;
  DongleSink sink_;
  std::function<void()> on_connected_;
  std::function<void()> on_disconnected_;

  // One libusb context for the whole supervisor lifetime, reused for every
  // poll/connect so probing doesn't init/exit a context twice a second.
  libusb_context* probe_ctx_ = nullptr;

  std::mutex mu_;  // guards usb_/dongle_ vs send()
  std::unique_ptr<UsbDevice> usb_;
  std::unique_ptr<Dongle> dongle_;

  std::thread sup_;
  std::atomic<bool> running_{false};
  std::atomic<bool> connected_{false};
};

}  // namespace ck
