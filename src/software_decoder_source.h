// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#pragma once
// SoftwareDecoderSource — the CPU H.264 decode fallback. libavcodec decodes to
// a CPU frame, libswscale converts it to NV12 directly into a DRM dumb buffer
// (LINEAR), and the buffer is scanned out on a HW plane like the other
// backends. No GPU / VAAPI / V4L2 involvement, so it works anywhere, but the
// per-frame decode + convert + copy is slow — the last resort in the chain.
//
// Threading mirrors VaapiDecoderSource: submit_bitstream decodes synchronously
// on the dongle RX thread (filling a free dumb buffer), and acquire() runs on
// the commit thread, handing the latest filled buffer to the scene. A small
// fixed pool (one on screen, one pending, spares being filled) is recycled in
// place; m_ guards the pool's displayed/pending bookkeeping and the format.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "decoder_source.h"

struct AVCodecContext;
struct AVCodecParserContext;
struct AVPacket;
struct AVFrame;
struct SwsContext;

namespace drm {
class Device;
}  // namespace drm

namespace ck {

class SoftwareDecoderSource : public DecoderSource {
 public:
  // coded_w/h seed the reported format until the first frame locks in the
  // actual decoded size (which sizes the dumb-buffer pool). Returns nullptr if
  // the software H.264 decoder cannot be opened.
  static std::unique_ptr<SoftwareDecoderSource> create(drm::Device& dev,
                                                       uint32_t coded_w,
                                                       uint32_t coded_h);

  ~SoftwareDecoderSource() override;

  void submit_bitstream(const uint8_t* data,
                        size_t len,
                        uint64_t pts_ns) override;
  void request_capture(const char* dir) override;

  // ── drm::scene::LayerBufferSource ──────────────────────────────────────────
  drm::expected<drm::scene::AcquiredBuffer, std::error_code> acquire() override;
  void release(drm::scene::AcquiredBuffer acquired) noexcept override;
  drm::scene::BindingModel binding_model() const noexcept override {
    return drm::scene::BindingModel::SceneSubmitsFbId;
  }
  drm::scene::SourceFormat format() const noexcept override;

 private:
  SoftwareDecoderSource(drm::Device& dev, int drm_fd, uint32_t w, uint32_t h);
  bool open_codec();
  void decode_packet(AVPacket* pkt);    // RX thread
  void on_frame(const AVFrame* frame);  // RX thread
  bool ensure_pool(uint32_t w,
                   uint32_t h);       // RX thread; sets fmt_/pool under m_
  int pick_free_slot_locked() const;  // holds m_
  void write_capture(int slot, uint32_t w, uint32_t h) const;

  // One CPU-mapped dumb buffer holding a full NV12 frame, imported once as a
  // KMS framebuffer and reused.
  static constexpr int kBufs = 4;
  struct Buf {
    uint32_t handle = 0;
    uint32_t fb_id = 0;
    uint8_t* map = nullptr;
    size_t size = 0;
    uint32_t stride = 0;     // pitch of the dumb buffer (>= width)
    bool in_flight = false;  // handed to the scene (acquire), not yet released
  };
  void destroy_buf(Buf& b) const;

  drm::Device& dev_;
  int drm_fd_;

  AVCodecContext* ctx_ = nullptr;
  AVCodecParserContext* parser_ = nullptr;
  AVPacket* pkt_ = nullptr;
  AVFrame* frame_ = nullptr;
  SwsContext* sws_ = nullptr;  // rebuilt on demand for the source pixel format

  mutable std::mutex m_;
  Buf bufs_[kBufs];
  bool pool_ready_ = false;
  bool res_warned_ = false;
  drm::scene::SourceFormat fmt_{};
  uint32_t frame_w_ = 0, frame_h_ = 0;
  // Pool bookkeeping: indices into bufs_. displayed_ is on screen; pending_ is
  // the latest filled buffer awaiting acquire(). -1 means none.
  int displayed_ = -1;
  int pending_ = -1;

  std::atomic<bool> capture_req_{false};
  std::string capture_dir_;  // guarded by m_
};

}  // namespace ck
