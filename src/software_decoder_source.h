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
#include <vector>

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
  // actual decoded size (which sizes the dumb-buffer pool). `rot` is a
  // DRM_MODE_ROTATE_* bit baked into the output buffer on the CPU (the plane is
  // not asked to rotate). Returns nullptr if the H.264 decoder cannot be
  // opened.
  static std::unique_ptr<SoftwareDecoderSource> create(drm::Device& dev,
                                                       uint32_t coded_w,
                                                       uint32_t coded_h,
                                                       uint64_t rot);

  ~SoftwareDecoderSource() override;

  void submit_bitstream(const uint8_t* data,
                        size_t len,
                        uint64_t pts_ns) override;
  void request_capture(const char* dir) override;
  [[nodiscard]] uint64_t applied_rotation() const noexcept override {
    return rot_;
  }

  // ── drm::scene::LayerBufferSource ──────────────────────────────────────────
  drm::expected<drm::scene::AcquiredBuffer, std::error_code> acquire() override;
  void release(drm::scene::AcquiredBuffer acquired) noexcept override;
  drm::scene::BindingModel binding_model() const noexcept override {
    return drm::scene::BindingModel::SceneSubmitsFbId;
  }
  drm::scene::SourceFormat format() const noexcept override;

  // ── GPU rotate/convert ingest ──────────────────────────────────────────────
  [[nodiscard]] bool enable_gpu_ingest() override;
  [[nodiscard]] bool acquire_decoded_frame(DecodedFrame& out) override;
  void release_decoded_frame() noexcept override;
  [[nodiscard]] uint64_t gpu_frame_seq() const noexcept override {
    return gpu_frame_seq_.load(std::memory_order_acquire);
  }

 private:
  SoftwareDecoderSource(drm::Device& dev,
                        int drm_fd,
                        uint32_t w,
                        uint32_t h,
                        uint64_t rot);
  bool open_codec();
  void decode_packet(AVPacket* pkt);    // RX thread
  void on_frame(const AVFrame* frame);  // RX thread
  // Output (post-rotation) dimensions for a source frame: 90/270 swap w<->h.
  void out_dims(uint32_t sw, uint32_t sh, uint32_t& ow, uint32_t& oh) const;
  bool ensure_pool(uint32_t sw,
                   uint32_t sh);      // RX thread; sets fmt_/pool under m_
  int pick_free_slot_locked() const;  // holds m_
  // Rotate a packed NV12 frame (src, sw x sh, both planes stride = sw) into the
  // dumb buffer `dst` (stride dstride) by rot_. RX thread.
  void rotate_nv12(const uint8_t* src,
                   uint32_t sw,
                   uint32_t sh,
                   uint8_t* dst,
                   uint32_t dstride) const;
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
  uint64_t rot_ = 0;  // DRM_MODE_ROTATE_* baked into the output on the CPU
  std::vector<uint8_t> staging_{};  // NV12 at source dims; rotated into buffer

  AVCodecContext* ctx_ = nullptr;
  AVCodecParserContext* parser_ = nullptr;
  AVPacket* pkt_ = nullptr;
  AVFrame* frame_ = nullptr;
  SwsContext* sws_ = nullptr;  // rebuilt on demand for the source pixel format

  // GPU-ingest lane: keep the latest raw YUV420P frame for a GPU rotate/convert
  // stage instead of converting to NV12 in a dumb buffer. gpu_borrowed_ holds a
  // ref while the commit thread uploads it (acquire..release_decoded_frame).
  bool gpu_ingest_ = false;
  AVFrame* latest_gpu_frame_ = nullptr;  // RX thread writes; guarded by m_
  AVFrame* gpu_borrowed_ = nullptr;      // commit-thread borrow ref
  // Bumped (under m_) each time a new frame is kept in latest_gpu_frame_; read
  // lock-free by a GPU consumer to detect a new frame. Stamped onto each
  // acquired DecodedFrame so the consumer knows exactly which frame it drew.
  std::atomic<uint64_t> gpu_frame_seq_{0};

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
