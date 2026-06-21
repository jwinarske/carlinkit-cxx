// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "protocol.h"
#include "stream_parser.h"
#include "usb_device.h"

struct libusb_transfer;

namespace ck {

// Parsed video frame: H.264 Annex-B payload (a view valid only for the duration
// of the callback — submit to the decoder inline to keep the zero-copy path).
struct VideoFrame {
  uint32_t width, height, flags, length, unknown;
  const uint8_t* data;
  size_t size;
};

struct AudioFrame {
  uint32_t decodeType;
  float volume;
  uint32_t audioType;
  const int16_t* pcm;
  size_t samples;
  int command;  // >=0 when this is an AudioCommand control frame, else -1
};

struct DongleSink {
  std::function<void(const VideoFrame&)> on_video;
  std::function<void(const AudioFrame&)> on_audio;
  std::function<void(PhoneType, bool wifi)> on_plugged;
  std::function<void()> on_unplugged;
  std::function<void(Command)> on_command;
  std::function<void(MessageType, const uint8_t*, size_t)> on_other;
};

// Async USB driver: a ring of in-flight DMA bulk-IN transfers feeds a
// StreamParser; a single libusb event thread services all transfers. TX is
// fire-and-forget async so it never contends with the event loop.
class Dongle {
 public:
  // rx_buffers/rx_buf_size control the async ring; defaults suit 1080p H.264.
  Dongle(UsbDevice& usb,
         DongleConfig cfg = {},
         int rx_buffers = 8,
         size_t rx_buf_size = 131072)
      : usb_(usb),
        cfg_(cfg),
        rx_count_(rx_buffers),
        rx_size_(rx_buf_size),
        parser_([this](const Header& h, const uint8_t* p, size_t n) {
          dispatch(h, p, n);
        }) {}
  ~Dongle();

  void set_sink(DongleSink sink) { sink_ = std::move(sink); }

  bool start();
  void stop();

  // Thread-safe async sends usable from any thread.
  bool send(std::vector<uint8_t> frame);
  bool send_touch(float x, float y, TouchAction a) {
    return send(ck::send_touch(x, y, a));
  }
  bool send_command(Command c) { return send(ck::send_command(c)); }

  // True once a transfer hit LIBUSB_TRANSFER_NO_DEVICE (surprise removal). The
  // supervisor watches this to tear the session down and re-connect.
  [[nodiscard]] bool failed() const { return failed_; }

  const DongleConfig& config() const { return cfg_; }

 private:
  void event_loop();
  void heartbeat_loop();
  void dispatch(const Header& h, const uint8_t* payload, size_t len) const;

  // libusb async transfer callbacks (static trampolines).
  static void rx_complete(libusb_transfer* t);
  static void tx_complete(libusb_transfer* t);

  UsbDevice& usb_;
  DongleConfig cfg_;
  int rx_count_;
  size_t rx_size_;
  DongleSink sink_;
  StreamParser parser_;

  std::vector<libusb_transfer*> rx_;
  std::vector<uint8_t*> rx_bufs_;
  std::atomic<int> rx_inflight_{0};
  std::atomic<int> tx_inflight_{0};

  std::thread events_;
  std::thread heart_;
  std::mutex tx_mutex_;
  std::atomic<bool> running_{false};
  std::atomic<bool> failed_{false};
};

}  // namespace ck
