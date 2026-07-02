// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#include "dongle_manager.h"

#include <libusb-1.0/libusb.h>

#include <algorithm>
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
  // Reconnect pacing. No delay while healthy or while the dongle is simply
  // absent (fast plug detection), but back off exponentially when it is present
  // yet failing -- claim keeps failing, or a session flaps (connects then dies
  // within kFlapWindow) -- so a wedged dongle doesn't thrash. After a few claim
  // failures, reset the device to shake it out of a stuck state; it
  // re-enumerates, so we drop the handle and let the next poll re-open it.
  constexpr auto kPollAbsent = milliseconds(500);  // steady poll while gone
  constexpr auto kBackoffMin = milliseconds(250);
  constexpr auto kBackoffMax = milliseconds(5000);
  constexpr auto kFlapWindow = seconds(3);
  constexpr int kResetAfterClaimFails = 3;

  auto backoff = milliseconds(0);
  int claim_fails = 0;
  const auto nap = [this](milliseconds d) {  // interruptible sleep
    const auto end = steady_clock::now() + d;
    while (running_ && steady_clock::now() < end)
      std::this_thread::sleep_for(milliseconds(50));
  };
  const auto grow_backoff = [&backoff, kBackoffMin, kBackoffMax] {
    backoff =
        backoff.count() == 0 ? kBackoffMin : std::min(kBackoffMax, backoff * 2);
  };

  while (running_) {
    if (!connected_) {
      if (backoff.count() > 0)
        nap(backoff);
      if (!running_)
        break;
      // Poll for the dongle (re)appearing (quiet -- no spam).
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
        connected_at_ = steady_clock::now();
        claim_fails = 0;
        backoff = milliseconds(0);
        std::fprintf(stderr, "[dongle] connected\n");
        if (on_connected_)
          on_connected_();
      } else if (usb) {
        // Present but not claimable -- a stuck state. Back off, and after a few
        // tries reset the device to force a clean re-enumeration.
        if (++claim_fails >= kResetAfterClaimFails) {
          std::fprintf(stderr,
                       "[dongle] claim failing x%d; resetting the device\n",
                       claim_fails);
          usb->reset();  // handle invalidated; usb dropped at scope end
          claim_fails = 0;
        }
        grow_backoff();
      } else {
        // Absent: fast steady poll for a fresh plug, and a clean slate.
        backoff = milliseconds(0);
        claim_fails = 0;
        nap(kPollAbsent);
      }
    } else {
      // Connected: watch for surprise removal (transfers hit NO_DEVICE).
      bool dead;
      {
        std::lock_guard<std::mutex> lk(mu_);
        dead = !dongle_ || dongle_->failed();
      }
      if (dead) {
        const bool flap = steady_clock::now() - connected_at_ < kFlapWindow;
        {
          std::lock_guard<std::mutex> lk(mu_);
          if (dongle_) {
            dongle_->stop();
            dongle_.reset();
          }
          usb_.reset();
        }
        connected_ = false;
        if (flap) {
          grow_backoff();  // died almost immediately -- back off before retry
          std::fprintf(stderr,
                       "[dongle] disconnected (flap; backing off %lldms)\n",
                       static_cast<long long>(backoff.count()));
        } else {
          backoff = milliseconds(0);  // a good session -- clean slate
          std::fprintf(stderr, "[dongle] disconnected\n");
        }
        if (on_disconnected_)
          on_disconnected_();
      } else {
        std::this_thread::sleep_for(milliseconds(200));
      }
    }
  }
}

}  // namespace ck
