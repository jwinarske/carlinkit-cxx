// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#include "dongle_manager.h"

#include <libusb-1.0/libusb.h>

#include <chrono>
#include <cstdio>

namespace ck {

void DongleManager::start() {
  if (running_.exchange(true))
    return;
  if (libusb_init(&probe_ctx_) != 0) {
    std::fprintf(stderr, "[dongle] libusb_init failed; per-poll context\n");
    probe_ctx_ = nullptr;  // open() falls back to a private context
  }
  sup_ = std::thread([this] { supervise(); });
}

void DongleManager::stop() {
  if (!running_.exchange(false))
    return;
  if (sup_.joinable())
    sup_.join();
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (dongle_) {
      dongle_->stop();
      dongle_.reset();
    }
    usb_.reset();  // closes the handle but not probe_ctx_ (not owned by it)
    connected_ = false;
  }
  if (probe_ctx_) {
    libusb_exit(probe_ctx_);  // safe now: no device or event thread uses it
    probe_ctx_ = nullptr;
  }
}

bool DongleManager::send(std::vector<uint8_t> frame) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!dongle_)
    return false;
  return dongle_->send(std::move(frame));
}

void DongleManager::supervise() {
  using namespace std::chrono;
  while (running_) {
    if (!connected_) {
      // Disconnected: poll for the dongle (re)appearing (quiet — no spam).
      auto usb =
          UsbDevice::open(kDongleVid, kDonglePid, /*quiet=*/true, probe_ctx_);
      if (usb && usb->claim()) {
        auto d = std::make_unique<Dongle>(*usb, cfg_);
        d->set_sink(sink_);
        d->start();
        {
          std::lock_guard<std::mutex> lk(mu_);
          usb_ =
              std::move(usb);  // object stays put; Dongle's ref remains valid
          dongle_ = std::move(d);
        }
        connected_ = true;
        std::fprintf(stderr, "[dongle] connected\n");
        if (on_connected_)
          on_connected_();
      } else {
        for (int i = 0; i < 5 && running_; ++i)
          std::this_thread::sleep_for(milliseconds(100));
      }
    } else {
      // Connected: watch for surprise removal (transfers hit NO_DEVICE).
      bool dead;
      {
        std::lock_guard<std::mutex> lk(mu_);
        dead = !dongle_ || dongle_->failed();
      }
      if (dead) {
        {
          std::lock_guard<std::mutex> lk(mu_);
          if (dongle_) {
            dongle_->stop();
            dongle_.reset();
          }
          usb_.reset();
        }
        connected_ = false;
        std::fprintf(stderr, "[dongle] disconnected\n");
        if (on_disconnected_)
          on_disconnected_();
      } else {
        std::this_thread::sleep_for(milliseconds(200));
      }
    }
  }
}

}  // namespace ck
