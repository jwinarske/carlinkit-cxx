// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
// carlinkit-kms — live CarPlay/Android Auto on a hardware plane (zero-copy).
//
// Receives the dongle's H.264 over USB, HW-decodes it to NV12 DMA-BUF
// (libavcodec+VAAPI), and scans it out on a KMS plane via drm-cxx.
//
// MUST run on a free VT with DRM master (Ctrl+Alt+F3), NOT inside a desktop or
// over SSH. Usage:  carlinkit-kms [/dev/dri/cardN] [--no-seat]
#include <poll.h>
#include <strings.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include <drm_fourcc.h>
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
#include "dongle.h"
#include "dongle_manager.h"
#include "input_touch.h"
#include "vaapi_decoder_source.h"

namespace {
std::atomic<bool> g_quit{false};
void on_sigint(int) {
  g_quit = true;
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

  auto out = drm::examples::open_and_pick_output(argc, argv);
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
  // Keep the panel's native (preferred) mode unless explicitly overridden — a
  // non-native mode makes the monitor rescale and distort aspect.
  if (auto m = forced_mode(dev.fd(), out->connector_id,
                           std::getenv("DRM_FORCE_MODE"));
      m)
    out->mode = *m;
  std::fprintf(stderr, "connector=%u crtc=%u\n", out->connector_id,
               out->crtc_id);
  const uint32_t W = out->mode.hdisplay, H = out->mode.vdisplay;
  std::fprintf(stderr, "display %ux%u@%uHz\n", W, H, out->mode.vrefresh);

  // CarPlay renders at this (landscape) size; the HW plane scales it to fit the
  // panel below while preserving aspect ratio.
  const uint32_t vw = 1280, vh = 720;

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

  auto src = ck::VaapiDecoderSource::create(dev, vw, vh);
  if (!src) {
    std::fprintf(stderr, "VaapiDecoderSource::create failed\n");
    return 1;
  }

  drm::scene::LayerDesc desc;
  desc.source = std::move(src);
  // Aspect-preserving fit of the vw x vh video into the W x H display, done by
  // the HW plane scaler (zero CPU). fv* is the final on-screen video rect; it
  // is reduced to native size below if the plane turns out not to scale.
  const double fit = std::min(double(W) / vw, double(H) / vh);
  int fvw = static_cast<int>(std::lround(vw * fit));
  int fvh = static_cast<int>(std::lround(vh * fit));
  int fvx = (int(W) - fvw) / 2;
  int fvy = (int(H) - fvh) / 2;
  desc.display.src_rect = drm::scene::Rect{0, 0, vw, vh};
  desc.display.dst_rect =
      drm::scene::Rect{fvx, fvy, uint32_t(fvw), uint32_t(fvh)};
  desc.content_type = drm::planes::ContentType::Video;
  auto lh = scene->add_layer(std::move(desc));
  if (!lh) {
    std::fprintf(stderr, "add_layer: %s\n", lh.error().message().c_str());
    return 1;
  }
  auto* vsrc =
      dynamic_cast<ck::VaapiDecoderSource*>(&scene->get_layer(*lh)->source());

  ck::DongleConfig dcfg;
  dcfg.width = vw;
  dcfg.height = vh;
  dcfg.fps = 30;

  // Audio: ALSA playback of the dongle PCM + on-demand mic capture. Devices are
  // overridable (e.g. CARLINKIT_AUDIO_DEV=plughw:1,3 for HDMI/monitor speakers
  // when PipeWire isn't routing "default").
  const char* audio_dev = std::getenv("CARLINKIT_AUDIO_DEV");
  const char* mic_dev = std::getenv("CARLINKIT_MIC_DEV");
  ck::AudioOutput audio_out(audio_dev != nullptr ? audio_dev : "default");
  audio_out.start();
  // `mgr` is set right after the manager is constructed; the mic callback
  // captures it to break the sink <-> mic <-> manager construction cycle.
  ck::DongleManager* mgr = nullptr;
  ck::AudioInput mic(mic_dev != nullptr ? mic_dev : "default",
                     [&mgr](const int16_t* pcm, size_t n) {
                       if (mgr != nullptr)
                         mgr->send(ck::send_audio(pcm, n));
                     });

  ck::DongleSink sink;
  sink.on_video = [&](const ck::VideoFrame& f) {
    vsrc->submit_bitstream(f.data, f.size);
  };
  sink.on_audio = [&](const ck::AudioFrame& f) {
    if (f.pcm) {
      audio_out.submit(f.decodeType, f.pcm, f.samples);
    } else if (f.command >= 0) {
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

  // Hotplug supervisor: connects/reconnects the dongle automatically and
  // survives surprise removal (the panel keeps the last frame until video
  // resumes).
  ck::DongleManager manager(dcfg, sink);
  mgr = &manager;
  manager.set_on_disconnected([] {
    std::fprintf(stderr, "[dongle] disconnected — waiting for reconnect\n");
  });
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
  const double afit = std::min(double(W) / aw, double(H) / ah);
  fvw = static_cast<int>(std::lround(aw * afit));
  fvh = static_cast<int>(std::lround(ah * afit));
  fvx = (int(W) - fvw) / 2;
  fvy = (int(H) - fvh) / 2;
  auto* layer = scene->get_layer(*lh);
  layer->set_src_rect(drm::scene::Rect{0, 0, aw, ah});
  layer->set_dst_rect(drm::scene::Rect{fvx, fvy, uint32_t(fvw), uint32_t(fvh)});
  std::fprintf(stderr, "video %ux%u -> dst %dx%d at (%d,%d) on %ux%u\n", aw, ah,
               fvw, fvh, fvx, fvy, W, H);
  // Probe HW plane scaling; fall back to native size centered if unsupported.
  if ((fvw != int(aw) || fvh != int(ah)) && !scene->test()) {
    const int nw = std::min<int>(int(aw), int(W));
    const int nh = std::min<int>(int(ah), int(H));
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
  drm::PageFlip page_flip(dev);
  page_flip.set_handler(
      [&](uint32_t, uint64_t, uint64_t) { flip_pending = false; });

  if (auto r = scene->commit(DRM_MODE_PAGE_FLIP_EVENT, &page_flip); !r) {
    std::fprintf(stderr, "first commit: %s\n", r.error().message().c_str());
    manager.stop();
    return 1;
  }
  flip_pending = true;

  // Touch / pointer input -> dongle touch messages (mapped into the video
  // rect).
  ck::TouchMapper mapper(int(W), int(H), fvx, fvy, fvw, fvh,
                         [&](float x, float y, ck::TouchAction a) {
                           manager.send_touch(x, y, a);
                         });
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
  while (!g_quit && !mapper.quit()) {
    pollfd p{dev.fd(), POLLIN, 0};
    if (::poll(&p, 1, flip_pending ? 100 : 8) > 0 && (p.revents & POLLIN))
      (void)page_flip.dispatch(0);
    if (cursor && mapper.take_cursor_dirty())
      (void)cursor->move_to(mapper.cursor_x(), mapper.cursor_y());
    if (!flip_pending) {
      auto r = scene->commit(DRM_MODE_PAGE_FLIP_EVENT, &page_flip);
      if (!r) {
        std::fprintf(stderr, "commit: %s\n", r.error().message().c_str());
        break;
      }
      flip_pending = true;
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
