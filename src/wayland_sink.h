// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#pragma once
// WaylandSink — the Wayland half of the FrameSink seam. It imports a decoder's
// NativeFrame into a wl_buffer via zwp_linux_buffer_params_v1.create_immed and
// caches one wl_buffer per pool_slot (the decoder reuses a fixed surface pool,
// so each slot is imported exactly once and reattached on re-appearance). The
// compositor's wl_buffer.release drives the slot back to the decoder.
//
// It also answers the negotiation question the zero-copy contract turns on: is
// the decoder's (fourcc, modifier) presentable at all, and is it in a scanout
// tranche (compositor direct scanout, no GPU composite)? — computed from the
// per-surface dmabuf-feedback snapshot the App owns.
//
// The App owns the display/registry/feedback objects; the sink is handed a
// factory that mints a fresh params proxy (wired to
// DmabufFeedback::CreateParams) so it need not be templated on the App type.
#include <cstdint>
#include <functional>
#include <memory>

#include "native_frame.h"

struct wl_proxy;
namespace wl {
struct FeedbackSnapshot;
}  // namespace wl

namespace ck {

class WaylandSink {
 public:
  // make_params mints a fresh zwp_linux_buffer_params_v1 proxy (App wires it to
  // DmabufFeedback::CreateParams). fourcc is the video format
  // (DRM_FORMAT_NV12).
  WaylandSink(std::function<wl_proxy*()> make_params, uint32_t fourcc);
  ~WaylandSink();
  WaylandSink(const WaylandSink&) = delete;
  WaylandSink& operator=(const WaylandSink&) = delete;

  // Presentability of a (fourcc, modifier) against a feedback snapshot.
  // `presentable` = the compositor can attach it (in some tranche);
  // `scanout` = it is in a scanout tranche, so direct scanout is possible
  // (the zero-copy path). Recompute on every OnDmabufFeedback — don't cache.
  struct Verdict {
    bool presentable = false;
    bool scanout = false;
  };
  static Verdict negotiate(const wl::FeedbackSnapshot& snap,
                           uint32_t fourcc,
                           uint64_t modifier);

  // Import `frame` (once per pool_slot; reused thereafter) and return the
  // wl_buffer proxy to attach, or nullptr on failure. The frame's fds are
  // consumed by create_immed, so borrowing them for the call is sufficient.
  wl_proxy* buffer_for(const NativeFrame& frame);

  // Invoked with a pool_slot when the compositor releases that slot's buffer —
  // the App forwards this to DecoderSource::release_native_frame.
  void set_on_release(std::function<void(uint32_t pool_slot)> cb);

  // Drop all cached wl_buffers (e.g. on a format change). Frees the imports.
  void clear();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ck
