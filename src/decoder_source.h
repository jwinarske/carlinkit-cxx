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

// A decoded frame's CPU pixel planes, borrowed by a GPU rotate/convert stage to
// upload as textures. Valid only until release_decoded_frame(); the backend
// keeps the frame pinned in between.
struct DecodedFrame {
  uint32_t fourcc{0};  // planar layout, e.g. DRM_FORMAT_YUV420 (I420)
  uint32_t width{0};
  uint32_t height{0};
  uint32_t num_planes{0};
  struct Plane {
    const uint8_t* data{nullptr};
    uint32_t stride{0};
  };
  Plane planes[3]{};
  uint64_t pts_ns{0};
  // Monotonic index of this frame within the source's GPU-ingest stream, so a
  // consumer can tell whether a later acquire would return a newer frame (see
  // DecoderSource::gpu_frame_seq) and skip redrawing an unchanged image.
  uint64_t seq{0};
};

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

  // The display rotation (a DRM_MODE_ROTATE_* bit) the source has already baked
  // into its output buffer, so format() is in final orientation and the caller
  // must not also rotate the plane. 0 means the source did not rotate (the
  // caller drives plane rotation). Only the software backend overrides this.
  [[nodiscard]] virtual uint64_t applied_rotation() const noexcept { return 0; }

  // Borrow the latest decoded frame's CPU planes for a GPU rotate/convert stage
  // to upload. Returns false when no frame is ready, or for a backend with no
  // CPU-side frame (the HW decoders, for now). The frame stays valid until
  // release_decoded_frame(); acquire again for the next frame.
  [[nodiscard]] virtual bool acquire_decoded_frame(DecodedFrame& /*out*/) {
    return false;
  }
  virtual void release_decoded_frame() noexcept {}

  // Switch the backend to "GPU ingest": stop producing its own scanout buffer
  // and keep the latest decoded frame available via acquire_decoded_frame() for
  // a GPU rotate/convert stage to consume. Returns true if the backend supports
  // it (software does; HW backends that hand out a dma-buf do not yet). Call
  // once before the first frame arrives.
  [[nodiscard]] virtual bool enable_gpu_ingest() { return false; }

  // Monotonic count of frames made available for GPU ingest since start, so a
  // consumer can detect a new frame without acquiring it — and skip re-drawing
  // (and re-committing) an unchanged image every vblank. The software backend
  // bumps it per kept frame; backends with no CPU-side frame stay at 0.
  [[nodiscard]] virtual uint64_t gpu_frame_seq() const noexcept { return 0; }
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
// could be opened. `rot` is a DRM_MODE_ROTATE_* bit the software backend bakes
// into its output (hardware backends ignore it and leave plane rotation to the
// caller); pass DRM_MODE_ROTATE_0 for none.
std::unique_ptr<DecoderSource> create_decoder_source(drm::Device& dev,
                                                     uint32_t coded_w,
                                                     uint32_t coded_h,
                                                     uint64_t rot);

}  // namespace ck
