// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
// carlinkit-kms — live CarPlay/Android Auto on a hardware plane (zero-copy).
//
// Receives the dongle's H.264 over USB, HW-decodes it to NV12 DMA-BUF
// (libavcodec+VAAPI), and scans it out on a KMS plane via drm-cxx.
//
// MUST run on a free VT with DRM master (Ctrl+Alt+F3), NOT inside a desktop or
// over SSH. Usage:
//   carlinkit-kms [/dev/dri/cardN] [--no-seat] [--drm-list-modes]
//                 [--drm-mode N|WxH[@R]]
// --drm-list-modes prints the connector's modes; --drm-mode selects one (by
// index or WxH[@R]) and sets resolution/fps/DPI from it.
#include <poll.h>
#include <strings.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drmMode.h>

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/cursor/cursor.hpp>
#include <drm-cxx/cursor/renderer.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/modeset/page_flip.hpp>
#include <drm-cxx/planes/layer.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>

#include "open_output.hpp"  // drm-cxx examples/common helper (on the include path)

#include <optional>

#include <drm-cxx/input/seat.hpp>

#include "audio_alsa.h"
#include "config_env.h"
#include "decoder_source.h"
#include "dongle.h"
#include "dongle_manager.h"
#include "input_touch.h"

#ifdef CARLINKIT_GL
#include "gl_rotate_source.h"
#endif

namespace {
std::atomic<bool> g_quit{false};
std::atomic<bool> g_capture{false};
void on_sigint(int) {
  g_quit = true;
}
void on_sigusr1(int) {  // request a screenshot of the current decoded frame
  g_capture = true;
}

// DPI from the connector's physical size: round(hdisplay / (mmWidth / 25.4)).
// Falls back to 160 when the panel reports no physical size. Clamped to a sane
// range so a bogus EDID can't produce an absurd density.
uint32_t computed_dpi(int fd, uint32_t connector_id, uint32_t hdisplay) {
  uint32_t dpi = 160;
  drmModeConnector* c = drmModeGetConnector(fd, connector_id);
  if (c != nullptr) {
    if (c->mmWidth > 0 && hdisplay > 0) {
      const auto d = static_cast<uint32_t>(
          std::lround(static_cast<double>(hdisplay) * 25.4 / c->mmWidth));
      dpi = std::clamp<uint32_t>(d, 72, 480);
    }
    drmModeFreeConnector(c);
  }
  return dpi;
}

// ── Plane rotation ──────────────────────────────────────────────────────────
// Supported rotation bits on a plane, from its "rotation" bitmask property
// (DRM_MODE_ROTATE_*/REFLECT_*). 0 if the plane exposes no rotation property.
uint64_t plane_rotation_bits(int fd, uint32_t plane_id) {
  uint64_t bits = 0;
  drmModeObjectProperties* props =
      drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
  if (props != nullptr) {
    for (uint32_t i = 0; i < props->count_props; ++i) {
      drmModePropertyRes* pr = drmModeGetProperty(fd, props->props[i]);
      if (pr != nullptr) {
        if (std::strcmp(pr->name, "rotation") == 0 &&
            (pr->flags & DRM_MODE_PROP_BITMASK) != 0)
          for (int e = 0; e < pr->count_enums; ++e)
            bits |= 1ULL << pr->enums[e].value;  // enum value = the bit index
        drmModeFreeProperty(pr);
      }
    }
    drmModeFreeObjectProperties(props);
  }
  return bits;
}

// CRTCs (bitmask, drmModeRes->crtcs order) this connector's encoders can drive.
uint32_t connector_crtc_mask(int fd, uint32_t connector_id) {
  uint32_t mask = 0;
  drmModeConnector* c = drmModeGetConnector(fd, connector_id);
  if (c != nullptr) {
    for (int e = 0; e < c->count_encoders; ++e) {
      drmModeEncoder* enc = drmModeGetEncoder(fd, c->encoders[e]);
      if (enc != nullptr) {
        mask |= enc->possible_crtcs;
        drmModeFreeEncoder(enc);
      }
    }
    drmModeFreeConnector(c);
  }
  return mask;
}

// Rotation bits supported by some NV12-capable plane that can drive
// `connector`.
uint64_t connector_rotation_caps(int fd, uint32_t connector_id) {
  const uint32_t cmask = connector_crtc_mask(fd, connector_id);
  uint64_t bits = 0;
  drmModePlaneRes* pr = drmModeGetPlaneResources(fd);
  for (uint32_t i = 0; pr != nullptr && i < pr->count_planes; ++i) {
    drmModePlane* pl = drmModeGetPlane(fd, pr->planes[i]);
    if (pl == nullptr)
      continue;
    bool nv12 = false;
    for (uint32_t f = 0; f < pl->count_formats; ++f)
      if (pl->formats[f] == DRM_FORMAT_NV12)
        nv12 = true;
    if (nv12 && (pl->possible_crtcs & cmask) != 0)
      bits |= plane_rotation_bits(fd, pl->plane_id);
    drmModeFreePlane(pl);
  }
  if (pr != nullptr)
    drmModeFreePlaneResources(pr);
  return bits;
}

// Human list of the angles in a rotation bitmask, for --drm-list-modes.
std::string rotation_angles_str(uint64_t bits) {
  if (bits == 0)
    return "none (no plane rotation property)";
  std::string s;
  const std::pair<uint64_t, const char*> angles[] = {
      {DRM_MODE_ROTATE_0, "0"},
      {DRM_MODE_ROTATE_90, "90"},
      {DRM_MODE_ROTATE_180, "180"},
      {DRM_MODE_ROTATE_270, "270"}};
  for (const auto& [b, n] : angles)
    if ((bits & b) != 0)
      s += (s.empty() ? "" : ", ") + std::string(n);
  if ((bits & DRM_MODE_REFLECT_X) != 0)
    s += ", reflect-x";
  if ((bits & DRM_MODE_REFLECT_Y) != 0)
    s += ", reflect-y";
  return s;
}

// CARLINKIT_ROTATE = 90|180|270 -> the DRM_MODE_ROTATE_* bit; 0 (none)
// otherwise.
uint64_t requested_rotation() {
  const char* r = std::getenv("CARLINKIT_ROTATE");
  if (r == nullptr)
    return 0;
  if (std::strcmp(r, "90") == 0)
    return DRM_MODE_ROTATE_90;
  if (std::strcmp(r, "180") == 0)
    return DRM_MODE_ROTATE_180;
  if (std::strcmp(r, "270") == 0)
    return DRM_MODE_ROTATE_270;
  return 0;
}

// Print the connector's available modes and plane rotation support (for
// --drm-list-modes), so the user can pick a mode and a rotation angle.
void list_modes(int fd, uint32_t connector_id) {
  drmModeConnector* c = drmModeGetConnector(fd, connector_id);
  if (c == nullptr) {
    std::fprintf(stderr, "no connector %u\n", connector_id);
    return;
  }
  char name[64];
  std::snprintf(name, sizeof name, "%s-%u",
                drmModeGetConnectorTypeName(c->connector_type),
                c->connector_type_id);
  std::printf("Modes for %s (connector %u):\n", name, connector_id);
  for (int i = 0; i < c->count_modes; ++i) {
    const drmModeModeInfo& m = c->modes[i];
    std::printf("  [%2d] %5ux%-5u @ %3uHz%s\n", i, m.hdisplay, m.vdisplay,
                m.vrefresh,
                (m.type & DRM_MODE_TYPE_PREFERRED) != 0 ? "  (preferred)" : "");
  }
  std::printf(
      "Rotation (NV12 plane): %s\n",
      rotation_angles_str(connector_rotation_caps(fd, connector_id)).c_str());
  drmModeFreeConnector(c);
}

// Resolve --drm-mode: a mode index (from --drm-list-modes) or a "WxH" / "WxH@R"
// string. The chosen mode drives resolution, fps, and (via the panel) DPI.
std::optional<drmModeModeInfo> pick_mode(int fd,
                                         uint32_t connector_id,
                                         const char* sel) {
  drmModeConnector* c = drmModeGetConnector(fd, connector_id);
  if (c == nullptr)
    return std::nullopt;
  std::optional<drmModeModeInfo> r;
  char* end = nullptr;
  const long idx = std::strtol(sel, &end, 10);
  if (end != sel && *end == '\0' && idx >= 0 && idx < c->count_modes) {
    r = c->modes[idx];
  } else {
    unsigned w = 0, h = 0, rate = 0;
    const int n =
        std::sscanf(sel, "%ux%u@%u", &w, &h, &rate);  // NOLINT(cert-err34-c)
    if (n >= 2)
      for (int i = 0; i < c->count_modes; ++i)
        if (c->modes[i].hdisplay == w && c->modes[i].vdisplay == h &&
            (n < 3 || c->modes[i].vrefresh == rate)) {
          r = c->modes[i];
          break;
        }
  }
  drmModeFreeConnector(c);
  return r;
}

// Resolve DRM_FORCE_MODE=WxH to a mode on the connector. We otherwise keep the
// connector's *preferred* (native) mode: driving a non-native mode makes the
// panel's own scaler rescale to its physical grid, distorting aspect. Returns
// nullopt unless a valid override is requested and matched.
std::optional<drmModeModeInfo> forced_mode(int fd,
                                           uint32_t connector_id,
                                           const char* force) {
  if (force == nullptr)
    return std::nullopt;
  unsigned fw = 0, fh = 0;
  const int n = std::sscanf(force, "%ux%u", &fw, &fh);  // NOLINT(cert-err34-c)
  if (n != 2 || fw == 0 || fh == 0)
    return std::nullopt;
  drmModeConnector* c = drmModeGetConnector(fd, connector_id);
  if (c == nullptr)
    return std::nullopt;
  std::optional<drmModeModeInfo> r;
  for (int i = 0; i < c->count_modes; ++i)
    if (c->modes[i].hdisplay == fw && c->modes[i].vdisplay == fh) {
      r = c->modes[i];
      break;
    }
  drmModeFreeConnector(c);
  return r;
}

// Bitmask of CRTC indices (in drmModeRes->crtcs order) that have at least one
// NV12-capable plane — i.e. CRTCs that can scan out our decoded video.
uint32_t video_crtc_mask(int fd) {
  uint32_t mask = 0;
  drmModePlaneRes* pr = drmModeGetPlaneResources(fd);
  if (pr == nullptr)
    return 0;
  for (uint32_t i = 0; i < pr->count_planes; ++i) {
    drmModePlane* pl = drmModeGetPlane(fd, pr->planes[i]);
    if (pl == nullptr)
      continue;
    for (uint32_t f = 0; f < pl->count_formats; ++f)
      if (pl->formats[f] == DRM_FORMAT_NV12) {
        mask |= pl->possible_crtcs;
        break;
      }
    drmModeFreePlane(pl);
  }
  drmModeFreePlaneResources(pr);
  return mask;
}

// Find a connected connector by name (e.g. "DP-1", "HDMI-A-1") and resolve a
// CRTC (preferring one with an NV12-capable plane) + preferred mode. Lets the
// user target a specific display. Returns nullopt if not found/connected.
struct ConnectorSel {
  uint32_t connector_id;
  uint32_t crtc_id;
  drmModeModeInfo mode;
};
std::optional<ConnectorSel> select_connector(int fd, const char* want) {
  drmModeRes* res = drmModeGetResources(fd);
  if (res == nullptr)
    return std::nullopt;
  const uint32_t vmask = video_crtc_mask(fd);
  std::optional<ConnectorSel> result;
  for (int i = 0; i < res->count_connectors && !result; ++i) {
    drmModeConnector* c = drmModeGetConnector(fd, res->connectors[i]);
    if (c == nullptr)
      continue;
    char name[64];
    std::snprintf(name, sizeof name, "%s-%u",
                  drmModeGetConnectorTypeName(c->connector_type),
                  c->connector_type_id);
    if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0 &&
        strcasecmp(name, want) == 0) {
      drmModeModeInfo mode = c->modes[0];  // preferred, else first
      for (int m = 0; m < c->count_modes; ++m)
        if ((c->modes[m].type & DRM_MODE_TYPE_PREFERRED) != 0) {
          mode = c->modes[m];
          break;
        }
      // Among CRTCs this connector's encoders can drive, prefer one that has an
      // NV12-capable plane; fall back to any compatible CRTC.
      uint32_t crtc_id = 0, crtc_fallback = 0;
      for (int e = 0; e < c->count_encoders && crtc_id == 0; ++e) {
        drmModeEncoder* enc = drmModeGetEncoder(fd, c->encoders[e]);
        if (enc == nullptr)
          continue;
        for (int j = 0; j < res->count_crtcs; ++j) {
          if ((enc->possible_crtcs & (1U << j)) == 0)
            continue;
          if (crtc_fallback == 0)
            crtc_fallback = res->crtcs[j];
          if ((vmask & (1U << j)) != 0) {
            crtc_id = res->crtcs[j];
            break;
          }
        }
        drmModeFreeEncoder(enc);
      }
      if (crtc_id == 0)
        crtc_id = crtc_fallback;
      if (crtc_id != 0)
        result = ConnectorSel{c->connector_id, crtc_id, mode};
    }
    drmModeFreeConnector(c);
  }
  drmModeFreeResources(res);
  return result;
}
}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, on_sigint);
  std::signal(SIGTERM, on_sigint);
  std::signal(SIGUSR1, on_sigusr1);  // screenshot the current frame

  // Pull our flags out of argv so the drm-cxx output picker doesn't see them.
  bool list_modes_flag = false;
  const char* drm_mode_sel = nullptr;
  std::vector<char*> av{argv[0]};
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--drm-list-modes")
      list_modes_flag = true;
    else if (a == "--drm-mode" && i + 1 < argc)
      drm_mode_sel = argv[++i];
    else if (a.rfind("--drm-mode=", 0) == 0)
      drm_mode_sel = argv[i] + 11;
    else
      av.push_back(argv[i]);
  }

  auto out = drm::examples::open_and_pick_output(static_cast<int>(av.size()),
                                                 av.data());
  if (!out) {
    std::fprintf(
        stderr,
        "failed to open a DRM output — run on a free VT; try --no-seat\n");
    return 1;
  }
  auto& dev = out->device;
  // Optional: target a specific display (CARLINKIT_CONNECTOR=DP-1, HDMI-A-1, …)
  // instead of the first connected one.
  if (const char* cn = std::getenv("CARLINKIT_CONNECTOR"); cn != nullptr) {
    if (auto s = select_connector(dev.fd(), cn); s) {
      out->connector_id = s->connector_id;
      out->crtc_id = s->crtc_id;
      out->mode = s->mode;
      std::fprintf(stderr, "using connector %s\n", cn);
    } else {
      std::fprintf(stderr,
                   "connector %s not found/connected; keeping default\n", cn);
    }
  }
  if (const char* cr = std::getenv("CARLINKIT_CRTC"); cr != nullptr)
    out->crtc_id = static_cast<uint32_t>(std::strtoul(cr, nullptr, 0));
  // --drm-list-modes: print the chosen connector's modes and exit.
  if (list_modes_flag) {
    list_modes(dev.fd(), out->connector_id);
    return 0;
  }
  // Keep the panel's native (preferred) mode unless explicitly overridden — a
  // non-native mode makes the monitor rescale and distort aspect.
  if (auto m = forced_mode(dev.fd(), out->connector_id,
                           std::getenv("DRM_FORCE_MODE"));
      m)
    out->mode = *m;
  // --drm-mode (index or WxH[@R]) takes precedence; it sets resolution, fps,
  // and DPI together via the derivation below. Useful for cycling modes by eye.
  if (drm_mode_sel != nullptr) {
    if (auto m = pick_mode(dev.fd(), out->connector_id, drm_mode_sel); m) {
      out->mode = *m;
    } else {
      std::fprintf(stderr, "--drm-mode '%s' not found; try --drm-list-modes\n",
                   drm_mode_sel);
      return 1;
    }
  }
  std::fprintf(stderr, "connector=%u crtc=%u\n", out->connector_id,
               out->crtc_id);
  const uint32_t W = out->mode.hdisplay, H = out->mode.vdisplay;
  std::fprintf(stderr, "display %ux%u@%uHz\n", W, H, out->mode.vrefresh);

  // Projection resolution defaults to the DRM mode (1:1, no scaling); fps from
  // the mode's refresh (capped); DPI from the panel's physical size. Each is
  // overridable via CARLINKIT_* (see config_env.h / README), as are the box
  // settings (Android Auto canvas, drive position, GNSS/dashboard/BT, …).
  ck::DongleConfig dcfg;
  dcfg.width = W;
  dcfg.height = H;
  dcfg.fps =
      std::min<uint32_t>(out->mode.vrefresh != 0 ? out->mode.vrefresh : 60, 60);
  dcfg.dpi = computed_dpi(dev.fd(), out->connector_id, W);
  ck::apply_box_env(dcfg);
  const uint32_t vw = dcfg.width, vh = dcfg.height;
  std::fprintf(stderr, "requesting %ux%u@%ufps dpi=%u (AA %ux%u)\n", vw, vh,
               dcfg.fps, dcfg.dpi, dcfg.aaWidth != 0 ? dcfg.aaWidth : vw,
               dcfg.aaHeight != 0 ? dcfg.aaHeight : vh);

  // Optional display rotation (CARLINKIT_ROTATE=90|180|270). Prefer the HW
  // plane: if an NV12 plane on this CRTC advertises the requested angle the
  // plane rotates the scanned-out buffer for free and the source stays
  // unrotated; only when no plane can does the software backend bake rotation
  // into its CPU convert (VAAPI/V4L2 never bake -- they rotate on a plane or
  // not at all). The plane query needs the scene, so the decision lands below.
  const uint64_t rot = requested_rotation();

  drm::scene::LayerScene::Config cfg;
  cfg.crtc_id = out->crtc_id;
  cfg.connector_id = out->connector_id;
  cfg.mode = out->mode;
  auto scene_r = drm::scene::LayerScene::create(dev, cfg);
  if (!scene_r) {
    std::fprintf(stderr, "LayerScene::create: %s\n",
                 scene_r.error().message().c_str());
    return 1;
  }
  auto scene = std::move(*scene_r);

  // Decide who rotates: the plane (if it advertises the angle), a GPU stage (if
  // not, and EGL/GLES are present), or the software backend as a last resort.
  const uint64_t plane_can_do = scene->candidate_rotation(DRM_FORMAT_NV12);
  const bool plane_can_rot = rot != 0 && (plane_can_do & rot) == rot;

  std::unique_ptr<ck::DecoderSource> src;
  bool gpu_rotated = false;
#ifdef CARLINKIT_GL
  // CARLINKIT_ROTATE_GPU forces the GPU stage even when the plane could rotate,
  // for testing the pass on hardware whose plane already does 90/270.
  const bool force_gpu = std::getenv("CARLINKIT_ROTATE_GPU") != nullptr;
  // The GPU stage needs a software inner: it consumes CPU YUV frames, which the
  // HW decoders don't hand out. When the decoder is Auto (or Software) build
  // one automatically, so a 90/270 the plane can't do just works instead of
  // dropping to slow CPU pre-rotation -- no CARLINKIT_DECODER=software needed.
  // An explicit VAAPI/V4L2 pin opts out (and gets the pre-rotation warning).
  const ck::DecoderBackend pref = ck::decoder_preference();
  const bool may_use_sw =
      pref == ck::DecoderBackend::Auto || pref == ck::DecoderBackend::Software;
  if (rot != 0 && (!plane_can_rot || force_gpu) && may_use_sw) {
    // The inner decodes unrotated and GlRotateSource bakes the angle into an
    // XRGB buffer the plane scans out. 90/270 swap the output extent.
    if (auto inner = ck::create_decoder_source(dev, vw, vh, DRM_MODE_ROTATE_0,
                                               ck::DecoderBackend::Software)) {
      const bool sw = rot == DRM_MODE_ROTATE_90 || rot == DRM_MODE_ROTATE_270;
      if (auto gl = ck::GlRotateSource::create(
              dev, std::move(inner), rot, sw ? vh : vw, sw ? vw : vh,
              scene->candidate_modifiers(DRM_FORMAT_ARGB8888))) {
        src = std::move(gl);
        gpu_rotated = true;
      }
    }
  }
#endif
  if (!src) {
    // The plane rotates it (residual 0), or the software backend bakes it.
    src = ck::create_decoder_source(dev, vw, vh, plane_can_rot ? 0 : rot);
  }
  if (!src) {
    std::fprintf(stderr, "no video decoder available\n");
    return 1;
  }

  // A source that baked the rotation in (the software backend) reports it via
  // applied_rotation(); whatever it did not handle is left for the HW plane.
  // The plane angle drives swap_wh, because the source's own buffer is already
  // in its final orientation (format() reflects it), so the on-screen aspect is
  // only swapped by what the plane still rotates.
  const uint64_t applied = src->applied_rotation();
  const uint64_t plane_rot = applied != 0 ? 0 : rot;
  const bool swap_wh =
      plane_rot == DRM_MODE_ROTATE_90 || plane_rot == DRM_MODE_ROTATE_270;
  if (rot != 0) {
    const unsigned deg = rot == DRM_MODE_ROTATE_90    ? 90
                         : rot == DRM_MODE_ROTATE_180 ? 180
                                                      : 270;
    if (gpu_rotated) {
      std::fprintf(stderr,
                   "rotating video %u degrees on the GPU (software decode)\n",
                   deg);
    } else if (applied != 0) {
      std::fprintf(stderr, "rotating video %u degrees in software (CPU)\n",
                   deg);
    } else if (plane_can_rot) {
      std::fprintf(stderr, "rotating video %u degrees on the HW plane\n", deg);
    } else {
      std::fprintf(
          stderr,
          "\n"
          "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n"
          "!!!  SOFTWARE ROTATION WARNING\n"
          "!!!  No NV12 hardware plane on this output supports %u-degree\n"
          "!!!  rotation. drm-cxx will fall back to CPU pre-rotation /\n"
          "!!!  composition, which defeats the zero-copy path, is very\n"
          "!!!  slow, and may fail outright for the tiled video buffer.\n"
          "!!!  Prefer an unrotated panel or a plane with rotation support.\n"
          "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n\n",
          deg);
    }
  }
  // Source rect = the source buffer's own dimensions (format() already reflects
  // any rotation the source applied); read before the source is moved away.
  const drm::scene::SourceFormat sf0 = src->format();

  drm::scene::LayerDesc desc;
  desc.source = std::move(src);
  // Aspect-preserving fit of the video into the W x H display, done by the HW
  // plane scaler (zero CPU). fv* is the final on-screen video rect; it is
  // reduced to native size below if the plane turns out not to scale. Under
  // 90/270 plane rotation the content is portrait, so fit the swapped extents.
  const uint32_t ivw = swap_wh ? sf0.height : sf0.width;
  const uint32_t ivh = swap_wh ? sf0.width : sf0.height;
  const double fit = std::min(double(W) / ivw, double(H) / ivh);
  int fvw = static_cast<int>(std::lround(ivw * fit));
  int fvh = static_cast<int>(std::lround(ivh * fit));
  int fvx = (int(W) - fvw) / 2;
  int fvy = (int(H) - fvh) / 2;
  desc.display.src_rect = drm::scene::Rect{0, 0, sf0.width, sf0.height};
  desc.display.dst_rect =
      drm::scene::Rect{fvx, fvy, uint32_t(fvw), uint32_t(fvh)};
  if (plane_rot != 0)
    desc.display.rotation = plane_rot;
  desc.content_type = drm::planes::ContentType::Video;
  auto lh = scene->add_layer(std::move(desc));
  if (!lh) {
    std::fprintf(stderr, "add_layer: %s\n", lh.error().message().c_str());
    return 1;
  }
  auto* vsrc =
      dynamic_cast<ck::DecoderSource*>(&scene->get_layer(*lh)->source());

  // Audio: ALSA playback of the dongle PCM + on-demand mic capture. Devices are
  // overridable (e.g. CARLINKIT_AUDIO_DEV=plughw:1,3 for HDMI/monitor speakers
  // when PipeWire isn't routing "default").
  const char* audio_dev = std::getenv("CARLINKIT_AUDIO_DEV");
  const char* mic_dev = std::getenv("CARLINKIT_MIC_DEV");
  ck::AudioMixer audio_out(audio_dev != nullptr ? audio_dev : "default");
  audio_out.start();
  // `mgr` is set right after the manager is constructed; the mic callback
  // captures it to break the sink <-> mic <-> manager construction cycle.
  ck::DongleManager* mgr = nullptr;
  ck::AudioInput mic(mic_dev != nullptr ? mic_dev : "default",
                     [&mgr](const int16_t* pcm, size_t n) {
                       if (mgr != nullptr)
                         mgr->send(ck::send_audio(pcm, n));
                     });

  // Optional frame-rate profiling (CARLINKIT_FPS_LOG=1): `video` counts decoded
  // frames the dongle delivers; `display` counts page flips (vsync-bound).
  const bool fps_log = std::getenv("CARLINKIT_FPS_LOG") != nullptr;
  std::atomic<uint32_t> vframe_count{0};

  ck::DongleSink sink;
  sink.on_video = [&](const ck::VideoFrame& f) {
    vsrc->submit_bitstream(f.data, f.size, 0);
    vframe_count.fetch_add(1, std::memory_order_relaxed);
  };
  sink.on_audio = [&](const ck::AudioFrame& f) {
    audio_out.submit(f);  // PCM mixing + ducking handled by the mixer
    if (f.command >= 0) {
      switch (static_cast<ck::AudioCommand>(f.command)) {
        case ck::AudioCommand::InputConfig:
        case ck::AudioCommand::PhonecallStart:
        case ck::AudioCommand::SiriStart:
          mic.start();
          break;
        case ck::AudioCommand::PhonecallStop:
        case ck::AudioCommand::SiriStop:
          mic.stop();
          break;
        default:
          break;
      }
    }
  };
  sink.on_plugged = [&](ck::PhoneType t, bool wifi) {
    std::fprintf(stderr, "[plugged] type=%d wifi=%d\n", int(t), wifi);
  };
  // The CarPlay "car logo" button (return to the vehicle's own UI) arrives as a
  // RequestHostUI command. This receiver has no separate host UI, so log it and
  // exit cleanly.
  sink.on_command = [&](ck::Command c) {
    if (c == ck::Command::RequestHostUI) {
      std::fprintf(stderr, "[command] RequestHostUI (car logo) — exiting\n");
      g_quit = true;
    }
  };

  // Hotplug supervisor: connects/reconnects the dongle automatically and
  // survives surprise removal (the panel keeps the last frame until video
  // resumes).
  ck::DongleManager manager(dcfg, sink);
  mgr = &manager;
  manager.set_on_disconnected([] {
    std::fprintf(stderr, "[dongle] disconnected — waiting for reconnect\n");
  });
  // Optional OEM branding: CARLINKIT_OEM_ICON=<path-to-png> sets the dongle's
  // own launcher tile (CARLINKIT_OEM_LABEL is the optional caption). Pushed on
  // each connect, before the phone pairs.
  const char* oem_icon = std::getenv("CARLINKIT_OEM_ICON");
  const char* oem_label = std::getenv("CARLINKIT_OEM_LABEL");
  if (oem_icon != nullptr) {
    manager.set_on_connected([oem_icon, oem_label, &manager] {
      std::ifstream f(oem_icon, std::ios::binary);
      std::vector<uint8_t> png((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
      if (png.empty()) {
        std::fprintf(stderr, "[oem-icon] could not read %s\n", oem_icon);
        return;
      }
      for (auto& frame : ck::send_oem_icon(
               png.data(), png.size(), oem_label != nullptr ? oem_label : ""))
        manager.send(std::move(frame));
      std::fprintf(stderr, "[oem-icon] sent %s (%zu bytes)\n", oem_icon,
                   png.size());
    });
  }
  std::fprintf(stderr, "waiting for dongle + phone (wifi+BT on, nearby)...\n");
  manager.start();

  // Preroll: wait (indefinitely, until quit) for the first decoded frame so the
  // first commit arms the plane. This also covers "dongle not connected yet".
  bool got = false;
  while (!g_quit && !got) {
    auto a = vsrc->acquire();
    if (a) {
      vsrc->release(std::move(*a));
      got = true;
    } else {
      usleep(50 * 1000);
    }
  }
  if (!got) {  // quit before any video arrived
    manager.stop();
    return 0;
  }

  // The phone may not honor our requested size, so use the ACTUAL decoded
  // dimensions (known after the first frame) for both the source rect and the
  // aspect-preserving fit — otherwise the plane stretches the wrong region.
  const auto sf = vsrc->format();
  const uint32_t aw = sf.width != 0 ? sf.width : vw;
  const uint32_t ah = sf.height != 0 ? sf.height : vh;
  // On-screen content dimensions: 90/270 rotation turns the landscape video
  // portrait, so fit (and the native fallback) use the swapped extents. The
  // source rect stays the decoded frame; the plane does the rotation.
  const uint32_t ew = swap_wh ? ah : aw;
  const uint32_t eh = swap_wh ? aw : ah;
  const double afit = std::min(double(W) / ew, double(H) / eh);
  fvw = static_cast<int>(std::lround(ew * afit));
  fvh = static_cast<int>(std::lround(eh * afit));
  fvx = (int(W) - fvw) / 2;
  fvy = (int(H) - fvh) / 2;
  auto* layer = scene->get_layer(*lh);
  layer->set_src_rect(drm::scene::Rect{0, 0, aw, ah});
  layer->set_dst_rect(drm::scene::Rect{fvx, fvy, uint32_t(fvw), uint32_t(fvh)});
  std::fprintf(stderr, "video %ux%u -> dst %dx%d at (%d,%d) on %ux%u%s\n", aw,
               ah, fvw, fvh, fvx, fvy, W, H, rot != 0 ? " (rotated)" : "");
  // Probe HW plane scaling; fall back to native size centered if unsupported.
  if ((fvw != int(ew) || fvh != int(eh)) && !scene->test()) {
    const int nw = std::min<int>(int(ew), int(W));
    const int nh = std::min<int>(int(eh), int(H));
    fvx = (int(W) - nw) / 2;
    fvy = (int(H) - nh) / 2;
    fvw = nw;
    fvh = nh;
    layer->set_dst_rect(
        drm::scene::Rect{fvx, fvy, uint32_t(fvw), uint32_t(fvh)});
    std::fprintf(stderr, "HW plane can't scale; native %dx%d centered\n", nw,
                 nh);
  }

  std::atomic<bool> flip_pending{false};
  uint32_t flip_count =
      0;  // page flips since the last fps report (main thread)
  drm::PageFlip page_flip(dev);
  page_flip.set_handler([&](uint32_t, uint64_t, uint64_t) {
    flip_pending = false;
    ++flip_count;
  });

  if (auto r = scene->commit(DRM_MODE_PAGE_FLIP_EVENT, &page_flip); !r) {
    std::fprintf(stderr, "first commit: %s\n", r.error().message().c_str());
    manager.stop();
    return 1;
  }
  flip_pending = true;

  // Touch / pointer input -> dongle touch messages (mapped into the video
  // rect).
  const int rot_deg = rot == DRM_MODE_ROTATE_90    ? 90
                      : rot == DRM_MODE_ROTATE_180 ? 180
                      : rot == DRM_MODE_ROTATE_270 ? 270
                                                   : 0;
  ck::TouchMapper mapper(
      int(W), int(H), fvx, fvy, fvw, fvh,
      [&](float x, float y, ck::TouchAction a) { manager.send_touch(x, y, a); },
      rot_deg);
  if (const char* g = std::getenv("CARLINKIT_POINTER_GAIN"); g != nullptr)
    mapper.set_pointer_gain(std::strtod(g, nullptr));
  if (const char* g = std::getenv("CARLINKIT_DRAG_GAIN"); g != nullptr)
    mapper.set_drag_gain(std::strtod(g, nullptr));
  if (const char* h = std::getenv("CARLINKIT_TOUCH_HZ"); h != nullptr)
    mapper.set_touch_rate_hz(std::strtod(h, nullptr));
  std::optional<drm::input::Seat> seat;
  if (auto sr = drm::input::Seat::open(); sr) {
    seat.emplace(std::move(*sr));
    seat->set_event_handler(
        [&](const drm::input::InputEvent& e) { mapper.handle(e); });
  } else {
    std::fprintf(stderr, "input disabled (seat open failed: %s)\n",
                 sr.error().message().c_str());
  }

  // Desktop HW cursor: a dot on the cursor plane that follows the mouse; the
  // same pointer position drives touch mapping (touchscreens never move it).
  std::optional<drm::cursor::Renderer> cursor;
  {
    constexpr uint32_t kCs = 24;
    std::vector<uint32_t> px(static_cast<size_t>(kCs) * kCs, 0);
    const double c = kCs / 2.0;
    for (uint32_t y = 0; y < kCs; ++y)
      for (uint32_t x = 0; x < kCs; ++x) {
        const double d = std::hypot(x + 0.5 - c, y + 0.5 - c);
        if (d <= 8.0)
          px[y * kCs + x] = 0xFFFFFFFFU;  // white fill
        else if (d <= 10.5)
          px[y * kCs + x] = 0xFF000000U;  // black ring for contrast
      }
    drm::cursor::RendererConfig ccfg;
    ccfg.crtc_id = out->crtc_id;
    if (auto cr = drm::cursor::Renderer::create(dev, ccfg); cr) {
      cursor.emplace(std::move(*cr));
      if (auto cur = drm::cursor::Cursor::from_argb(
              drm::span<const uint32_t>(px.data(), px.size()), kCs, kCs,
              kCs / 2, kCs / 2);
          cur) {
        (void)cursor->set_cursor(std::move(*cur));
        (void)cursor->move_to(int(W) / 2, int(H) / 2);
      }
    } else {
      std::fprintf(stderr, "cursor disabled: %s\n",
                   cr.error().message().c_str());
    }
  }

  // Dedicated input thread: dispatch libinput continuously so events never
  // queue behind the render/commit cadence (avoids the "lagging behind"
  // warnings).
  std::thread input_thread;
  if (seat) {
    input_thread = std::thread([&] {
      while (!g_quit && !mapper.quit()) {
        pollfd ip{seat->fd(), POLLIN, 0};
        if (::poll(&ip, 1, 50) > 0 && (ip.revents & POLLIN))
          (void)seat->dispatch();
      }
    });
  }

  std::fprintf(stderr, "playing — Esc/Q/Ctrl-C to quit\n");

  // Render loop: page-flip pacing + one coalesced cursor move + scene commit.
  const char* capture_dir = std::getenv("CARLINKIT_CAPTURE_DIR");
  if (capture_dir == nullptr)
    capture_dir = ".";
  auto fps_t0 = std::chrono::steady_clock::now();
  // Per-window timing (CARLINKIT_FPS_LOG): where does each frame's time go —
  // blocking in scene->commit(), or waiting for the flip event in poll()?
  double commit_us_sum = 0, commit_us_max = 0, poll_us_sum = 0, poll_us_max = 0;
  uint32_t commit_n = 0;
  const auto us_since = [](std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::micro>(
               std::chrono::steady_clock::now() - t)
        .count();
  };
  while (!g_quit && !mapper.quit()) {
    if (g_capture.exchange(false) && vsrc != nullptr)
      vsrc->request_capture(capture_dir);  // written by the next decoded frame
    pollfd p{dev.fd(), POLLIN, 0};
    const auto poll_t0 = std::chrono::steady_clock::now();
    if (::poll(&p, 1, flip_pending ? 100 : 8) > 0 && (p.revents & POLLIN))
      (void)page_flip.dispatch(0);
    if (fps_log) {
      const double us = us_since(poll_t0);
      poll_us_sum += us;
      poll_us_max = std::max(poll_us_max, us);
    }
    if (cursor && mapper.take_cursor_dirty())
      (void)cursor->move_to(mapper.cursor_x(), mapper.cursor_y());
    // Skip the flip when the source has no new frame: the panel keeps scanning
    // out the current buffer, so a static screen costs no commits. The GPU
    // rotate path reports staleness via has_fresh_content(); the other backends
    // report fresh unconditionally (default), so their cadence is unchanged.
    // Single video layer, so the source's freshness is the scene's.
    if (!flip_pending && vsrc->has_fresh_content()) {
      const auto commit_t0 = std::chrono::steady_clock::now();
      // Non-blocking: submit the flip and return immediately rather than block
      // ~one vblank inside the ioctl. The page-flip event (polled above) drives
      // the next commit, so the loop isn't gated by the commit's duration — the
      // kernel paces the flip to vblank. (The first commit, which modesets,
      // stays blocking — async modeset isn't allowed.)
      auto r = scene->commit(
          DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK, &page_flip);
      if (fps_log) {
        const double us = us_since(commit_t0);
        commit_us_sum += us;
        commit_us_max = std::max(commit_us_max, us);
        ++commit_n;
      }
      if (!r) {
        std::fprintf(stderr, "commit: %s\n", r.error().message().c_str());
        break;
      }
      flip_pending = true;
    }
    if (fps_log) {
      const auto now = std::chrono::steady_clock::now();
      const double secs = std::chrono::duration<double>(now - fps_t0).count();
      if (secs >= 1.0) {
        const double cn = commit_n > 0 ? commit_n : 1;
        std::fprintf(stderr,
                     "[fps] video=%.1f display=%.1f | commit avg=%.0f max=%.0f "
                     "| poll avg=%.0f max=%.0f us\n",
                     vframe_count.exchange(0) / secs, flip_count / secs,
                     commit_us_sum / cn, commit_us_max, poll_us_sum / cn,
                     poll_us_max);
        flip_count = 0;
        fps_t0 = now;
        commit_us_sum = commit_us_max = poll_us_sum = poll_us_max = 0;
        commit_n = 0;
      }
    }
  }

  std::fprintf(stderr, "stopping\n");
  if (input_thread.joinable())
    input_thread.join();
  manager.stop();
  mic.stop();
  audio_out.stop();
  return 0;
}
