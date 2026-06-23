// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#pragma once
// V4l2DecoderSourceAdapter — wraps drm-cxx's V4l2DecoderSource (a stateful SoC
// video decoder producing NV12 DMA-BUFs) so it presents the same fire-and-
// forget DecoderSource surface as the VAAPI and software backends.
//
// drm-cxx's V4l2DecoderSource is event-loop shaped: the caller polls fd(),
// calls drive() to move buffers, and submit_bitstream() returns EAGAIN when the
// OUTPUT queue is full. This adapter hides that behind an internal pump thread:
// submit_bitstream() (dongle RX thread) only enqueues coded chunks, the pump
// thread drives the decoder and feeds the queue, and acquire()/release()/
// format() (commit thread) forward to the inner source. All inner-source calls
// are serialized under one mutex so the pump and commit threads don't race.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "decoder_source.h"

namespace drm {
class Device;
namespace scene {
class V4l2DecoderSource;
}  // namespace scene
}  // namespace drm

namespace ck {

class V4l2DecoderSourceAdapter : public DecoderSource {
 public:
  // Find an H.264 stateful decoder (CARLINKIT_V4L2_DEV, or a scan of
  // /dev/video*), open it for NV12 output at coded_w/h, and start the pump
  // thread. Returns nullptr if no decoder is found or it cannot be opened.
  static std::unique_ptr<V4l2DecoderSourceAdapter> create(drm::Device& dev,
                                                          uint32_t coded_w,
                                                          uint32_t coded_h);

  ~V4l2DecoderSourceAdapter() override;

  void submit_bitstream(const uint8_t* data,
                        size_t len,
                        uint64_t pts_ns) override;

  // ── drm::scene::LayerBufferSource ──────────────────────────────────────────
  drm::expected<drm::scene::AcquiredBuffer, std::error_code> acquire() override;
  void release(drm::scene::AcquiredBuffer acquired) noexcept override;
  drm::scene::BindingModel binding_model() const noexcept override {
    return drm::scene::BindingModel::SceneSubmitsFbId;
  }
  drm::scene::SourceFormat format() const noexcept override;

 private:
  explicit V4l2DecoderSourceAdapter(
      std::unique_ptr<drm::scene::V4l2DecoderSource> inner);
  bool start();
  void pump();  // pump thread: drive the decoder and feed queued chunks

  std::unique_ptr<drm::scene::V4l2DecoderSource> inner_;
  mutable std::mutex inner_m_;  // serializes all inner_ calls across threads

  std::mutex q_m_;
  std::deque<std::vector<uint8_t>> queue_;  // coded chunks awaiting submit
  static constexpr size_t kMaxQueue = 16;   // bound memory; drop oldest if full

  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> failed_{false};  // drive() reported a fatal error
  int inner_fd_ = -1;
  int wake_r_ = -1, wake_w_ = -1;  // self-pipe to wake the pump on new data
};

}  // namespace ck
