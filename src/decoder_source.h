// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#pragma once
// DecoderSource — the carlinkit-side interface a video-decode backend presents
// to the KMS scene. It extends drm-cxx's LayerBufferSource (acquire/release/
// format/binding_model, which the scene consumes each commit) with the feed
// API the scene doesn't cover: encoded H.264 in, optional screenshot out.
//
// Backends, in fallback order (see create_decoder_source):
//   * VAAPI    — libavcodec hardware decode to a vendor-tiled NV12 DMA-BUF,
//                scanned out zero-copy on a HW plane. The desktop/default path.
//   * V4L2     — a stateful SoC video decoder (NV12 LINEAR DMA-BUF). The
//                embedded path; selected when no VAAPI device is present.
//   * software — libavcodec software decode into a CPU dumb buffer. Always
//                available, but slow (a per-frame convert + copy); the last
//                resort, announced with a loud warning when it engages.
#include <drm-cxx/scene/buffer_source.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace drm {
class Device;
}  // namespace drm

namespace ck {

class DecoderSource : public drm::scene::LayerBufferSource {
 public:
  // Feed Annex-B H.264 bytes (any chunking), called from the dongle RX thread.
  // No default pts_ns: a default argument on a virtual is a lint trap (an
  // override could change it); callers pass 0 when pts is irrelevant.
  virtual void submit_bitstream(const uint8_t* data,
                                size_t len,
                                uint64_t pts_ns) = 0;

  // Request that the next decoded frame be written to `<dir>/capture-WxH.nv12`
  // (tightly-packed NV12). Backends with no CPU access to decoded frames may
  // ignore the request; the default is a no-op.
  virtual void request_capture(const char* /*dir*/) {}
};

// Which backend create_decoder_source should use. Auto runs the full fallback
// chain; the others pin a single backend (a test directive — a pinned backend
// that fails to open is an error, not a fall-through).
enum class DecoderBackend { Auto, Vaapi, V4l2, Software };

// Build the video-decode source. The backend is chosen by CARLINKIT_DECODER
// (vaapi|v4l2|software|auto, default auto); Auto tries VAAPI, then V4L2, then
// software, and uses the first that opens. A CARLINKIT_SOFTWARE_ONLY build
// compiles in only the software backend. coded_w/coded_h seed the decoder's
// buffer pool (the dongle's Open resolution). Returns nullptr if no backend
// could be opened.
std::unique_ptr<DecoderSource> create_decoder_source(drm::Device& dev,
                                                     uint32_t coded_w,
                                                     uint32_t coded_h);

}  // namespace ck
