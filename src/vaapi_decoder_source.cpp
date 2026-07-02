// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#include "vaapi_decoder_source.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
}
#include <drm_fourcc.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm-cxx/core/device.hpp>

#include <cstdio>

namespace ck {

std::unique_ptr<VaapiDecoderSource> VaapiDecoderSource::create(
    drm::Device& dev,
    uint32_t coded_w,
    uint32_t coded_h,
    const char* render_node) {
  auto src = std::unique_ptr<VaapiDecoderSource>(
      new VaapiDecoderSource(dev, dev.fd(), coded_w, coded_h));
  if (!src->decoder_.open(render_node))
    return nullptr;
  src->decoder_.set_frame_cb([s = src.get()](const DrmFrame& f, AVFrame* af) {
    s->on_decoded(f, af);
  });
  return src;
}

VaapiDecoderSource::VaapiDecoderSource(drm::Device& dev,
                                       int drm_fd,
                                       uint32_t w,
                                       uint32_t h)
    : dev_(dev), drm_fd_(drm_fd), coded_w_(w), coded_h_(h) {
  fmt_.drm_fourcc = DRM_FORMAT_NV12;
  fmt_.modifier = DRM_FORMAT_MOD_INVALID;
  fmt_.width = w;
  fmt_.height = h;
}

VaapiDecoderSource::~VaapiDecoderSource() {
  std::lock_guard<std::mutex> lk(m_);
  for (auto& kv : fb_cache_)
    destroy_fb(kv.second);
  fb_cache_.clear();
  for (AVFrame* f : retained_)
    av_frame_free(&f);
  retained_.clear();
  if (pending_.valid) {
    for (int i = 0; i < pending_.nplanes; ++i)
      if (pending_.fd[i] >= 0)
        close(pending_.fd[i]);
    if (pending_.held)
      av_frame_free(&pending_.held);
  }
}

void VaapiDecoderSource::submit_bitstream(const uint8_t* data,
                                          size_t len,
                                          uint64_t pts_ns) {
  decoder_.submit(data, len, pts_ns);
}

namespace {
// Download a decoded VAAPI frame to CPU and write tightly-packed raw NV12 to
// `<dir>/capture-WxH.nv12` (WxH from the frame's actual decoded size, so the
// name always matches the bytes). Must run on the decode thread (the VAAPI
// context is live there). The written path is returned in out_path.
bool write_nv12_frame(AVFrame* hw,
                      const std::string& dir,
                      std::string& out_path) {
  AVFrame* sw = av_frame_alloc();
  if (sw == nullptr)
    return false;
  bool ok = false;
  if (av_hwframe_transfer_data(sw, hw, 0) == 0 && sw->data[0] != nullptr &&
      sw->data[1] != nullptr) {
    const int w = sw->width;
    const int h = sw->height;
    char name[600];
    std::snprintf(name, sizeof name, "%s/capture-%dx%d.nv12", dir.c_str(), w,
                  h);
    if (FILE* fp = std::fopen(name, "wb"); fp != nullptr) {
      for (int y = 0; y < h; ++y)  // Y plane (stride may exceed width)
        std::fwrite(sw->data[0] + static_cast<size_t>(y) * sw->linesize[0], 1,
                    static_cast<size_t>(w), fp);
      for (int y = 0; y < h / 2; ++y)  // interleaved UV plane
        std::fwrite(sw->data[1] + static_cast<size_t>(y) * sw->linesize[1], 1,
                    static_cast<size_t>(w), fp);
      std::fclose(fp);
      out_path = name;
      ok = true;
    }
  }
  av_frame_free(&sw);
  return ok;
}
}  // namespace

void VaapiDecoderSource::request_capture(const char* dir) {
  {
    std::lock_guard<std::mutex> lk(m_);
    capture_dir_ = dir;
  }
  capture_req_ = true;  // serviced by the next on_decoded (decode thread)
}

// Decode-thread callback. No DRM ioctls here — just dup the dma-buf fds and
// clone the surface-holding AVFrame, then stash as the pending frame.
void VaapiDecoderSource::on_decoded(const DrmFrame& f, AVFrame* surface_frame) {
  // Screenshot the freshly-decoded surface here (decode thread, live context).
  if (capture_req_.exchange(false)) {
    std::string dir;
    {
      std::lock_guard<std::mutex> lk(m_);
      dir = capture_dir_;
    }
    std::string written;
    if (write_nv12_frame(surface_frame, dir, written))
      std::fprintf(stderr, "[capture] wrote %s\n", written.c_str());
    else
      std::fprintf(stderr, "[capture] download failed\n");
  }

  Pending p;
  p.valid = true;
  p.surface_id = f.surface_id;
  p.w = f.width;
  p.h = f.height;
  p.fourcc = f.drm_fourcc;
  p.modifier = f.modifier;
  p.nplanes = f.nplanes;
  for (int i = 0; i < f.nplanes && i < 4; ++i) {
    p.fd[i] = ::dup(f.planes[i].fd);
    p.offset[i] = f.planes[i].offset;
    p.pitch[i] = f.planes[i].pitch;
  }
  p.held = av_frame_clone(surface_frame);
  if (p.held == nullptr) {  // OOM: can't pin the surface — drop this frame
    for (int i = 0; i < p.nplanes; ++i)
      if (p.fd[i] >= 0)
        close(p.fd[i]);
    return;
  }

  std::lock_guard<std::mutex> lk(m_);
  if (pending_.valid) {  // drop the un-consumed previous frame
    for (int i = 0; i < pending_.nplanes; ++i)
      if (pending_.fd[i] >= 0)
        close(pending_.fd[i]);
    if (pending_.held)
      av_frame_free(&pending_.held);
  }
  pending_ = p;
}

void VaapiDecoderSource::import_pending_locked() {
  // Mid-stream resolution change isn't supported (the dongle Open config fixes
  // the size); drop a differently-sized frame rather than scan out a mismatched
  // framebuffer.
  if (fmt_.modifier != DRM_FORMAT_MOD_INVALID &&
      (pending_.w != fmt_.width || pending_.h != fmt_.height)) {
    if (!res_warned_) {
      std::fprintf(stderr,
                   "vaapi: decoded size changed %ux%u -> %ux%u; unsupported, "
                   "dropping frames\n",
                   fmt_.width, fmt_.height, pending_.w, pending_.h);
      res_warned_ = true;
    }
    for (int i = 0; i < pending_.nplanes; ++i)
      if (pending_.fd[i] >= 0)
        close(pending_.fd[i]);
    if (pending_.held)
      av_frame_free(&pending_.held);
    pending_ = Pending{};
    return;
  }

  uint32_t fb = 0;
  auto it = fb_cache_.find(pending_.surface_id);
  if (it != fb_cache_.end()) {
    fb = it->second.fb_id;  // surface already imported — reuse its framebuffer
    for (int i = 0; i < pending_.nplanes; ++i)
      if (pending_.fd[i] >= 0)
        close(pending_.fd[i]);
  } else {
    uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
    uint64_t mods[4] = {0};
    for (int i = 0; i < pending_.nplanes; ++i) {
      if (drmPrimeFDToHandle(drm_fd_, pending_.fd[i], &handles[i]) != 0) {
        std::fprintf(stderr, "kms: drmPrimeFDToHandle failed\n");
        handles[i] = 0;
      }
      pitches[i] = pending_.pitch[i];
      offsets[i] = pending_.offset[i];
      mods[i] = pending_.modifier;
    }
    int r = drmModeAddFB2WithModifiers(
        drm_fd_, pending_.w, pending_.h, pending_.fourcc, handles, pitches,
        offsets, mods, &fb, DRM_MODE_FB_MODIFIERS);
    for (int i = 0; i < pending_.nplanes; ++i)
      if (pending_.fd[i] >= 0)
        close(pending_.fd[i]);
    if (r != 0) {
      std::fprintf(stderr, "kms: drmModeAddFB2WithModifiers failed: %d\n", r);
      if (pending_.held)
        av_frame_free(&pending_.held);
      pending_ = Pending{};
      return;
    }
    FbEntry e;
    e.fb_id = fb;
    for (int i = 0; i < pending_.nplanes; ++i) {  // dedup handle values
      bool seen = false;
      for (int j = 0; j < e.nhandles; ++j)
        if (e.handles[j] == handles[i])
          seen = true;
      if (!seen && handles[i] != 0)
        e.handles[e.nhandles++] = handles[i];
    }
    fb_cache_.emplace(pending_.surface_id, e);
  }

  if (fmt_.modifier == DRM_FORMAT_MOD_INVALID) {  // lock in the real format
    fmt_.drm_fourcc = pending_.fourcc;
    fmt_.modifier = pending_.modifier;
    fmt_.width = pending_.w;
    fmt_.height = pending_.h;
  }
  current_fb_ = fb;

  // Keep the surface (via its AVFrame) for a few frames so the decoder pool
  // can't recycle and overwrite a buffer still on screen.
  retained_.push_back(pending_.held);
  pending_.held = nullptr;  // ownership moved to retained_
  while (retained_.size() > kRetain) {
    av_frame_free(&retained_.front());
    retained_.pop_front();
  }
  pending_ = Pending{};
}

void VaapiDecoderSource::destroy_fb(FbEntry& e) const {
  if (e.fb_id)
    drmModeRmFB(drm_fd_, e.fb_id);
  for (int i = 0; i < e.nhandles; ++i) {
    drm_gem_close gc{};
    gc.handle = e.handles[i];
    drmIoctl(drm_fd_, DRM_IOCTL_GEM_CLOSE, &gc);
  }
  e = FbEntry{};
}

drm::expected<drm::scene::AcquiredBuffer, std::error_code>
VaapiDecoderSource::acquire() {
  std::lock_guard<std::mutex> lk(m_);
  if (pending_.valid)
    import_pending_locked();
  if (current_fb_ == 0)
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::resource_unavailable_try_again));
  drm::scene::AcquiredBuffer ab;
  ab.fb_id = current_fb_;
  ab.opaque =
      nullptr;  // framebuffers/surfaces are owned here, not by the scene
  return ab;
}

void VaapiDecoderSource::release(drm::scene::AcquiredBuffer) noexcept {
  // No-op: framebuffers are cached for the source's lifetime and surfaces are
  // freed as they age out of the retain ring.
}

drm::scene::SourceFormat VaapiDecoderSource::format() const noexcept {
  std::lock_guard<std::mutex> lk(m_);
  return fmt_;
}

bool VaapiDecoderSource::has_fresh_content() const noexcept {
  std::lock_guard<std::mutex> lk(m_);
  return pending_
      .valid;  // a decoded surface is waiting for acquire() to import
}

}  // namespace ck
