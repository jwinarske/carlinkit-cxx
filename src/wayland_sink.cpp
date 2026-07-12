// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#include "wayland_sink.h"

#include <algorithm>
#include <cstdio>
#include <unordered_map>

// wcs include order is load-bearing: generated client bindings first, then the
// wl/* helpers that define the interface tables and the feedback snapshot type.
// clang-format off
#include "wayland_client.hpp"
#include "linux_dmabuf_client.hpp"

#include <wl/linux_dmabuf.hpp>
#include <wl/dmabuf_feedback.hpp>
#include <wl/proxy_impl.hpp>
#include <wl/wl_ptr.hpp>
// clang-format on

namespace ck {

namespace {

// wl_buffer wrapper: tracks the compositor's release and forwards the slot to
// the sink's release callback so the decoder can recycle that pool buffer.
class WlBufferHandler : public wayland::client::CWlBuffer<WlBufferHandler> {
 public:
  bool released_ = true;
  uint32_t slot_ = 0;
  std::function<void(uint32_t)>* on_release_ = nullptr;

  void OnRelease() override {
    released_ = true;
    if (on_release_ != nullptr && *on_release_)
      (*on_release_)(slot_);
  }
};

// zwp_linux_buffer_params_v1 wrapper. Only used transiently to add planes and
// issue create_immed; its created/failed events are irrelevant to create_immed.
class ParamsHandler
    : public linux_dmabuf_unstable_v1::client::CZwpLinuxBufferParamsV1<
          ParamsHandler> {};

}  // namespace

struct WaylandSink::Impl {
  std::function<wl_proxy*()> make_params;
  uint32_t fourcc = 0;
  std::function<void(uint32_t)> on_release;
  // pool_slot -> imported wl_buffer, one per surface, reused on re-appearance.
  std::unordered_map<uint32_t, wl::WlPtr<WlBufferHandler>> cache;
};

WaylandSink::WaylandSink(std::function<wl_proxy*()> make_params,
                         uint32_t fourcc)
    : impl_(std::make_unique<Impl>()) {
  impl_->make_params = std::move(make_params);
  impl_->fourcc = fourcc;
}

WaylandSink::~WaylandSink() = default;

void WaylandSink::set_on_release(std::function<void(uint32_t)> cb) {
  impl_->on_release = std::move(cb);
}

void WaylandSink::clear() {
  impl_->cache.clear();
}

WaylandSink::Verdict WaylandSink::negotiate(const wl::FeedbackSnapshot& snap,
                                            uint32_t fourcc,
                                            uint64_t modifier) {
  Verdict v;
  v.presentable = snap.Supports(fourcc, modifier);
  const std::vector<uint64_t> scan = snap.ScanoutModifiersFor(fourcc);
  v.scanout = std::find(scan.begin(), scan.end(), modifier) != scan.end();
  return v;
}

wl_proxy* WaylandSink::buffer_for(const NativeFrame& frame) {
  if (!frame.valid())
    return nullptr;

  if (auto it = impl_->cache.find(frame.pool_slot); it != impl_->cache.end()) {
    // Re-attaching a buffer the compositor still holds is a protocol error;
    // skip this frame until it releases (the decoder keeps the slot pinned).
    if (!it->second.Get()->released_)
      return nullptr;
    it->second.Get()->released_ = false;  // about to be attached again
    return it->second.Get()->GetProxy();
  }

  wl_proxy* raw_params = impl_->make_params();
  if (raw_params == nullptr) {
    std::fprintf(stderr, "wayland-sink: create_params failed\n");
    return nullptr;
  }
  wl::WlPtr<ParamsHandler> params;
  params.Attach(raw_params);

  const auto mod_hi = static_cast<uint32_t>(frame.modifier >> 32u);
  const auto mod_lo = static_cast<uint32_t>(frame.modifier & 0xffffffffu);
  for (uint32_t i = 0; i < frame.num_planes && i < 4; ++i)
    params.Get()->Add(frame.planes[i].fd, i, frame.planes[i].offset,
                      frame.planes[i].pitch, mod_hi, mod_lo);

  using ParamsTraits =
      linux_dmabuf_unstable_v1::client::zwp_linux_buffer_params_v1_traits;
  wl_proxy* buf_raw = wl::construct<wayland::client::wl_buffer_traits,
                                    ParamsTraits::Op::CreateImmed>(
      *params.Get(), static_cast<int32_t>(frame.width),
      static_cast<int32_t>(frame.height), impl_->fourcc, 0u);
  if (buf_raw == nullptr) {
    std::fprintf(stderr, "wayland-sink: create_immed failed\n");
    return nullptr;
  }

  wl::WlPtr<WlBufferHandler> buffer;
  buffer.Get()->slot_ = frame.pool_slot;
  buffer.Get()->on_release_ = &impl_->on_release;
  buffer.Get()->released_ = false;
  buffer.Get()->_SetProxy(buf_raw);  // installs the release listener
  wl_proxy* proxy = buffer.Get()->GetProxy();
  impl_->cache.emplace(frame.pool_slot, std::move(buffer));
  return proxy;
}

}  // namespace ck
