// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#pragma once
// WlShmSink — the software (no-dmabuf) presentation path. It converts a decoded
// planar YUV420 frame to XRGB8888 with libswscale straight into a wl_shm buffer
// and returns it to attach. A small ring of buffers is carved from one shm pool
// (one on screen, spares being filled); wl_buffer.release frees a slot.
//
// This is the fallback when the VAAPI/dmabuf path is unavailable — it works on
// any compositor (wl_shm is mandatory) at the cost of a per-frame CPU convert
// and copy. The frame's pixels are copied during buffer_for, so the decoder's
// borrowed frame may be released immediately after the call returns.
#include <cstdint>
#include <functional>
#include <memory>

#include "decoder_source.h"  // ck::DecodedFrame

struct wl_proxy;

namespace ck {

class WlShmSink {
 public:
  // make_pool issues wl_shm.create_pool(fd, size) and returns the pool proxy
  // (the App wires it to the bound wl_shm). The sink owns the returned pool.
  explicit WlShmSink(std::function<wl_proxy*(int fd, int32_t size)> make_pool);
  ~WlShmSink();
  WlShmSink(const WlShmSink&) = delete;
  WlShmSink& operator=(const WlShmSink&) = delete;

  // Convert `frame` (planar YUV420) into a free shm buffer and return the
  // wl_buffer to attach, or nullptr if the pool could not be sized or no buffer
  // is free this frame (the compositor still holds them all).
  wl_proxy* buffer_for(const DecodedFrame& frame);

  // Invoked when the compositor releases a buffer (a slot freed up), so the App
  // can try to present the next frame.
  void set_on_release(std::function<void()> cb);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ck
