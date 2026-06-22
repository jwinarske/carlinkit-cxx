// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#pragma once
// VaapiDecoderSource — a drm-cxx LayerBufferSource backed by the
// libavcodec+VAAPI H.264 decoder. H.264 is fed in (submit_bitstream, from the
// dongle thread); each decoded NV12 DMA-BUF is imported as a KMS framebuffer
// (on the commit thread, in acquire()) and scanned out zero-copy on a HW plane.
//
// Surface lifetime: the decoder reuses a fixed pool of VASurfaces, so we import
// one KMS framebuffer per surface (cached by VASurfaceID, created once) and
// hold an AVFrame ref for the last few frames so the pool cannot overwrite a
// frame still on screen.
#include <drm-cxx/scene/buffer_source.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "vaapi_decoder.h"

namespace drm {
class Device;
}

namespace ck {

class VaapiDecoderSource : public drm::scene::LayerBufferSource {
 public:
  // coded_w/h size the decoder's surface pool; should match the dongle's Open
  // resolution. Returns nullptr on decoder-open failure.
  static std::unique_ptr<VaapiDecoderSource> create(
      drm::Device& dev,
      uint32_t coded_w,
      uint32_t coded_h,
      const char* render_node = "/dev/dri/renderD128");

  ~VaapiDecoderSource() override;

  // Feed Annex-B H.264 bytes (called from the dongle's RX thread).
  void submit_bitstream(const uint8_t* data, size_t len, uint64_t pts_ns = 0);

  // Request a screenshot: the next decoded frame is written to
  // `<dir>/capture-WxH.nv12` as tightly-packed raw NV12 (w*h Y + w*h/2
  // interleaved UV), WxH being the frame's actual decoded size. The download
  // runs on the decode thread, where the VAAPI context is live. Convert with
  // e.g. ffmpeg -f rawvideo -pix_fmt nv12 -s WxH -i file.nv12 out.png
  void request_capture(const char* dir);

  // ── drm::scene::LayerBufferSource ──────────────────────────────────────────
  drm::expected<drm::scene::AcquiredBuffer, std::error_code> acquire() override;
  void release(drm::scene::AcquiredBuffer acquired) noexcept override;
  drm::scene::BindingModel binding_model() const noexcept override {
    return drm::scene::BindingModel::SceneSubmitsFbId;
  }
  drm::scene::SourceFormat format() const noexcept override;

 private:
  VaapiDecoderSource(drm::Device& dev, int drm_fd, uint32_t w, uint32_t h);
  void on_decoded(const DrmFrame& f, AVFrame* surface_frame);  // decode thread
  void import_pending_locked();  // commit thread, holds m_

  // One cached KMS framebuffer + its GEM handles, imported once per VASurface.
  struct FbEntry {
    uint32_t fb_id = 0;
    uint32_t handles[4] = {0};
    int nhandles = 0;
  };
  void destroy_fb(FbEntry& e) const;

  drm::Device& dev_;
  int drm_fd_;
  uint32_t coded_w_, coded_h_;
  VaapiDecoder decoder_;

  mutable std::mutex m_;
  drm::scene::SourceFormat fmt_{};
  bool res_warned_ = false;

  // Newest decoded frame: fds duped on the decode thread, imported on the
  // commit thread. Replaced (older one dropped) if a new frame arrives before
  // acquire().
  struct Pending {
    bool valid = false;
    uint32_t surface_id = 0;
    uint32_t w = 0, h = 0, fourcc = 0;
    uint64_t modifier = 0;
    int nplanes = 0;
    int fd[4] = {-1, -1, -1, -1};
    uint32_t offset[4] = {0}, pitch[4] = {0};
    AVFrame* held = nullptr;
  } pending_;

  // One framebuffer per surface, created once and reused (the pool is fixed).
  std::unordered_map<uint32_t, FbEntry> fb_cache_;
  // The last few surface-holding AVFrames, kept so the decoder pool can't
  // overwrite a frame still on screen; oldest freed as newer ones arrive.
  std::deque<AVFrame*> retained_;
  uint32_t current_fb_ = 0;
  static constexpr size_t kRetain = 4;

  // Screenshot request: the next decoded frame is written under capture_dir_.
  std::atomic<bool> capture_req_{false};
  std::string capture_dir_;  // guarded by m_
};

}  // namespace ck
