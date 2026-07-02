// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#include "gl_rotate_source.h"

#include <GLES3/gl3.h>
#include <drm_fourcc.h>
#include <drm_mode.h>

#include <algorithm>
#include <array>
#include <cstdio>

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/present/gl_scanout_producer.hpp>
#include <drm-cxx/scene/buffer_source.hpp>

namespace ck {
namespace {

// Fullscreen triangle; gl_Position covers the viewport, v_uv is 0..1 across it.
// u_rot maps the sample coordinate about the center (rotation plus the texture
// y-flip) so the plane sees an already-oriented image (it does no rotation).
constexpr const char* kVert = R"(#version 300 es
layout(location = 0) in vec2 a_pos;
out vec2 v_uv;
uniform mat2 u_rot;
void main() {
  gl_Position = vec4(a_pos, 0.0, 1.0);
  vec2 uv = a_pos * 0.5 + 0.5;
  v_uv = u_rot * (uv - 0.5) + 0.5;
}
)";

// Sample the three YUV420P planes and convert to RGB (BT.601 limited range).
constexpr const char* kFrag = R"(#version 300 es
precision mediump float;
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_y;
uniform sampler2D u_u;
uniform sampler2D u_v;
void main() {
  float y = texture(u_y, v_uv).r;
  float u = texture(u_u, v_uv).r - 0.5;
  float v = texture(u_v, v_uv).r - 0.5;
  y = (y - 0.0625) * 1.164;
  vec3 rgb = vec3(y + 1.596 * v,
                  y - 0.391 * u - 0.813 * v,
                  y + 2.018 * u);
  frag = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
)";

unsigned compile(GLenum type, const char* src) {
  const unsigned s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  int ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (ok == 0) {
    std::array<char, 512> log{};
    glGetShaderInfoLog(s, static_cast<int>(log.size()), nullptr, log.data());
    std::fprintf(stderr, "gl_rotate: shader compile failed: %s\n", log.data());
    glDeleteShader(s);
    return 0;
  }
  return s;
}

// Column-major mat2 (glUniformMatrix2fv, GL_FALSE) mapping the screen sample
// coord to the top-down YUV texture for a DRM_MODE_ROTATE_* display angle. Each
// folds in the GL texture y-flip (data row 0 is the image top) so the result is
// upright rather than mirrored.
std::array<float, 4> rot_matrix(uint64_t rot) {
  switch (rot) {
    case DRM_MODE_ROTATE_90:
      return {0.f, 1.f, 1.f, 0.f};
    case DRM_MODE_ROTATE_180:
      return {-1.f, 0.f, 0.f, 1.f};
    case DRM_MODE_ROTATE_270:
      return {0.f, -1.f, -1.f, 0.f};
    default:
      return {1.f, 0.f, 0.f, -1.f};
  }
}

}  // namespace

GlRotateSource::GlRotateSource(drm::Device& dev,
                               std::unique_ptr<DecoderSource> inner,
                               uint64_t rot,
                               uint32_t out_w,
                               uint32_t out_h)
    : dev_(dev),
      inner_(std::move(inner)),
      rot_(rot),
      out_w_(out_w),
      out_h_(out_h) {}

GlRotateSource::~GlRotateSource() {
  if (gl_ready_ && producer_ && producer_->make_current()) {
    if (program_ != 0) {
      glDeleteProgram(program_);
    }
    if (vbo_ != 0) {
      glDeleteBuffers(1, &vbo_);
    }
    glDeleteTextures(3, tex_);
  }
}

std::unique_ptr<GlRotateSource> GlRotateSource::create(
    drm::Device& dev,
    std::unique_ptr<DecoderSource> inner,
    uint64_t rot,
    uint32_t out_w,
    uint32_t out_h,
    const std::vector<uint64_t>& scene_modifiers) {
  if (!inner || !inner->enable_gpu_ingest()) {
    return nullptr;  // no raw-frame ingest (e.g. a HW decoder)
  }
  auto prod = drm::present::GlScanoutProducer::create(dev);
  if (!prod) {
    std::fprintf(stderr, "gl_rotate: GlScanoutProducer::create failed: %s\n",
                 prod.error().message().c_str());
    return nullptr;
  }
  auto producer = std::move(*prod);

  // Intersect what EGL can render for XRGB8888 with what the scene's planes
  // accept; fall back to EGL's own list, then LINEAR.
  std::vector<uint64_t> allowed;
  for (uint64_t m : producer->exportable_modifiers(DRM_FORMAT_ARGB8888)) {
    if (scene_modifiers.empty() ||
        std::find(scene_modifiers.begin(), scene_modifiers.end(), m) !=
            scene_modifiers.end()) {
      allowed.push_back(m);
    }
  }
  if (allowed.empty()) {
    allowed.push_back(DRM_FORMAT_MOD_LINEAR);
  }

  auto buf =
      producer->create_buffer(out_w, out_h, DRM_FORMAT_ARGB8888, allowed);
  if (!buf) {
    std::fprintf(stderr, "gl_rotate: create_buffer failed: %s\n",
                 buf.error().message().c_str());
    return nullptr;
  }

  std::unique_ptr<GlRotateSource> self(
      new GlRotateSource(dev, std::move(inner), rot, out_w, out_h));
  self->producer_ = std::move(producer);
  self->proxy_ = std::move(*buf);
  if (!self->init_gl()) {
    return nullptr;
  }
  return self;
}

bool GlRotateSource::init_gl() {
  if (auto r = producer_->make_current(); !r) {
    std::fprintf(stderr, "gl_rotate: make_current failed: %s\n",
                 r.error().message().c_str());
    return false;
  }
  const unsigned vs = compile(GL_VERTEX_SHADER, kVert);
  const unsigned fs = compile(GL_FRAGMENT_SHADER, kFrag);
  if (vs == 0 || fs == 0) {
    return false;
  }
  program_ = glCreateProgram();
  glAttachShader(program_, vs);
  glAttachShader(program_, fs);
  glLinkProgram(program_);
  glDeleteShader(vs);
  glDeleteShader(fs);
  int ok = 0;
  glGetProgramiv(program_, GL_LINK_STATUS, &ok);
  if (ok == 0) {
    std::fprintf(stderr, "gl_rotate: program link failed\n");
    return false;
  }
  glUseProgram(program_);
  glUniform1i(glGetUniformLocation(program_, "u_y"), 0);
  glUniform1i(glGetUniformLocation(program_, "u_u"), 1);
  glUniform1i(glGetUniformLocation(program_, "u_v"), 2);
  loc_rot_ = glGetUniformLocation(program_, "u_rot");

  // A single triangle that covers the whole viewport (clip coords).
  static constexpr std::array<float, 6> kTri = {-1.f, -1.f, 3.f,
                                                -1.f, -1.f, 3.f};
  glGenBuffers(1, &vbo_);
  glBindBuffer(GL_ARRAY_BUFFER, vbo_);
  glBufferData(GL_ARRAY_BUFFER, sizeof(kTri), kTri.data(), GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

  glGenTextures(3, tex_);
  for (unsigned t : tex_) {
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  gl_ready_ = true;
  return true;
}

bool GlRotateSource::render_frame() {
  if (!gl_ready_) {
    return false;
  }
  DecodedFrame f;
  if (!inner_->acquire_decoded_frame(f) || f.num_planes < 3) {
    return false;  // no new frame ready
  }
  if (auto r = producer_->make_current(); !r) {
    inner_->release_decoded_frame();
    return false;
  }

  // Upload Y (full), U and V (half each). GL_R8 single-channel textures.
  const std::array<uint32_t, 3> pw = {f.width, f.width / 2, f.width / 2};
  const std::array<uint32_t, 3> ph = {f.height, f.height / 2, f.height / 2};
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  for (int i = 0; i < 3; ++i) {
    glActiveTexture(GL_TEXTURE0 + i);
    glBindTexture(GL_TEXTURE_2D, tex_[i]);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<int>(f.planes[i].stride));
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, static_cast<int>(pw[i]),
                 static_cast<int>(ph[i]), 0, GL_RED, GL_UNSIGNED_BYTE,
                 f.planes[i].data);
  }
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  inner_->release_decoded_frame();

  glViewport(0, 0, static_cast<int>(out_w_), static_cast<int>(out_h_));
  glUseProgram(program_);
  const auto m = rot_matrix(rot_);
  glUniformMatrix2fv(loc_rot_, 1, GL_FALSE, m.data());
  glBindBuffer(GL_ARRAY_BUFFER, vbo_);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
  glDrawArrays(GL_TRIANGLES, 0, 3);

  if (auto r = producer_->swap_buffers(); !r) {
    return false;
  }
  rendered_once_ = true;
  last_seq_ = f.seq;
  return true;
}

drm::expected<drm::scene::AcquiredBuffer, std::error_code>
GlRotateSource::acquire() {
  render_frame();  // renders the latest inner frame if one is ready
  if (!rendered_once_) {
    return drm::unexpected(
        std::make_error_code(std::errc::resource_unavailable_try_again));
  }
  return proxy_->acquire();
}

void GlRotateSource::release(drm::scene::AcquiredBuffer acquired) noexcept {
  proxy_->release(std::move(acquired));
}

bool GlRotateSource::has_fresh_content() const noexcept {
  // Fresh only when the inner decoder has a newer frame than the one last
  // rendered; when the screen is static the scene then keeps scanning out the
  // current buffer instead of re-committing an identical image every vblank.
  return inner_->gpu_frame_seq() > last_seq_;
}

drm::scene::BindingModel GlRotateSource::binding_model() const noexcept {
  return proxy_->binding_model();
}

drm::scene::SourceFormat GlRotateSource::format() const noexcept {
  return proxy_->format();
}

}  // namespace ck
