// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#include "software_decoder_source.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}
#include <drm_fourcc.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm-cxx/core/device.hpp>

#include <cstdio>
#include <cstring>

namespace ck {

std::unique_ptr<SoftwareDecoderSource> SoftwareDecoderSource::create(
    drm::Device& dev,
    uint32_t coded_w,
    uint32_t coded_h) {
  auto src = std::unique_ptr<SoftwareDecoderSource>(
      new SoftwareDecoderSource(dev, dev.fd(), coded_w, coded_h));
  if (!src->open_codec())
    return nullptr;
  return src;
}

SoftwareDecoderSource::SoftwareDecoderSource(drm::Device& dev,
                                             int drm_fd,
                                             uint32_t w,
                                             uint32_t h)
    : dev_(dev), drm_fd_(drm_fd) {
  fmt_.drm_fourcc = DRM_FORMAT_NV12;
  fmt_.modifier = DRM_FORMAT_MOD_LINEAR;
  fmt_.width = w;
  fmt_.height = h;
}

SoftwareDecoderSource::~SoftwareDecoderSource() {
  std::lock_guard<std::mutex> lk(m_);
  for (Buf& b : bufs_)
    destroy_buf(b);
  if (sws_)
    sws_freeContext(sws_);
  if (frame_)
    av_frame_free(&frame_);
  if (pkt_)
    av_packet_free(&pkt_);
  if (parser_)
    av_parser_close(parser_);
  if (ctx_)
    avcodec_free_context(&ctx_);
}

bool SoftwareDecoderSource::open_codec() {
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
  if (!codec) {
    std::fprintf(stderr, "software: no H.264 decoder\n");
    return false;
  }
  parser_ = av_parser_init(codec->id);
  ctx_ = avcodec_alloc_context3(codec);
  if (!parser_ || !ctx_)
    return false;
  if (int r = avcodec_open2(ctx_, codec, nullptr); r < 0) {
    std::fprintf(stderr, "software: avcodec_open2 failed: %d\n", r);
    return false;
  }
  pkt_ = av_packet_alloc();
  frame_ = av_frame_alloc();
  return pkt_ && frame_;
}

void SoftwareDecoderSource::submit_bitstream(const uint8_t* data,
                                             size_t len,
                                             uint64_t pts_ns) {
  while (len > 0) {
    uint8_t* out = nullptr;
    int out_size = 0;
    int used = av_parser_parse2(
        parser_, ctx_, &out, &out_size, data, static_cast<int>(len),
        static_cast<int64_t>(pts_ns), AV_NOPTS_VALUE, 0);
    if (used < 0) {
      std::fprintf(stderr, "software: parser error\n");
      return;
    }
    data += used;
    len -= static_cast<size_t>(used);
    if (out_size > 0) {
      pkt_->data = out;
      pkt_->size = out_size;
      decode_packet(pkt_);
    }
  }
}

void SoftwareDecoderSource::decode_packet(AVPacket* pkt) {
  int r = avcodec_send_packet(ctx_, pkt);
  if (r < 0 && r != AVERROR_EOF) {
    std::fprintf(stderr, "software: send_packet: %d\n", r);
    return;
  }
  while (true) {
    r = avcodec_receive_frame(ctx_, frame_);
    if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
      break;
    if (r < 0) {
      std::fprintf(stderr, "software: receive_frame: %d\n", r);
      break;
    }
    on_frame(frame_);
    av_frame_unref(frame_);
  }
}

void SoftwareDecoderSource::on_frame(const AVFrame* frame) {
  const auto w = static_cast<uint32_t>(frame->width);
  const auto h = static_cast<uint32_t>(frame->height);
  if (w == 0 || h == 0)
    return;
  if (!ensure_pool(w, h))
    return;

  // Build (or rebuild, if the source pixel format changed) the converter to
  // NV12 at the same dimensions — no scaling, just a format/layout convert.
  sws_ = sws_getCachedContext(sws_, static_cast<int>(w), static_cast<int>(h),
                              static_cast<AVPixelFormat>(frame->format),
                              static_cast<int>(w), static_cast<int>(h),
                              AV_PIX_FMT_NV12, SWS_POINT, nullptr, nullptr,
                              nullptr);
  if (!sws_) {
    std::fprintf(stderr, "software: sws_getCachedContext failed\n");
    return;
  }

  int slot = -1;
  {
    std::lock_guard<std::mutex> lk(m_);
    slot = pick_free_slot_locked();
  }
  if (slot < 0)
    return;  // no free buffer (should not happen with kBufs >= 3)

  Buf& b = bufs_[slot];
  uint8_t* dst[4] = {b.map, b.map + static_cast<size_t>(b.stride) * h, nullptr,
                     nullptr};
  int dst_stride[4] = {static_cast<int>(b.stride), static_cast<int>(b.stride),
                       0, 0};
  sws_scale(sws_, frame->data, frame->linesize, 0, static_cast<int>(h), dst,
            dst_stride);

  if (capture_req_.exchange(false))
    write_capture(slot, w, h);

  std::lock_guard<std::mutex> lk(m_);
  pending_ = slot;  // latest-frame-wins; any prior unconsumed pending is freed
}

bool SoftwareDecoderSource::ensure_pool(uint32_t w, uint32_t h) {
  if (pool_ready_) {
    // Mid-stream resolution change isn't supported (the dongle Open config
    // fixes the size); drop a differently-sized frame rather than scan out a
    // mismatched buffer.
    if (w != frame_w_ || h != frame_h_) {
      if (!res_warned_) {
        std::fprintf(stderr,
                     "software: decoded size changed %ux%u -> %ux%u; "
                     "unsupported, dropping frames\n",
                     frame_w_, frame_h_, w, h);
        res_warned_ = true;
      }
      return false;
    }
    return true;
  }

  // NV12 in a single dumb buffer: Y plane (h rows) followed by the interleaved
  // UV plane (h/2 rows), so the buffer is h*3/2 rows of `pitch` bytes.
  for (Buf& b : bufs_) {
    uint32_t handle = 0;
    uint32_t pitch = 0;
    uint64_t size = 0;
    if (drmModeCreateDumbBuffer(drm_fd_, w, h * 3 / 2, 8, 0, &handle, &pitch,
                                &size) != 0) {
      std::fprintf(stderr, "software: drmModeCreateDumbBuffer failed\n");
      return false;
    }
    uint64_t off = 0;
    if (drmModeMapDumbBuffer(drm_fd_, handle, &off) != 0) {
      std::fprintf(stderr, "software: drmModeMapDumbBuffer failed\n");
      drmModeDestroyDumbBuffer(drm_fd_, handle);
      return false;
    }
    void* map = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd_,
                     static_cast<off_t>(off));
    if (map == MAP_FAILED) {
      std::fprintf(stderr, "software: mmap dumb buffer failed\n");
      drmModeDestroyDumbBuffer(drm_fd_, handle);
      return false;
    }
    std::memset(map, 0, size);  // black until the first convert lands

    uint32_t handles[4] = {handle, handle, 0, 0};
    uint32_t pitches[4] = {pitch, pitch, 0, 0};
    uint32_t offsets[4] = {0, pitch * h, 0, 0};
    uint32_t fb = 0;
    if (drmModeAddFB2(drm_fd_, w, h, DRM_FORMAT_NV12, handles, pitches, offsets,
                      &fb, 0) != 0) {
      std::fprintf(stderr, "software: drmModeAddFB2 failed\n");
      munmap(map, size);
      drmModeDestroyDumbBuffer(drm_fd_, handle);
      return false;
    }
    b.handle = handle;
    b.fb_id = fb;
    b.map = static_cast<uint8_t*>(map);
    b.size = size;
    b.stride = pitch;
  }

  std::lock_guard<std::mutex> lk(m_);
  frame_w_ = w;
  frame_h_ = h;
  fmt_.drm_fourcc = DRM_FORMAT_NV12;
  fmt_.modifier = DRM_FORMAT_MOD_LINEAR;
  fmt_.width = w;
  fmt_.height = h;
  pool_ready_ = true;
  return true;
}

int SoftwareDecoderSource::pick_free_slot_locked() const {
  // A buffer is free to fill only if it is not the latest filled (pending_),
  // not the last one handed to the scene (displayed_), and not still in flight
  // — a previously displayed buffer can remain on screen until its page-flip
  // completes, signaled by release(). kBufs is sized so one is always free.
  for (int i = 0; i < kBufs; ++i)
    if (i != displayed_ && i != pending_ && !bufs_[i].in_flight)
      return i;
  return -1;
}

void SoftwareDecoderSource::write_capture(int slot,
                                          uint32_t w,
                                          uint32_t h) const {
  std::string dir;
  {
    std::lock_guard<std::mutex> lk(m_);
    dir = capture_dir_;
  }
  const Buf& b = bufs_[slot];
  char name[600];
  std::snprintf(name, sizeof name, "%s/capture-%ux%u.nv12", dir.c_str(), w, h);
  FILE* fp = std::fopen(name, "wb");
  if (fp == nullptr) {
    std::fprintf(stderr, "[capture] cannot open %s\n", name);
    return;
  }
  for (uint32_t y = 0; y < h; ++y)  // Y plane (stride may exceed width)
    std::fwrite(b.map + static_cast<size_t>(y) * b.stride, 1, w, fp);
  for (uint32_t y = 0; y < h / 2; ++y)  // interleaved UV plane
    std::fwrite(b.map + static_cast<size_t>(b.stride) * h +
                    static_cast<size_t>(y) * b.stride,
                1, w, fp);
  std::fclose(fp);
  std::fprintf(stderr, "[capture] wrote %s\n", name);
}

void SoftwareDecoderSource::request_capture(const char* dir) {
  {
    std::lock_guard<std::mutex> lk(m_);
    capture_dir_ = dir;
  }
  capture_req_ = true;  // serviced by the next on_frame (decode thread)
}

void SoftwareDecoderSource::destroy_buf(Buf& b) const {
  if (b.fb_id)
    drmModeRmFB(drm_fd_, b.fb_id);
  if (b.map && b.size)
    munmap(b.map, b.size);
  if (b.handle)
    drmModeDestroyDumbBuffer(drm_fd_, b.handle);
  b = Buf{};
}

drm::expected<drm::scene::AcquiredBuffer, std::error_code>
SoftwareDecoderSource::acquire() {
  std::lock_guard<std::mutex> lk(m_);
  if (pending_ >= 0) {  // promote the latest filled buffer to on-screen
    displayed_ = pending_;
    pending_ = -1;
  }
  if (displayed_ < 0)
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::resource_unavailable_try_again));
  bufs_[displayed_].in_flight = true;  // scene holds it until release()
  drm::scene::AcquiredBuffer ab;
  ab.fb_id = bufs_[displayed_].fb_id;
  ab.opaque = nullptr;  // buffers are owned here, not by the scene
  return ab;
}

void SoftwareDecoderSource::release(
    drm::scene::AcquiredBuffer acquired) noexcept {
  std::lock_guard<std::mutex> lk(m_);
  for (Buf& b : bufs_)
    if (b.fb_id == acquired.fb_id) {  // off screen now — free to refill
      b.in_flight = false;
      break;
    }
}

drm::scene::SourceFormat SoftwareDecoderSource::format() const noexcept {
  std::lock_guard<std::mutex> lk(m_);
  return fmt_;
}

}  // namespace ck
