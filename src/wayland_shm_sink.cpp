// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#include "wayland_shm_sink.h"

#include <linux/memfd.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>

extern "C" {
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

// wcs include order is load-bearing: generated bindings first, then the wl/*
// helpers. clang-format must not sort this block.
// clang-format off
#include "wayland_client.hpp"

#include <wl/proxy_impl.hpp>
#include <wl/wl_ptr.hpp>
// clang-format on

namespace ck {

namespace {

// WL_SHM_FORMAT_XRGB8888 — mandatory in wl_shm. In memory (LE) the bytes are
// B,G,R,X, which is libswscale's AV_PIX_FMT_BGR0.
constexpr uint32_t kFormatXrgb8888 = 1;

class ShmBufferHandler : public wayland::client::CWlBuffer<ShmBufferHandler> {
 public:
  bool released_ = true;
  std::function<void()>* on_release_ = nullptr;
  void OnRelease() override {
    released_ = true;
    if (on_release_ != nullptr && *on_release_)
      (*on_release_)();
  }
};

class ShmPoolHandler : public wayland::client::CWlShmPool<ShmPoolHandler> {};

}  // namespace

struct WlShmSink::Impl {
  std::function<wl_proxy*(int, int32_t)> make_pool;
  std::function<void()> on_release;

  int fd = -1;
  uint8_t* map = nullptr;
  size_t map_size = 0;
  wl::WlPtr<ShmPoolHandler> pool;
  bool have_pool = false;
  uint32_t w = 0, h = 0, stride = 0;

  static constexpr int kBufs = 3;
  struct Slot {
    wl::WlPtr<ShmBufferHandler> buffer;
    size_t offset = 0;
    bool active = false;
  } slots[kBufs];

  SwsContext* sws = nullptr;

  void teardown();
  bool ensure_pool(uint32_t nw, uint32_t nh);
};

void WlShmSink::Impl::teardown() {
  for (Slot& s : slots) {
    s.buffer.Reset();
    s.active = false;
    s.offset = 0;
  }
  pool.Reset();
  have_pool = false;
  if (map != nullptr) {
    munmap(map, map_size);
    map = nullptr;
    map_size = 0;
  }
  if (fd >= 0) {
    close(fd);
    fd = -1;
  }
  w = h = stride = 0;
}

bool WlShmSink::Impl::ensure_pool(uint32_t nw, uint32_t nh) {
  if (have_pool && nw == w && nh == h)
    return true;
  teardown();

  const uint32_t nstride = nw * 4;  // XRGB8888
  const size_t buf_size = static_cast<size_t>(nstride) * nh;
  const size_t total = buf_size * kBufs;

  fd = memfd_create("carlinkit-wl-shm", MFD_CLOEXEC);
  if (fd < 0 || ftruncate(fd, static_cast<off_t>(total)) != 0) {
    std::fprintf(stderr, "shm-sink: memfd/ftruncate failed\n");
    teardown();
    return false;
  }
  map = static_cast<uint8_t*>(
      mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
  if (map == MAP_FAILED) {
    map = nullptr;
    std::fprintf(stderr, "shm-sink: mmap failed\n");
    teardown();
    return false;
  }
  map_size = total;

  wl_proxy* pool_raw = make_pool(fd, static_cast<int32_t>(total));
  if (pool_raw == nullptr) {
    std::fprintf(stderr, "shm-sink: create_pool failed\n");
    teardown();
    return false;
  }
  pool.Attach(pool_raw);

  using PoolTraits = wayland::client::wl_shm_pool_traits;
  for (int i = 0; i < kBufs; ++i) {
    const size_t offset = buf_size * i;
    wl_proxy* buf_raw = wl::construct<wayland::client::wl_buffer_traits,
                                      PoolTraits::Op::CreateBuffer>(
        *pool.Get(), static_cast<int32_t>(offset), static_cast<int32_t>(nw),
        static_cast<int32_t>(nh), static_cast<int32_t>(nstride),
        kFormatXrgb8888);
    if (buf_raw == nullptr) {
      std::fprintf(stderr, "shm-sink: create_buffer failed\n");
      teardown();
      return false;
    }
    slots[i].buffer.Get()->on_release_ = &on_release;
    slots[i].buffer.Get()->released_ = true;
    slots[i].buffer.Get()->_SetProxy(buf_raw);
    slots[i].offset = offset;
    slots[i].active = true;
  }

  w = nw;
  h = nh;
  stride = nstride;
  have_pool = true;
  return true;
}

WlShmSink::WlShmSink(std::function<wl_proxy*(int, int32_t)> make_pool)
    : impl_(std::make_unique<Impl>()) {
  impl_->make_pool = std::move(make_pool);
}

WlShmSink::~WlShmSink() {
  impl_->teardown();
  if (impl_->sws != nullptr)
    sws_freeContext(impl_->sws);
}

void WlShmSink::set_on_release(std::function<void()> cb) {
  impl_->on_release = std::move(cb);
}

wl_proxy* WlShmSink::buffer_for(const DecodedFrame& frame) {
  if (frame.num_planes < 3 || frame.width == 0 || frame.height == 0)
    return nullptr;
  if (!impl_->ensure_pool(frame.width, frame.height))
    return nullptr;

  int slot = -1;
  for (int i = 0; i < Impl::kBufs; ++i)
    if (impl_->slots[i].active && impl_->slots[i].buffer.Get()->released_) {
      slot = i;
      break;
    }
  if (slot < 0)
    return nullptr;  // every buffer still held by the compositor

  impl_->sws = sws_getCachedContext(
      impl_->sws, static_cast<int>(impl_->w), static_cast<int>(impl_->h),
      AV_PIX_FMT_YUV420P, static_cast<int>(impl_->w),
      static_cast<int>(impl_->h), AV_PIX_FMT_BGR0, SWS_POINT, nullptr, nullptr,
      nullptr);
  if (impl_->sws == nullptr)
    return nullptr;

  const uint8_t* src[4] = {frame.planes[0].data, frame.planes[1].data,
                           frame.planes[2].data, nullptr};
  const int src_stride[4] = {static_cast<int>(frame.planes[0].stride),
                             static_cast<int>(frame.planes[1].stride),
                             static_cast<int>(frame.planes[2].stride), 0};
  uint8_t* dst[4] = {impl_->map + impl_->slots[slot].offset, nullptr, nullptr,
                     nullptr};
  const int dst_stride[4] = {static_cast<int>(impl_->stride), 0, 0, 0};
  sws_scale(impl_->sws, src, src_stride, 0, static_cast<int>(impl_->h), dst,
            dst_stride);

  impl_->slots[slot].buffer.Get()->released_ = false;
  return impl_->slots[slot].buffer.Get()->GetProxy();
}

}  // namespace ck
