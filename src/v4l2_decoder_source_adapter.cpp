// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#include "v4l2_decoder_source_adapter.h"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/scene/v4l2_decoder_source.hpp>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <system_error>
#include <utility>

namespace ck {
namespace {

// True if `path` is a V4L2 memory-to-memory device whose OUTPUT (coded) side
// advertises H.264 — i.e. a stateful H.264 decoder.
bool is_h264_decoder(const char* path) {
  int fd = open(path, O_RDWR | O_CLOEXEC);
  if (fd < 0)
    return false;
  bool ok = false;
  v4l2_capability cap{};
  if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
    const uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) != 0U
                              ? cap.device_caps
                              : cap.capabilities;
    const bool mplane = (caps & V4L2_CAP_VIDEO_M2M_MPLANE) != 0U;
    if (mplane || (caps & V4L2_CAP_VIDEO_M2M) != 0U) {
      // Normally terminated by VIDIOC_ENUM_FMT failing past the last format;
      // the index cap guards against a buggy driver that never reports EINVAL.
      for (uint32_t i = 0; i < 256; ++i) {
        v4l2_fmtdesc desc{};
        desc.index = i;
        desc.type = mplane ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
                           : V4L2_BUF_TYPE_VIDEO_OUTPUT;
        if (ioctl(fd, VIDIOC_ENUM_FMT, &desc) != 0)
          break;
        if (desc.pixelformat == V4L2_PIX_FMT_H264) {
          ok = true;
          break;
        }
      }
    }
  }
  close(fd);
  return ok;
}

// CARLINKIT_V4L2_DEV if set, otherwise the first /dev/videoN that is an H.264
// stateful decoder. Empty string if none.
std::string find_h264_decoder() {
  if (const char* env = std::getenv("CARLINKIT_V4L2_DEV");
      env != nullptr && *env != '\0')
    return env;
  char path[32];
  for (int i = 0; i < 64; ++i) {
    std::snprintf(path, sizeof path, "/dev/video%d", i);
    if (is_h264_decoder(path))
      return path;
  }
  return {};
}

}  // namespace

std::unique_ptr<V4l2DecoderSourceAdapter> V4l2DecoderSourceAdapter::create(
    drm::Device& dev,
    uint32_t coded_w,
    uint32_t coded_h) {
  const std::string path = find_h264_decoder();
  if (path.empty()) {
    std::fprintf(stderr, "v4l2: no H.264 stateful decoder found\n");
    return nullptr;
  }

  drm::scene::V4l2DecoderConfig cfg;
  cfg.codec_fourcc = V4L2_PIX_FMT_H264;
  cfg.capture_fourcc = V4L2_PIX_FMT_NV12;
  cfg.coded_width = coded_w;
  cfg.coded_height = coded_h;
  cfg.output_buffer_count = 4;
  cfg.capture_buffer_count = 4;
  // A compressed frame is far smaller than the raw frame; w*h bytes is a safe
  // upper bound for an OUTPUT buffer (the dongle's coded chunks are per-frame).
  cfg.output_buffer_size = static_cast<size_t>(coded_w) * coded_h;

  auto r = drm::scene::V4l2DecoderSource::create(dev, path.c_str(), cfg);
  if (!r) {
    std::fprintf(stderr, "v4l2: create(%s) failed: %s\n", path.c_str(),
                 r.error().message().c_str());
    return nullptr;
  }
  auto adapter = std::unique_ptr<V4l2DecoderSourceAdapter>(
      new V4l2DecoderSourceAdapter(std::move(*r)));
  if (!adapter->start())
    return nullptr;
  std::fprintf(stderr, "v4l2: using %s\n", path.c_str());
  return adapter;
}

V4l2DecoderSourceAdapter::V4l2DecoderSourceAdapter(
    std::unique_ptr<drm::scene::V4l2DecoderSource> inner)
    : inner_(std::move(inner)) {}

V4l2DecoderSourceAdapter::~V4l2DecoderSourceAdapter() {
  running_ = false;
  if (wake_w_ >= 0) {
    const char c = 1;
    (void)!write(wake_w_, &c, 1);  // nudge the pump out of poll()
  }
  if (thread_.joinable())
    thread_.join();
  if (wake_r_ >= 0)
    close(wake_r_);
  if (wake_w_ >= 0)
    close(wake_w_);
}

bool V4l2DecoderSourceAdapter::start() {
  int p[2] = {-1, -1};
  if (pipe2(p, O_NONBLOCK | O_CLOEXEC) != 0) {
    std::fprintf(stderr, "v4l2: pipe2 failed\n");
    return false;
  }
  wake_r_ = p[0];
  wake_w_ = p[1];
  inner_fd_ = inner_->fd();
  running_ = true;
  thread_ = std::thread([this] { pump(); });
  return true;
}

void V4l2DecoderSourceAdapter::submit_bitstream(const uint8_t* data,
                                                size_t len,
                                                uint64_t /*pts_ns*/) {
  if (failed_ || len == 0)
    return;
  {
    std::lock_guard<std::mutex> lk(q_m_);
    if (queue_.size() >= kMaxQueue)
      queue_.pop_front();  // decoder is behind; drop the oldest coded chunk
    queue_.emplace_back(data, data + len);
  }
  // Nudge the pump. A failed write here is harmless: a full pipe means a wake
  // is already pending (poll will fire), and even with no wake at all the pump
  // re-checks the queue every poll timeout, so the chunk is picked up
  // regardless.
  const char c = 1;
  (void)!write(wake_w_, &c, 1);
}

void V4l2DecoderSourceAdapter::pump() {
  std::vector<uint8_t> cur;  // chunk held across EAGAIN until it submits
  bool have_cur = false;
  while (running_) {
    pollfd pfds[2];
    pfds[0].fd = inner_fd_;
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;
    pfds[1].fd = wake_r_;
    pfds[1].events = POLLIN;
    pfds[1].revents = 0;
    // 50 ms cap so a full OUTPUT queue (EAGAIN) is retried after drive() frees
    // a buffer even if fd() doesn't signal, and so running_ is rechecked.
    const int pr = poll(pfds, 2, 50);
    if (pr < 0 && errno != EINTR) {
      // A persistent poll error (e.g. a bad fd) would otherwise busy-spin;
      // treat it as fatal rather than burn a core.
      std::fprintf(stderr, "v4l2: poll failed: %s\n", std::strerror(errno));
      failed_ = true;
      return;
    }
    if ((pfds[1].revents & POLLIN) != 0) {
      char buf[64];
      while (read(wake_r_, buf, sizeof buf) > 0) {
      }
    }
    if (!running_)
      break;

    {
      std::lock_guard<std::mutex> lk(inner_m_);
      auto r = inner_->drive();
      if (!r) {
        std::fprintf(stderr, "v4l2: drive failed: %s\n",
                     r.error().message().c_str());
        failed_ = true;
        return;
      }
    }

    // Feed as many queued chunks as the OUTPUT queue will take.
    while (running_) {
      if (!have_cur) {
        std::lock_guard<std::mutex> lk(q_m_);
        if (queue_.empty())
          break;
        cur = std::move(queue_.front());
        queue_.pop_front();
        have_cur = true;
      }
      bool keep_chunk = false;
      {
        std::lock_guard<std::mutex> lk(inner_m_);
        auto r = inner_->submit_bitstream(
            drm::span<const uint8_t>(cur.data(), cur.size()), 0);
        if (!r) {
          if (r.error() ==
              std::make_error_code(std::errc::resource_unavailable_try_again))
            keep_chunk = true;  // OUTPUT full; retry after the next drive()
          else
            std::fprintf(stderr, "v4l2: submit_bitstream: %s\n",
                         r.error().message().c_str());  // drop this chunk
        }
      }
      if (keep_chunk)
        break;
      have_cur = false;
    }
  }
}

drm::expected<drm::scene::AcquiredBuffer, std::error_code>
V4l2DecoderSourceAdapter::acquire() {
  std::lock_guard<std::mutex> lk(inner_m_);
  return inner_->acquire();
}

void V4l2DecoderSourceAdapter::release(
    drm::scene::AcquiredBuffer acquired) noexcept {
  std::lock_guard<std::mutex> lk(inner_m_);
  inner_->release(std::move(acquired));
}

drm::scene::SourceFormat V4l2DecoderSourceAdapter::format() const noexcept {
  std::lock_guard<std::mutex> lk(inner_m_);
  return inner_->format();
}

}  // namespace ck
