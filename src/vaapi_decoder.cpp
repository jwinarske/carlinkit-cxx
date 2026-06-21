// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#include "vaapi_decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/pixdesc.h>
#include <va/va.h>
#include <va/va_drmcommon.h>
}

#include <unistd.h>  // close
#include <cstdio>

namespace ck {

namespace {
// Decoder asks which output format to use; pick the VAAPI HW surface and set up
// the frames pool so the decoder writes into exportable VASurfaces.
enum AVPixelFormat get_vaapi_format(AVCodecContext* ctx,
                                    const enum AVPixelFormat* fmts) {
  bool has_vaapi = false;
  for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
    if (*p == AV_PIX_FMT_VAAPI)
      has_vaapi = true;
  if (!has_vaapi) {
    std::fprintf(stderr, "vaapi: VAAPI not offered; falling back to %s\n",
                 av_get_pix_fmt_name(fmts[0]));
    return fmts[0];
  }
  AVBufferRef* frames = av_hwframe_ctx_alloc(ctx->hw_device_ctx);
  if (!frames)
    return fmts[0];
  auto* fc = reinterpret_cast<AVHWFramesContext*>(frames->data);
  fc->format = AV_PIX_FMT_VAAPI;
  fc->sw_format = AV_PIX_FMT_NV12;
  fc->width = ctx->coded_width;
  fc->height = ctx->coded_height;
  fc->initial_pool_size = 20;
  if (av_hwframe_ctx_init(frames) < 0) {
    std::fprintf(stderr, "vaapi: av_hwframe_ctx_init failed\n");
    av_buffer_unref(&frames);
    return fmts[0];
  }
  ctx->hw_frames_ctx = frames;
  return AV_PIX_FMT_VAAPI;
}
}  // namespace

VaapiDecoder::~VaapiDecoder() {
  if (frame_)
    av_frame_free(&frame_);
  if (pkt_)
    av_packet_free(&pkt_);
  if (parser_)
    av_parser_close(parser_);
  if (ctx_)
    avcodec_free_context(&ctx_);
  if (hw_device_)
    av_buffer_unref(&hw_device_);
}

bool VaapiDecoder::open(const char* render_node) {
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
  if (!codec) {
    std::fprintf(stderr, "vaapi: no H.264 decoder\n");
    return false;
  }
  // Confirm this libavcodec build actually exposes a VAAPI hwaccel for H.264.
  // Fedora's stripped `libavcodec-free` reports none here and would silently
  // software-decode; `libavcodec-freeworld` (RPM Fusion) provides it. See
  // README.
  bool vaapi_capable = false;
  for (int i = 0;; ++i) {
    const AVCodecHWConfig* hw = avcodec_get_hw_config(codec, i);
    if (!hw)
      break;
    if (hw->device_type == AV_HWDEVICE_TYPE_VAAPI)
      vaapi_capable = true;
  }
  if (!vaapi_capable)
    std::fprintf(
        stderr,
        "vaapi: WARNING this libavcodec has no VAAPI H.264 hwaccel; will fall "
        "back to "
        "software. On Fedora: sudo dnf install libavcodec-freeworld\n");

  parser_ = av_parser_init(codec->id);
  ctx_ = avcodec_alloc_context3(codec);
  if (!parser_ || !ctx_)
    return false;

  if (int r = av_hwdevice_ctx_create(&hw_device_, AV_HWDEVICE_TYPE_VAAPI,
                                     render_node, nullptr, 0);
      r < 0) {
    std::fprintf(stderr, "vaapi: av_hwdevice_ctx_create(%s) failed: %d\n",
                 render_node, r);
    return false;
  }
  ctx_->hw_device_ctx = av_buffer_ref(hw_device_);
  ctx_->get_format = get_vaapi_format;

  if (int r = avcodec_open2(ctx_, codec, nullptr); r < 0) {
    std::fprintf(stderr, "vaapi: avcodec_open2 failed: %d\n", r);
    return false;
  }
  pkt_ = av_packet_alloc();
  frame_ = av_frame_alloc();
  return pkt_ && frame_;
}

bool VaapiDecoder::submit(const uint8_t* data, size_t len, uint64_t pts_ns) {
  while (len > 0) {
    uint8_t* out = nullptr;
    int out_size = 0;
    int used = av_parser_parse2(
        parser_, ctx_, &out, &out_size, data, static_cast<int>(len),
        static_cast<int64_t>(pts_ns), AV_NOPTS_VALUE, 0);
    if (used < 0) {
      std::fprintf(stderr, "vaapi: parser error\n");
      return false;
    }
    data += used;
    len -= static_cast<size_t>(used);
    if (out_size > 0) {
      pkt_->data = out;
      pkt_->size = out_size;
      if (!decode_packet(pkt_))
        return false;
    }
  }
  return true;
}

void VaapiDecoder::flush() {
  decode_packet(nullptr);
}

bool VaapiDecoder::decode_packet(AVPacket* pkt) {
  int r = avcodec_send_packet(ctx_, pkt);
  if (r < 0 && r != AVERROR_EOF) {
    std::fprintf(stderr, "vaapi: send_packet: %d\n", r);
    return false;
  }
  while (true) {
    r = avcodec_receive_frame(ctx_, frame_);
    if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
      break;
    if (r < 0) {
      std::fprintf(stderr, "vaapi: receive_frame: %d\n", r);
      return false;
    }
    deliver(frame_);
    av_frame_unref(frame_);
  }
  return true;
}

void VaapiDecoder::deliver(AVFrame* vaapi_frame) {
  if (!cb_)
    return;

  // Reach the VADisplay (from the ffmpeg HW device) and the decoded surface.
  auto* dctx = reinterpret_cast<AVHWDeviceContext*>(hw_device_->data);
  auto* vactx = reinterpret_cast<AVVAAPIDeviceContext*>(dctx->hwctx);
  VADisplay dpy = vactx->display;
  auto surf = static_cast<VASurfaceID>(
      reinterpret_cast<uintptr_t>(vaapi_frame->data[3]));

  vaSyncSurface(dpy, surf);  // ensure decode has completed before export

  // Export the surface as DRM-PRIME dma-buf(s). COMPOSED_LAYERS folds NV12 into
  // a single layer (drm_format=NV12, 2 planes) — exactly what a KMS FB wants.
  VADRMPRIMESurfaceDescriptor d{};
  VAStatus s = vaExportSurfaceHandle(
      dpy, surf, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
      VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &d);
  if (s != VA_STATUS_SUCCESS) {
    std::fprintf(stderr, "vaapi: vaExportSurfaceHandle: %s (0x%x)\n",
                 vaErrorStr(s), s);
    return;
  }

  DrmFrame f;
  f.width = d.width;
  f.height = d.height;
  f.surface_id = static_cast<uint32_t>(surf);
  const auto& layer = d.layers[0];
  f.drm_fourcc = layer.drm_format;
  f.nplanes = static_cast<int>(layer.num_planes);
  for (uint32_t p = 0; p < layer.num_planes && p < 4; ++p) {
    uint32_t obj = layer.object_index[p];
    f.planes[p].fd = d.objects[obj].fd;
    f.planes[p].offset = layer.offset[p];
    f.planes[p].pitch = layer.pitch[p];
    f.modifier = d.objects[obj].drm_format_modifier;
  }

  cb_(f, vaapi_frame);  // consumer dups fds + clones the AVFrame it needs

  for (uint32_t i = 0; i < d.num_objects; ++i)
    if (d.objects[i].fd >= 0)
      close(d.objects[i].fd);
}

}  // namespace ck
