// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#pragma once
// NativeFrame — a decoded video frame described in presentation-neutral terms:
// a DMA-BUF (fourcc + modifier + planes) plus the identity a consumer needs to
// cache its per-frame import. It is the seam between decode and presentation.
//
// Today the KMS import (drmPrimeFDToHandle + AddFB2) is fused into the VAAPI
// backend's acquire(). NativeFrame splits that: a DecoderSource *produces*
// frames (acquire_native_frame), and a FrameSink *imports and presents* them —
// KmsSink into a DRM framebuffer, WaylandSink into a wl_buffer. The same frame
// feeds either sink, so the zero-copy contract holds on both paths.
//
// fd ownership: the fds in a NativeFrame are BORROWED — the producer owns them
// and keeps them valid from the acquire that returned the frame until the next
// acquire_native_frame (or the producer's destruction). A sink that imports
// dups whatever it needs (drmPrimeFDToHandle / zwp_linux_buffer_params consume
// the fd on their own terms) before returning; it must not close the borrowed
// fds. A sink caches its import keyed by pool_slot and imports once per slot.
#include <cstdint>

namespace ck {

struct NativeFrame {
  uint32_t fourcc{0};    // DRM FourCC, e.g. DRM_FORMAT_NV12
  uint64_t modifier{0};  // verbatim from the decoder (vendor-tiled, LINEAR…)
  uint32_t width{0};
  uint32_t height{0};
  uint32_t num_planes{0};
  struct Plane {
    int fd{-1};  // borrowed; see fd-ownership note above
    uint32_t offset{0};
    uint32_t pitch{0};
  } planes[4];
  uint64_t pts_ns{0};
  // Stable identity of this frame's buffer within the decoder's ring (the
  // VASurfaceID for VAAPI). The decoder reuses a fixed pool, so a sink imports
  // one framebuffer / wl_buffer per pool_slot and reuses it on re-appearance.
  uint32_t pool_slot{0};

  [[nodiscard]] bool valid() const noexcept { return num_planes > 0; }
};

}  // namespace ck
