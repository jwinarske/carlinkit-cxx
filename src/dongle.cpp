// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#include "dongle.h"

#include <libusb-1.0/libusb.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace ck {

namespace {
uint64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
      .count();
}
}  // namespace

Dongle::~Dongle() {
  stop();
}

// ── TX: fire-and-forget async bulk OUT ──────────────────────────────────────
void Dongle::tx_complete(libusb_transfer* t) {
  auto* self = static_cast<Dongle*>(t->user_data);
  std::free(t->buffer);
  libusb_free_transfer(t);
  self->tx_inflight_--;
}

bool Dongle::send(std::vector<uint8_t> frame) {
  if (!running_)
    return false;
  libusb_transfer* t = libusb_alloc_transfer(0);
  if (!t)
    return false;
  auto* buf = static_cast<uint8_t*>(std::malloc(frame.size()));
  if (!buf) {
    libusb_free_transfer(t);
    return false;
  }
  std::memcpy(buf, frame.data(), frame.size());
  libusb_fill_bulk_transfer(t, usb_.handle(), usb_.ep_out(), buf,
                            static_cast<int>(frame.size()),
                            &Dongle::tx_complete, this, 1000);
  std::lock_guard<std::mutex> lk(tx_mutex_);
  tx_inflight_++;
  if (int r = libusb_submit_transfer(t); r != 0) {
    std::fprintf(stderr, "tx submit failed: %s\n", libusb_error_name(r));
    if (r == LIBUSB_ERROR_NO_DEVICE)
      failed_ = true;  // surprise removal: signal the supervisor to reconnect
    std::free(buf);
    libusb_free_transfer(t);
    tx_inflight_--;
    return false;
  }
  return true;
}

// ── RX: ring of in-flight DMA bulk-IN transfers ─────────────────────────────
void Dongle::rx_complete(libusb_transfer* t) {
  auto* self = static_cast<Dongle*>(t->user_data);
  if (t->status == LIBUSB_TRANSFER_COMPLETED) {
    if (t->actual_length > 0)
      self->parser_.feed(t->buffer, static_cast<size_t>(t->actual_length));
  } else if (t->status != LIBUSB_TRANSFER_TIMED_OUT) {
    // Canceled / no-device / stall: stop streaming, drop this transfer.
    // NO_DEVICE (surprise removal) is expected and handled by the supervisor.
    if (self->running_ && t->status != LIBUSB_TRANSFER_CANCELLED &&
        t->status != LIBUSB_TRANSFER_NO_DEVICE)
      std::fprintf(stderr, "rx transfer status %d\n", t->status);
    if (t->status == LIBUSB_TRANSFER_NO_DEVICE)
      self->failed_ = true;  // surprise removal
    self->rx_inflight_--;
    return;
  }
  if (!self->running_) {
    self->rx_inflight_--;
    return;
  }
  if (int r = libusb_submit_transfer(t); r != 0) {
    std::fprintf(stderr, "rx resubmit failed: %s\n", libusb_error_name(r));
    if (r == LIBUSB_ERROR_NO_DEVICE)
      self->failed_ = true;
    self->rx_inflight_--;
  }
}

bool Dongle::start() {
  running_ = true;

  // Allocate and submit the RX ring before sending anything, so no early
  // dongle responses are missed.
  rx_.reserve(rx_count_);
  rx_bufs_.reserve(rx_count_);
  for (int i = 0; i < rx_count_; ++i) {
    uint8_t* buf = usb_.alloc_dma(rx_size_);
    libusb_transfer* t = libusb_alloc_transfer(0);
    if (!buf || !t) {
      std::fprintf(stderr, "failed to allocate RX transfer %d\n", i);
      running_ = false;
      return false;
    }
    libusb_fill_bulk_transfer(t, usb_.handle(), usb_.ep_in(), buf,
                              static_cast<int>(rx_size_), &Dongle::rx_complete,
                              this, 0);
    rx_bufs_.push_back(buf);
    rx_.push_back(t);
  }
  for (libusb_transfer* t : rx_) {
    if (int r = libusb_submit_transfer(t); r != 0) {
      std::fprintf(stderr, "initial rx submit failed: %s\n",
                   libusb_error_name(r));
      running_ = false;
      return false;
    }
    rx_inflight_++;
  }

  events_ = std::thread([this] { event_loop(); });

  // Init sequence (order mirrors node-CarPlay DongleDriver.start).
  send(ck::send_number(file::kDpi, cfg_.dpi));
  send(ck::send_open(cfg_));
  send(ck::send_number(file::kNightMode, cfg_.nightMode ? 1 : 0));
  send(ck::send_number(file::kHandDrive, cfg_.hand));
  send(ck::send_number(file::kChargeMode, 1));
  send(ck::send_string(file::kBoxName, cfg_.boxName));
  send(ck::send_box_settings(cfg_, now_ms()));
  send(ck::send_command(Command::WifiEnable));
  send(ck::send_command(cfg_.wifi5g ? Command::Wifi5g : Command::Wifi24g));
  send(ck::send_command(cfg_.boxMic ? Command::BoxMic : Command::Mic));
  send(ck::send_command(cfg_.audioTransferMode ? Command::AudioTransferOn
                                               : Command::AudioTransferOff));

  heart_ = std::thread([this] { heartbeat_loop(); });

  std::this_thread::sleep_for(std::chrono::seconds(1));
  send(ck::send_command(Command::WifiConnect));
  return true;
}

void Dongle::stop() {
  if (!running_.exchange(false))
    return;
  if (heart_.joinable())
    heart_.join();

  // Cancel outstanding RX transfers, then join the event thread. Its loop runs
  // while rx_inflight_/tx_inflight_ > 0, so by the time the join returns every
  // transfer's completion callback has fired — no transfer is still in flight,
  // so freeing them below cannot race a callback (no fixed-timeout guess).
  for (libusb_transfer* t : rx_)
    libusb_cancel_transfer(t);
  if (events_.joinable())
    events_.join();

  for (size_t i = 0; i < rx_.size(); ++i) {
    if (rx_[i])
      libusb_free_transfer(rx_[i]);
    if (rx_bufs_[i])
      usb_.free_dma(rx_bufs_[i], rx_size_);
  }
  rx_.clear();
  rx_bufs_.clear();
}

void Dongle::event_loop() {
  // Sole event handler for the context: services both RX and TX completions.
  struct timeval tv{0, 100000};  // 100ms
  while (running_ || rx_inflight_ > 0 || tx_inflight_ > 0) {
    libusb_handle_events_timeout_completed(usb_.context(), &tv, nullptr);
  }
}

void Dongle::heartbeat_loop() {
  while (running_) {
    send(ck::heartbeat());
    for (int i = 0; i < 20 && running_; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

void Dongle::dispatch(const Header& h, const uint8_t* p, size_t len) const {
  switch (h.type) {
    case MessageType::VideoData: {
      if (len < 20)
        return;
      VideoFrame vf{get_u32(p),      get_u32(p + 4),  get_u32(p + 8),
                    get_u32(p + 12), get_u32(p + 16), p + 20,
                    len - 20};
      if (sink_.on_video)
        sink_.on_video(vf);
      break;
    }
    case MessageType::AudioData: {
      if (len < 12)
        return;
      AudioFrame af{};
      af.decodeType = get_u32(p);
      af.volume = get_f32(p + 4);
      af.audioType = get_u32(p + 8);
      af.command = -1;
      size_t rest = len - 12;
      if (rest == 1) {
        af.command =
            static_cast<int>(p[12]);  // AudioCommand 1..13 (unsigned byte)
      } else if (rest == 4) {
        af.volumeDuration =
            get_f32(p + 12);  // duck: ramp to `volume` over this
        af.hasVolumeDuration = true;
      } else if (rest >= 2) {
        af.pcm = reinterpret_cast<const int16_t*>(p + 12);
        af.samples = rest / 2;
      }
      // CARLINKIT_AUDIO_TRACE=1: log every AudioData with a timestamp so we can
      // see whether distinct (decodeType,audioType) streams overlap in time
      // (concurrent → mixing needed) or are serialized (gain-ramp ducking
      // only).
      static const bool atrace =
          std::getenv("CARLINKIT_AUDIO_TRACE") != nullptr;
      if (atrace) {
        const auto t = static_cast<unsigned long long>(now_ms());
        if (rest == 1)
          std::fprintf(stderr, "[atrace] t=%llu dt=%u at=%u cmd=%d\n", t,
                       af.decodeType, af.audioType, af.command);
        else if (rest == 4)
          std::fprintf(stderr,
                       "[atrace] t=%llu dt=%u at=%u dur=%.3fs vol=%.3f\n", t,
                       af.decodeType, af.audioType, get_f32(p + 12), af.volume);
        else
          std::fprintf(stderr, "[atrace] t=%llu dt=%u at=%u pcm=%zu vol=%.3f\n",
                       t, af.decodeType, af.audioType, af.samples, af.volume);
      }
      if (sink_.on_audio)
        sink_.on_audio(af);
      break;
    }
    case MessageType::Plugged: {
      if (len < 4)
        return;
      auto pt = static_cast<PhoneType>(get_u32(p));
      bool wifi = (len == 8) && get_u32(p + 4);
      if (sink_.on_plugged)
        sink_.on_plugged(pt, wifi);
      break;
    }
    case MessageType::Unplugged:
      if (sink_.on_unplugged)
        sink_.on_unplugged();
      break;
    case MessageType::Command:
      if (len >= 4 && sink_.on_command)
        sink_.on_command(static_cast<Command>(get_u32(p)));
      break;
    default:
      if (sink_.on_other)
        sink_.on_other(h.type, p, len);
      break;
  }
}

}  // namespace ck
