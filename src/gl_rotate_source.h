// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#pragma once
// GlRotateSource — the GPU rotate + YUV->RGB fallback for angles a KMS plane
// can't do (90/270 on the Pi's vc6). It wraps an inner DecoderSource that hands
// out raw YUV420P frames (see DecoderSource::enable_gpu_ingest), uploads each
// frame's three planes as GLES textures, and draws a rotated, color-converted
// fullscreen quad into a GlScanoutProducer's gbm XRGB8888 surface -- which the
// scene scans out directly. The plane does no rotation; the source reports the
// baked angle via applied_rotation().
//
// It is itself a DecoderSource, so main_kms treats it like any decoder: feed
// submit_bitstream (forwarded to the inner source) and hand it to add_layer.
// The producer's EGL/GLES context lives on the commit thread that calls
// acquire(); the inner decode runs on the RX thread. create() returns nullptr
// if EGL/GLES setup fails, so the caller can fall back to the software rotate.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "decoder_source.h"

namespace drm {
class Device;
}  // namespace drm
namespace drm::present {
class GlScanoutProducer;
}  // namespace drm::present

namespace ck {

class GlRotateSource : public DecoderSource {
 public:
  // Wrap `inner` in a GPU rotate/convert stage. `rot` is the DRM_MODE_ROTATE_*
  // angle to bake on the GPU; `out_w`/`out_h` are the post-rotation output
  // extent (swapped from the video for 90/270). `scene_modifiers` are the
  // scene's candidate XRGB8888 modifiers, intersected with what EGL can render.
  // Returns nullptr on any EGL/GLES/inner-ingest failure.
  static std::unique_ptr<GlRotateSource> create(
      drm::Device& dev,
      std::unique_ptr<DecoderSource> inner,
      uint64_t rot,
      uint32_t out_w,
      uint32_t out_h,
      const std::vector<uint64_t>& scene_modifiers);

  ~GlRotateSource() override;

  GlRotateSource(const GlRotateSource&) = delete;
  GlRotateSource& operator=(const GlRotateSource&) = delete;

  // ── ck::DecoderSource ──────────────────────────────────────────────────────
  void submit_bitstream(const uint8_t* data,
                        size_t len,
                        uint64_t pts_ns) override {
    inner_->submit_bitstream(data, len, pts_ns);
  }
  [[nodiscard]] uint64_t applied_rotation() const noexcept override {
    return rot_;
  }

  // ── drm::scene::LayerBufferSource ──────────────────────────────────────────
  // Renders the latest inner frame (if any) into the producer's surface, then
  // delegates to the producer's proxy for the fb_id.
  drm::expected<drm::scene::AcquiredBuffer, std::error_code> acquire() override;
  void release(drm::scene::AcquiredBuffer acquired) noexcept override;
  [[nodiscard]] bool has_fresh_content() const noexcept override;
  drm::scene::BindingModel binding_model() const noexcept override;
  drm::scene::SourceFormat format() const noexcept override;

 private:
  GlRotateSource(drm::Device& dev,
                 std::unique_ptr<DecoderSource> inner,
                 uint64_t rot,
                 uint32_t out_w,
                 uint32_t out_h);
  bool init_gl();       // compile the program, set up the quad + textures
  bool render_frame();  // make_current -> upload -> draw -> swap_buffers

  drm::Device& dev_;
  std::unique_ptr<DecoderSource> inner_;
  // Declared before proxy_ so the proxy (non-owning over the producer's
  // GbmSurfaceSource) is destroyed first -- EGL surface before gbm_surface.
  std::unique_ptr<drm::present::GlScanoutProducer> producer_;
  std::unique_ptr<drm::scene::LayerBufferSource> proxy_;

  uint64_t rot_ = 0;
  uint32_t out_w_ = 0;
  uint32_t out_h_ = 0;

  // GLES objects (0 until init_gl); typed as unsigned to avoid a GL header
  // here.
  unsigned program_ = 0;
  unsigned vbo_ = 0;
  unsigned tex_[3] = {0, 0, 0};
  int loc_rot_ = -1;
  bool gl_ready_ = false;
  bool rendered_once_ = false;  // the producer's front buffer has valid content
};

}  // namespace ck
