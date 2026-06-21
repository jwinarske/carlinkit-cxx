// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#pragma once
// Reassembles the dongle's [16-byte header][payload] message stream from
// arbitrary bulk-transfer chunk boundaries. Zero-copy fast path: when a whole
// message is contained in one fed chunk, the payload is dispatched by pointer
// straight from the source buffer with no copy. Messages that straddle two
// chunks are accumulated into an internal buffer (one copy) and then
// dispatched.
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

#include "protocol.h"

namespace ck {

// Upper bound on a single message's payload. The wire `length` is only
// validated by the magic + type-check, so a desynced/garbage header could
// otherwise request a multi-gigabyte allocation. No real message (an H.264
// keyframe at most) approaches this; anything larger is treated as a bad header
// and resynced.
constexpr size_t kMaxMessage = 16 * 1024 * 1024;

class StreamParser {
 public:
  // Dispatched for each complete message. `payload` may point into the fed
  // buffer (zero-copy) or into the internal assembly buffer; it is valid only
  // for the duration of the call.
  using Handler =
      std::function<void(const Header&, const uint8_t* payload, size_t len)>;

  explicit StreamParser(Handler h) : handler_(std::move(h)) {}

  void reset() {
    have_header_ = false;
    hdr_have_ = 0;
    payload_have_ = 0;
    payload_need_ = 0;
  }

  // Feed one received chunk. May produce zero or more dispatched messages.
  void feed(const uint8_t* buf, size_t len) {
    size_t pos = 0;
    while (pos < len) {
      if (assembling_) {
        pos += assemble(buf + pos, len - pos);
        continue;
      }
      // Fast path: need a full 16-byte header contiguously to peek.
      if (len - pos < kHeaderLen) {
        std::memcpy(hdr_, buf + pos, len - pos);
        hdr_have_ = len - pos;
        have_header_ = false;
        assembling_ = true;
        return;
      }
      Header h;
      if (!Header::parse(buf + pos, h) || h.length > kMaxMessage) {
        ++pos;  // bad/oversized header: resync, scanning forward for the magic
        continue;
      }
      size_t avail = len - pos - kHeaderLen;
      if (avail >= h.length) {
        handler_(h, buf + pos + kHeaderLen, h.length);  // zero-copy dispatch
        pos += kHeaderLen + h.length;
      } else {
        // Payload straddles this chunk boundary: start assembling.
        cur_ = h;
        payload_.resize(h.length);
        std::memcpy(payload_.data(), buf + pos + kHeaderLen, avail);
        payload_have_ = avail;
        payload_need_ = h.length;
        have_header_ = true;
        assembling_ = true;
        return;
      }
    }
  }

 private:
  // Consume bytes while completing a carried-over header and/or payload.
  // Returns bytes consumed from [in, in+len).
  size_t assemble(const uint8_t* in, size_t len) {
    size_t pos = 0;
    if (!have_header_) {
      size_t need = kHeaderLen - hdr_have_;
      size_t n = need < len ? need : len;
      std::memcpy(hdr_ + hdr_have_, in, n);
      hdr_have_ += n;
      pos += n;
      if (hdr_have_ < kHeaderLen)
        return pos;
      if (!Header::parse(hdr_, cur_) || cur_.length > kMaxMessage) {
        // Bad/oversized header: drop the first byte and keep scanning.
        std::memmove(hdr_, hdr_ + 1, kHeaderLen - 1);
        hdr_have_ = kHeaderLen - 1;
        return pos;
      }
      have_header_ = true;
      payload_need_ = cur_.length;
      payload_have_ = 0;
      payload_.resize(payload_need_);
      if (payload_need_ == 0) {
        handler_(cur_, nullptr, 0);
        finish_message();
        return pos;
      }
    }
    // Filling payload.
    size_t need = payload_need_ - payload_have_;
    size_t n = need < (len - pos) ? need : (len - pos);
    std::memcpy(payload_.data() + payload_have_, in + pos, n);
    payload_have_ += n;
    pos += n;
    if (payload_have_ == payload_need_) {
      handler_(cur_, payload_.data(), payload_need_);
      finish_message();
    }
    return pos;
  }

  void finish_message() {
    assembling_ = false;
    have_header_ = false;
    hdr_have_ = 0;
    payload_have_ = 0;
    payload_need_ = 0;
  }

  Handler handler_;
  bool assembling_ = false;
  bool have_header_ = false;
  uint8_t hdr_[kHeaderLen];
  size_t hdr_have_ = 0;
  Header cur_{};
  std::vector<uint8_t> payload_;
  size_t payload_have_ = 0;
  size_t payload_need_ = 0;
};

}  // namespace ck
