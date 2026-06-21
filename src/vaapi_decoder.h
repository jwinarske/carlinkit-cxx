// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#pragma once
// libavcodec + VAAPI H.264 decoder that exports each decoded frame as a
// zero-copy DRM-PRIME DMA-BUF (NV12, vendor-tiled modifier). The exported
// descriptor feeds drm-cxx's ExternalDmaBufSource for HW-plane scanout.
//
// The decode output (NV12 surface) never touches the CPU: av_hwframe_map maps
// the VASurface to DRM-PRIME fds via vaExportSurfaceHandle.
#include <cstddef>
#include <cstdint>
#include <functional>

struct AVCodecContext;
struct AVCodecParserContext;
struct AVBufferRef;
struct AVPacket;
struct AVFrame;

namespace ck {

// One decoded frame as DRM-PRIME planes. Valid only for the duration of the
// FrameCb (the backing AVFrame is freed right after). fds are owned by the
// frame; dup them (ExternalDmaBufSource does) to outlive the callback.
struct DrmFrame {
  uint32_t width = 0, height = 0;
  uint32_t drm_fourcc = 0;  // e.g. DRM_FORMAT_NV12
  uint64_t modifier = 0;    // vendor tiling modifier (AMD GFX, etc.)
  uint32_t surface_id = 0;  // VASurfaceID — stable per decoder pool slot; lets
                            // the consumer cache one KMS FB per surface
  int nplanes = 0;
  struct Plane {
    int fd = -1;
    uint32_t offset = 0;
    uint32_t pitch = 0;
  } planes[4];
};

class VaapiDecoder {
 public:
  // The AVFrame holds the decoded VASurface; clone it (av_frame_clone) to keep
  // the surface out of the decoder's pool while the frame is in use (e.g. on a
  // KMS plane). Valid only during the callback otherwise. Pass nullptr-ignore
  // for headless probing.
  using FrameCb = std::function<void(const DrmFrame&, AVFrame* surface_frame)>;

  VaapiDecoder() = default;
  ~VaapiDecoder();
  VaapiDecoder(const VaapiDecoder&) = delete;
  VaapiDecoder& operator=(const VaapiDecoder&) = delete;

  // Initialize H.264 decode on the given DRM render node.
  bool open(const char* render_node = "/dev/dri/renderD128");

  void set_frame_cb(FrameCb cb) { cb_ = std::move(cb); }

  // Feed Annex-B H.264 bytes (any chunking). Complete frames are parsed,
  // decoded, exported as DRM-PRIME, and delivered via the FrameCb. Returns
  // false on a fatal decode error.
  bool submit(const uint8_t* data, size_t len, uint64_t pts_ns = 0);

  // Drain buffered frames (call at end of stream).
  void flush();

 private:
  bool decode_packet(AVPacket* pkt);
  void deliver(AVFrame* vaapi_frame);

  AVCodecContext* ctx_ = nullptr;
  AVCodecParserContext* parser_ = nullptr;
  AVBufferRef* hw_device_ = nullptr;
  AVPacket* pkt_ = nullptr;
  AVFrame* frame_ = nullptr;  // decoded VAAPI frame
  FrameCb cb_;
};

}  // namespace ck
