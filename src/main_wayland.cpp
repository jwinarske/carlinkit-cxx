// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
// carlinkit-wayland — present live CarPlay/Android Auto through a Wayland
// compositor (no DRM master, no free VT — runs as an ordinary client).
//
// The dongle's H.264 is HW-decoded to an NV12 DMA-BUF (headless VAAPI, no KMS
// device), imported into a wl_buffer (WaylandSink), and presented on a
// fullscreen xdg toplevel. Per-surface dmabuf feedback drives the zero-copy
// verdict; wl_surface.frame paces commits; wp_presentation reports whether the
// compositor scanned the buffer out directly (the ZERO_COPY flag).
//
// When VAAPI is unavailable it falls back to software (CPU) H.264 decode
// presented through a wl_shm XRGB pool, so it runs on any compositor.
#include <linux/input-event-codes.h>
#include <strings.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <drm_fourcc.h>
#include <xf86drm.h>

#include "audio_alsa.h"
#include "config_env.h"
#include "decoder_source.h"
#include "dongle.h"
#include "dongle_manager.h"
#include "protocol.h"
#include "software_decoder_source.h"
#include "vaapi_decoder_source.h"
#include "wayland_shm_sink.h"
#include "wayland_sink.h"

// wcs include order is load-bearing: each wl/*.hpp helper needs its generated
// <proto>_client.hpp (and, for dmabuf feedback, the wl/linux_dmabuf.hpp tables)
// included first, so shield the block from the formatter's alphabetical sort.
// clang-format off
#include <wayland-client-core.h>
#include <wayland-util.h>

#include "wayland_client.hpp"
#include "xdg_shell_client.hpp"
#include "linux_dmabuf_client.hpp"
#include "viewporter_client.hpp"
#include "presentation_time_client.hpp"

#include <wl/display.hpp>
#include <wl/registry.hpp>
#include <wl/client_helpers.hpp>
#include <wl/proxy_impl.hpp>
#include <wl/wl_ptr.hpp>
#include <wl/xdg_shell.hpp>
#include <wl/linux_dmabuf.hpp>
#include <wl/dmabuf_feedback.hpp>
#include <wl/presentation.hpp>
#include <wl/present_feedback.hpp>
// clang-format on

namespace {

std::atomic<bool> g_running{true};
void on_sigint(int) {
  g_running = false;
}

constexpr int kRoundtripTimeoutMs = 5000;
constexpr uint32_t kVideoFourcc = DRM_FORMAT_NV12;

// The render-node path for the DRM device the compositor decodes/scans out on
// (dmabuf feedback's main_device, a dev_t), or empty if none matches. Decoding
// on this device avoids a silent compositor-side cross-device copy that would
// defeat zero-copy. `want` is matched against every node of each DRM device.
std::string render_node_for_device(dev_t want) {
  if (want == 0)
    return {};
  drmDevicePtr devs[16];
  const int n = drmGetDevices2(0, devs, 16);
  if (n <= 0)
    return {};
  std::string result;
  for (int i = 0; i < n && result.empty(); ++i) {
    drmDevicePtr d = devs[i];
    bool match = false;
    for (int node = 0; node < DRM_NODE_MAX && !match; ++node) {
      if ((d->available_nodes & (1 << node)) == 0)
        continue;
      struct stat st{};
      if (stat(d->nodes[node], &st) == 0 && st.st_rdev == want)
        match = true;
    }
    if (match && (d->available_nodes & (1 << DRM_NODE_RENDER)) != 0)
      result = d->nodes[DRM_NODE_RENDER];
  }
  drmFreeDevices(devs, n);
  return result;
}

// Display/panel orientation is the compositor's output configuration
// (wl_output.transform) — it rotates the whole output and transforms input to
// match, transparently to clients. So carlinkit-wayland presents an unrotated
// buffer and does no rotation of its own (unlike carlinkit-kms, which drives
// the panel directly and must rotate itself).

class App;

// Event-less handlers (we only issue requests on these objects).
class WlCompositorHandler
    : public wayland::client::CWlCompositor<WlCompositorHandler> {};
class WlSurfaceHandler : public wayland::client::CWlSurface<WlSurfaceHandler> {
};
class WpViewporterHandler
    : public viewporter::client::CWpViewporter<WpViewporterHandler> {};
class WpViewportHandler
    : public viewporter::client::CWpViewport<WpViewportHandler> {};

// wl_surface.frame callback — fires once per displayed frame to pace commits.
class WlCallbackHandler
    : public wayland::client::CWlCallback<WlCallbackHandler> {
 public:
  App* app_ = nullptr;
  void OnDone(uint32_t time_ms) override;
};

// Pointer: a left-button drag is mapped to a touch (down/move/up). wl_pointer
// button events carry no position, so the last motion/enter point is tracked.
class WlPointerHandler : public wayland::client::CWlPointer<WlPointerHandler> {
 public:
  App* app_ = nullptr;
  double x_ = 0.0, y_ = 0.0;
  void OnEnter(uint32_t, wl_proxy*, wl_fixed_t sx, wl_fixed_t sy) override {
    x_ = wl_fixed_to_double(sx);
    y_ = wl_fixed_to_double(sy);
  }
  void OnMotion(uint32_t, wl_fixed_t sx, wl_fixed_t sy) override;
  void OnButton(uint32_t, uint32_t, uint32_t button, uint32_t state) override;
};

// Touch: the primary contact is forwarded to the dongle (single-point).
class WlTouchHandler : public wayland::client::CWlTouch<WlTouchHandler> {
 public:
  App* app_ = nullptr;
  void OnDown(uint32_t,
              uint32_t,
              wl_proxy*,
              int32_t id,
              wl_fixed_t x,
              wl_fixed_t y) override;
  void OnUp(uint32_t, uint32_t, int32_t id) override;
  void OnMotion(uint32_t, int32_t id, wl_fixed_t x, wl_fixed_t y) override;
};

// Seat: create the pointer / touch handlers as the capabilities advertise them.
class WlSeatHandler : public wayland::client::CWlSeat<WlSeatHandler> {
 public:
  App* app_ = nullptr;
  wl::WlPtr<WlPointerHandler> pointer_;
  wl::WlPtr<WlTouchHandler> touch_;
  void OnCapabilities(uint32_t caps) override;
};

// wl_shm: the format events are ignored (XRGB8888 is mandatory).
class WlShmHandler : public wayland::client::CWlShm<WlShmHandler> {};

class App {
 public:
  int Run();

  // ── wcs App hooks (SFINAE-detected / handler callbacks) ────────────────────
  void OnDmabufFeedback(const wl::FeedbackSnapshot& snap);
  void OnPresented(const wl::PresentFeedback& fb);
  void OnDiscarded(uint32_t /*frame*/) {}
  void OnXdgSurfaceConfigure(uint32_t /*serial*/) {}
  void OnToplevelConfigure(int32_t w, int32_t h);
  void OnToplevelClose() { g_running = false; }
  void OnFrameReady(uint32_t time_ms);

  // Input (forwarded by the seat/pointer/touch handlers). Surface-local coords
  // map straight to the dongle's [0,1] touch space: wp_viewport makes the
  // surface's logical size the fitted video rect.
  void OnPointerMotion(double x, double y);
  void OnPointerButton(double x, double y, uint32_t button, bool pressed);
  void OnTouchDown(int32_t id, double x, double y);
  void OnTouchMotion(int32_t id, double x, double y);
  void OnTouchUp(int32_t id);

 private:
  bool Connect();
  bool ScanGlobals();
  bool BindGlobals();
  bool CreateSurface();
  bool StartDecode();
  void ReportScanout(const wl::FeedbackSnapshot& snap);
  void RenderIfReady();
  void RequestFrameCallback();
  void SendTouchNorm(double x, double y, ck::TouchAction a);
  void SendTouchUp();

  wl::DisplayHandle display_;
  wl::CRegistry registry_;
  wl::DmabufFeedback<App> dmabuf_feedback_;
  wl::PresentationManager<App> presentation_;
  wl::WlPtr<WlSeatHandler> seat_;

  wl::WlPtr<WlCompositorHandler> compositor_;
  wl::WlPtr<wl::XdgWmBaseHandler> xdg_wm_base_;
  wl::WlPtr<WlSurfaceHandler> surface_;
  wl::WlPtr<wl::XdgSurfaceHandler<App>> xdg_surface_;
  wl::WlPtr<wl::XdgToplevelHandler<App>> xdg_toplevel_;
  wl::WlPtr<WpViewporterHandler> viewporter_;
  wl::WlPtr<WpViewportHandler> viewport_;
  wl::WlPtr<WlCallbackHandler> frame_callback_;
  wl::WlPtr<WlShmHandler> shm_;

  std::unique_ptr<ck::DecoderSource> decoder_;
  std::unique_ptr<ck::WaylandSink> sink_;    // dmabuf (VAAPI) path
  std::unique_ptr<ck::WlShmSink> shm_sink_;  // software (wl_shm) path
  bool use_shm_ = false;                     // software fallback engaged
  uint64_t last_shm_seq_ = 0;                // last presented CPU frame
  ck::DongleManager* mgr_ = nullptr;  // set in Run(); drives touch output

  int32_t surf_w_ = 0, surf_h_ = 0;  // surface logical size (viewport dest)
  bool ptr_down_ = false;            // pointer-drag as touch
  int32_t touch_id_ = -1;            // primary touch contact forwarded
  float last_nx_ = 0.5F, last_ny_ = 0.5F;  // last touch point (for a bare Up)

  uint32_t compositor_name_ = 0, compositor_ver_ = 0;
  uint32_t xdg_wm_base_name_ = 0, xdg_wm_base_ver_ = 0;
  uint32_t viewporter_name_ = 0, viewporter_ver_ = 0;
  uint32_t seat_name_ = 0, seat_ver_ = 0;
  uint32_t shm_name_ = 0, shm_ver_ = 0;
  bool have_compositor_ = false, have_xdg_ = false, have_dmabuf_ = false;
  bool have_viewporter_ = false, have_presentation_ = false, have_seat_ = false;
  bool have_shm_ = false;
  bool surface_feedback_started_ = false;
  bool frame_pending_ = false;  // a frame callback is in flight (pacing)
  uint32_t present_seq_ = 0;
  int32_t width_ = 0, height_ = 0;  // configured (fullscreen) surface size
};

void WlCallbackHandler::OnDone(uint32_t time_ms) {
  if (app_ != nullptr)
    app_->OnFrameReady(time_ms);
}

void App::OnDmabufFeedback(const wl::FeedbackSnapshot& snap) {
  // Per-surface feedback re-advertises on fullscreen/output changes;
  // re-evaluate the scanout verdict every time rather than caching it.
  if (surface_feedback_started_)
    ReportScanout(snap);
}

void App::OnPresented(const wl::PresentFeedback& fb) {
  static bool logged_zero_copy = false;
  const bool zero_copy =
      (fb.flags &
       static_cast<uint32_t>(
           presentation_time::client::WpPresentationFeedbackKind::ZeroCopy)) !=
      0;
  // Log the first present and every zero-copy transition, not every frame.
  if (fb.frame == 0 || zero_copy != logged_zero_copy) {
    std::fprintf(stderr, "[present] frame=%u flags=0x%x zero_copy=%d\n",
                 fb.frame, fb.flags, zero_copy ? 1 : 0);
    logged_zero_copy = zero_copy;
  }
}

void App::OnToplevelConfigure(int32_t w, int32_t h) {
  if (w > 0 && h > 0 && (w != width_ || h != height_)) {
    width_ = w;
    height_ = h;
    std::fprintf(stderr, "[configure] %dx%d\n", w, h);
  }
}

void App::ReportScanout(const wl::FeedbackSnapshot& snap) {
  const std::vector<uint64_t> present = snap.ModifiersFor(kVideoFourcc);
  const std::vector<uint64_t> scanout = snap.ScanoutModifiersFor(kVideoFourcc);
  std::fprintf(stderr,
               "[dmabuf] NV12: %zu presentable modifier(s), %zu scanout "
               "modifier(s)\n",
               present.size(), scanout.size());
  if (scanout.empty())
    std::fprintf(stderr,
                 "[dmabuf] no NV12 scanout modifier — direct scanout "
                 "unavailable; compositor would GPU-composite\n");
}

bool App::Connect() {
  if (!display_.Connect()) {
    std::fprintf(stderr, "cannot connect to a Wayland display\n");
    return false;
  }
  return true;
}

bool App::ScanGlobals() {
  if (!registry_.Create(display_.Get())) {
    std::fprintf(stderr, "wl_display.get_registry failed\n");
    return false;
  }
  registry_.OnGlobal([this](wl::CRegistry&, uint32_t name,
                            std::string_view iface, uint32_t ver) {
    using namespace wayland::client;
    if (iface == wl_compositor_traits::interface_name) {
      compositor_name_ = name;
      compositor_ver_ = ver;
      have_compositor_ = true;
    } else if (iface == xdg_shell::client::xdg_wm_base_traits::interface_name) {
      xdg_wm_base_name_ = name;
      xdg_wm_base_ver_ = ver;
      have_xdg_ = true;
    } else if (iface == linux_dmabuf_unstable_v1::client::
                            zwp_linux_dmabuf_v1_traits::interface_name) {
      dmabuf_feedback_.Record(name, ver);
      have_dmabuf_ = true;
    } else if (iface ==
               viewporter::client::wp_viewporter_traits::interface_name) {
      viewporter_name_ = name;
      viewporter_ver_ = ver;
      have_viewporter_ = true;
    } else if (iface == wl_seat_traits::interface_name) {
      seat_name_ = name;
      seat_ver_ = ver;
      have_seat_ = true;
    } else if (iface == wl_shm_traits::interface_name) {
      shm_name_ = name;
      shm_ver_ = ver;
      have_shm_ = true;
    } else if (iface == presentation_time::client::wp_presentation_traits::
                            interface_name) {
      presentation_.Record(name, ver);
    }
  });
  if (!wl::RoundtripWithTimeout(display_.Get(), kRoundtripTimeoutMs)) {
    std::fprintf(stderr, "registry roundtrip timed out\n");
    return false;
  }
  if (!have_compositor_ || !have_xdg_ || !have_dmabuf_) {
    std::fprintf(stderr,
                 "missing required global(s): compositor=%d xdg_wm_base=%d "
                 "linux_dmabuf=%d\n",
                 have_compositor_, have_xdg_, have_dmabuf_);
    return false;
  }
  return true;
}

bool App::BindGlobals() {
  using namespace wayland::client;
  if (wl_proxy* raw = registry_.Bind<wl_compositor_traits>(
          compositor_name_,
          std::min(compositor_ver_, wl_compositor_traits::version)))
    compositor_.Attach(raw);
  else
    return false;

  if (!wl::BindHandler<xdg_shell::client::xdg_wm_base_traits>(
          registry_, xdg_wm_base_, xdg_wm_base_name_, xdg_wm_base_ver_)) {
    std::fprintf(stderr, "bind xdg_wm_base failed\n");
    return false;
  }

  if (have_viewporter_) {
    if (wl_proxy* raw =
            registry_.Bind<viewporter::client::wp_viewporter_traits>(
                viewporter_name_,
                std::min(viewporter_ver_,
                         viewporter::client::wp_viewporter_traits::version)))
      viewporter_.Attach(raw);
  }

  if (!dmabuf_feedback_.Bind(registry_, this)) {
    std::fprintf(stderr, "bind linux_dmabuf failed\n");
    return false;
  }
  have_presentation_ = presentation_.Bind(registry_, this);
  if (have_seat_ && wl::BindHandler<wayland::client::wl_seat_traits>(
                        registry_, seat_, seat_name_, seat_ver_))
    seat_.Get()->app_ = this;
  if (have_shm_)
    (void)wl::BindHandler<wayland::client::wl_shm_traits>(registry_, shm_,
                                                          shm_name_, shm_ver_);

  if (dmabuf_feedback_.BoundVersion() >= 4)
    (void)dmabuf_feedback_.StartDefault(display_.Get());
  if (!wl::RoundtripWithTimeout(display_.Get(), kRoundtripTimeoutMs))
    return false;
  if (dmabuf_feedback_.BoundVersion() < 4)
    dmabuf_feedback_.CommitLegacy();

  if (dmabuf_feedback_.Current().ModifiersFor(kVideoFourcc).empty()) {
    std::fprintf(stderr,
                 "compositor advertises no NV12 dmabuf support — cannot "
                 "present the decoded video\n");
    return false;
  }
  return true;
}

bool App::CreateSurface() {
  using namespace wayland::client;
  if (wl_proxy* raw = wl::construct<wl_surface_traits,
                                    wl_compositor_traits::Op::CreateSurface>(
          *compositor_.Get()))
    surface_.Get()->_SetProxy(raw);
  else
    return false;

  if (!wl::SetupHandler(
          xdg_surface_,
          wl::construct<
              xdg_shell::client::xdg_surface_traits,
              xdg_shell::client::xdg_wm_base_traits::Op::GetXdgSurface>(
              *xdg_wm_base_.Get(), surface_.Get()->GetProxy()))) {
    std::fprintf(stderr, "get_xdg_surface failed\n");
    return false;
  }
  xdg_surface_.Get()->app_ = this;

  if (!wl::SetupHandler(
          xdg_toplevel_,
          wl::construct<xdg_shell::client::xdg_toplevel_traits,
                        xdg_shell::client::xdg_surface_traits::Op::GetToplevel>(
              *xdg_surface_.Get()))) {
    std::fprintf(stderr, "get_toplevel failed\n");
    return false;
  }
  auto* toplevel = xdg_toplevel_.Get();
  toplevel->app_ = this;
  toplevel->SetTitle("carlinkit");
  toplevel->SetAppId("org.carlinkit.wayland");
  toplevel->SetFullscreen(nullptr);  // compositor chooses the output

  // Opaque video surface with no alpha: a viewport scales the decoded buffer to
  // an aspect-fitted destination the compositor centers within the output.
  if (viewporter_.Get() != nullptr &&
      viewporter_.Get()->GetProxy() != nullptr) {
    if (wl_proxy* raw = wl::construct<
            viewporter::client::wp_viewport_traits,
            viewporter::client::wp_viewporter_traits::Op::GetViewport>(
            *viewporter_.Get(), surface_.Get()->GetProxy()))
      viewport_.Attach(raw);  // event-less: attach without a dispatcher
  }

  surface_.Get()->Commit();
  if (!wl::RoundtripWithTimeout(display_.Get(), kRoundtripTimeoutMs))
    return false;

  // Per-surface feedback: its tranches reflect this surface's real plane-
  // assignment potential (the scanout flag), unlike the default feedback.
  if (dmabuf_feedback_.BoundVersion() >= 4) {
    surface_feedback_started_ = dmabuf_feedback_.StartSurface(
        display_.Get(), surface_.Get()->GetProxy());
    (void)wl::RoundtripWithTimeout(display_.Get(), kRoundtripTimeoutMs);
  }
  ReportScanout(dmabuf_feedback_.Current());
  return true;
}

bool App::StartDecode() {
  ck::DongleConfig cfg;
  ck::apply_box_env(cfg);

  const char* pref = std::getenv("CARLINKIT_DECODER");
  const bool force_sw = pref != nullptr && (strcasecmp(pref, "software") == 0 ||
                                            strcasecmp(pref, "sw") == 0);
  if (!force_sw) {
    // VAAPI headless (dmabuf, zero-copy-capable). Prefer the render node of the
    // compositor's main_device so the decoder and compositor share a GPU (no
    // cross-device copy). CARLINKIT_VAAPI_NODE overrides; a mismatch warns.
    const dev_t main_dev = dmabuf_feedback_.Current().main_device;
    const std::string matched = render_node_for_device(main_dev);
    std::string node;
    if (const char* env = std::getenv("CARLINKIT_VAAPI_NODE"); env != nullptr) {
      node = env;
      if (!matched.empty() && matched != node)
        std::fprintf(stderr,
                     "[vaapi] CARLINKIT_VAAPI_NODE=%s differs from the "
                     "compositor's device (%s) — possible cross-device copy\n",
                     node.c_str(), matched.c_str());
    } else if (!matched.empty()) {
      node = matched;
    } else {
      node = "/dev/dri/renderD128";
      if (main_dev != 0)
        std::fprintf(stderr,
                     "[vaapi] no render node matches the compositor's "
                     "main_device; using %s (possible cross-device copy)\n",
                     node.c_str());
    }

    decoder_ = ck::VaapiDecoderSource::create_headless(cfg.width, cfg.height,
                                                       node.c_str());
    if (decoder_ != nullptr) {
      sink_ = std::make_unique<ck::WaylandSink>(
          [this] { return dmabuf_feedback_.CreateParams(); }, kVideoFourcc);
      sink_->set_on_release([this](uint32_t slot) {
        if (decoder_ != nullptr)
          decoder_->release_native_frame(slot);
      });
      std::fprintf(stderr, "[decode] VAAPI dmabuf path (render node %s)\n",
                   node.c_str());
      return true;
    }
    std::fprintf(stderr,
                 "[decode] VAAPI unavailable; falling back to software "
                 "(CPU decode + wl_shm)\n");
  }

  // Software (CPU) decode presented through wl_shm — works on any compositor
  // without a dmabuf scanout path, at the cost of a per-frame convert and copy.
  if (shm_.IsNull()) {
    std::fprintf(stderr,
                 "[decode] no wl_shm global; cannot software-present\n");
    return false;
  }
  decoder_ = ck::SoftwareDecoderSource::create_headless(cfg.width, cfg.height);
  if (decoder_ == nullptr) {
    std::fprintf(stderr, "[decode] software decoder open failed\n");
    return false;
  }
  shm_sink_ = std::make_unique<ck::WlShmSink>([this](int fd, int32_t size) {
    return wl::construct<wayland::client::wl_shm_pool_traits,
                         wayland::client::wl_shm_traits::Op::CreatePool>(
        *shm_.Get(), fd, size);
  });
  shm_sink_->set_on_release([this] { RenderIfReady(); });
  use_shm_ = true;
  std::fprintf(stderr, "[decode] software (CPU) + wl_shm path\n");
  return true;
}

void App::RequestFrameCallback() {
  using wl_s = wayland::client::wl_surface_traits;
  if (wl_proxy* raw =
          wl::construct<wayland::client::wl_callback_traits, wl_s::Op::Frame>(
              *surface_.Get())) {
    frame_callback_.Get()->app_ = this;
    frame_callback_.Get()->_SetProxy(raw);
  }
}

void App::RenderIfReady() {
  if (frame_pending_ || decoder_ == nullptr)
    return;

  wl_proxy* buf = nullptr;
  uint32_t fw = 0, fh = 0;
  if (use_shm_) {
    // Software path: present the newest CPU frame (seq-gated) via wl_shm.
    if (shm_sink_ == nullptr || decoder_->gpu_frame_seq() == last_shm_seq_)
      return;
    ck::DecodedFrame df;
    if (!decoder_->acquire_decoded_frame(df))
      return;
    buf = shm_sink_->buffer_for(df);  // converts + copies the pixels
    fw = df.width;
    fh = df.height;
    const uint64_t seq = df.seq;
    decoder_->release_decoded_frame();
    if (buf == nullptr)
      return;  // no free shm slot; retry when one releases (seq not advanced)
    last_shm_seq_ = seq;
  } else {
    // dmabuf path: import the newest decoded surface as a wl_buffer.
    if (sink_ == nullptr || !decoder_->has_fresh_content())
      return;
    ck::NativeFrame f;
    if (!decoder_->acquire_native_frame(f))
      return;
    buf = sink_->buffer_for(f);
    fw = f.width;
    fh = f.height;
    if (buf == nullptr)
      return;  // import failed, or the slot's buffer is still in flight
  }

  auto* surf = surface_.Get();
  surf->Attach(buf, 0, 0);
  surf->DamageBuffer(0, 0, static_cast<int32_t>(fw), static_cast<int32_t>(fh));
  // Surface logical size = the viewport destination (aspect-fitted, compositor-
  // centered), or the buffer size when no viewport. Input maps against this.
  surf_w_ = static_cast<int32_t>(fw);
  surf_h_ = static_cast<int32_t>(fh);
  if (viewport_.Get() != nullptr && viewport_.Get()->GetProxy() != nullptr &&
      width_ > 0 && height_ > 0 && fw > 0 && fh > 0) {
    const double fit = std::min(static_cast<double>(width_) / fw,
                                static_cast<double>(height_) / fh);
    surf_w_ = static_cast<int32_t>(std::lround(fw * fit));
    surf_h_ = static_cast<int32_t>(std::lround(fh * fit));
    viewport_.Get()->SetDestination(surf_w_, surf_h_);
  }
  RequestFrameCallback();
  if (have_presentation_)
    presentation_.Arm(surf->GetProxy(), present_seq_++);
  surf->Commit();
  frame_pending_ = true;
}

void App::OnFrameReady(uint32_t /*time_ms*/) {
  if (wl_proxy* spent = frame_callback_.Detach(); spent != nullptr)
    wl_proxy_destroy(spent);
  frame_pending_ = false;
  RenderIfReady();
}

void App::SendTouchNorm(double x, double y, ck::TouchAction a) {
  if (mgr_ == nullptr || surf_w_ <= 0 || surf_h_ <= 0)
    return;
  // Surface-local coordinates are already in the video's frame: the compositor
  // delivers input in the (output-transformed) surface space, so any panel
  // rotation is handled upstream and needs no un-rotation here.
  last_nx_ = static_cast<float>(std::clamp(x / surf_w_, 0.0, 1.0));
  last_ny_ = static_cast<float>(std::clamp(y / surf_h_, 0.0, 1.0));
  mgr_->send_touch(last_nx_, last_ny_, a);
}

void App::SendTouchUp() {
  // wl_touch.up carries no coordinates; release at the last known point.
  if (mgr_ != nullptr)
    mgr_->send_touch(last_nx_, last_ny_, ck::TouchAction::Up);
}

void App::OnPointerMotion(double x, double y) {
  if (ptr_down_)
    SendTouchNorm(x, y, ck::TouchAction::Move);
}

void App::OnPointerButton(double x, double y, uint32_t button, bool pressed) {
  if (button != BTN_LEFT)
    return;
  ptr_down_ = pressed;
  SendTouchNorm(x, y, pressed ? ck::TouchAction::Down : ck::TouchAction::Up);
}

void App::OnTouchDown(int32_t id, double x, double y) {
  if (touch_id_ == -1) {  // forward the first (primary) contact only
    touch_id_ = id;
    SendTouchNorm(x, y, ck::TouchAction::Down);
  }
}

void App::OnTouchMotion(int32_t id, double x, double y) {
  if (id == touch_id_)
    SendTouchNorm(x, y, ck::TouchAction::Move);
}

void App::OnTouchUp(int32_t id) {
  if (id == touch_id_) {
    touch_id_ = -1;
    SendTouchUp();
  }
}

// ── Seat / pointer / touch handler bodies (forward to the App)
// ────────────────
void WlPointerHandler::OnMotion(uint32_t, wl_fixed_t sx, wl_fixed_t sy) {
  x_ = wl_fixed_to_double(sx);
  y_ = wl_fixed_to_double(sy);
  if (app_ != nullptr)
    app_->OnPointerMotion(x_, y_);
}

void WlPointerHandler::OnButton(uint32_t,
                                uint32_t,
                                uint32_t button,
                                uint32_t state) {
  constexpr uint32_t kPressed = 1;  // WL_POINTER_BUTTON_STATE_PRESSED
  if (app_ != nullptr)
    app_->OnPointerButton(x_, y_, button, state == kPressed);
}

void WlTouchHandler::OnDown(uint32_t,
                            uint32_t,
                            wl_proxy*,
                            int32_t id,
                            wl_fixed_t x,
                            wl_fixed_t y) {
  if (app_ != nullptr)
    app_->OnTouchDown(id, wl_fixed_to_double(x), wl_fixed_to_double(y));
}

void WlTouchHandler::OnUp(uint32_t, uint32_t, int32_t id) {
  if (app_ != nullptr)
    app_->OnTouchUp(id);
}

void WlTouchHandler::OnMotion(uint32_t,
                              int32_t id,
                              wl_fixed_t x,
                              wl_fixed_t y) {
  if (app_ != nullptr)
    app_->OnTouchMotion(id, wl_fixed_to_double(x), wl_fixed_to_double(y));
}

void WlSeatHandler::OnCapabilities(uint32_t caps) {
  using namespace wayland::client;
  const bool has_ptr =
      (caps & static_cast<uint32_t>(WlSeatCapability::Pointer)) != 0u;
  const bool has_touch =
      (caps & static_cast<uint32_t>(WlSeatCapability::Touch)) != 0u;
  if (has_ptr && pointer_.IsNull()) {
    if (wl_proxy* raw =
            wl::construct<wl_pointer_traits, wl_seat_traits::Op::GetPointer>(
                *this)) {
      pointer_.Get()->app_ = app_;
      pointer_.Get()->_SetProxy(raw);
    }
  }
  if (has_touch && touch_.IsNull()) {
    if (wl_proxy* raw =
            wl::construct<wl_touch_traits, wl_seat_traits::Op::GetTouch>(
                *this)) {
      touch_.Get()->app_ = app_;
      touch_.Get()->_SetProxy(raw);
    }
  }
}

int App::Run() {
  if (!Connect() || !ScanGlobals() || !BindGlobals() || !CreateSurface() ||
      !StartDecode())
    return 1;

  // A cross-thread wakeup: the dongle RX thread signals the loop when a video
  // packet is submitted so the loop can present the next decoded frame.
  const int video_evt = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

  ck::DongleConfig cfg;
  ck::apply_box_env(cfg);
  const char* audio_dev = std::getenv("CARLINKIT_AUDIO_DEV");
  const char* mic_dev = std::getenv("CARLINKIT_MIC_DEV");
  ck::AudioMixer audio_out(audio_dev != nullptr ? audio_dev : "default");
  audio_out.start();
  ck::DongleManager* mgr = nullptr;
  ck::AudioInput mic(mic_dev != nullptr ? mic_dev : "default",
                     [&mgr](const int16_t* pcm, size_t n) {
                       if (mgr != nullptr)
                         mgr->send(ck::send_audio(pcm, n));
                     });

  ck::DongleSink sink;
  sink.on_video = [&](const ck::VideoFrame& fr) {
    decoder_->submit_bitstream(fr.data, fr.size, 0);
    if (video_evt >= 0) {
      const uint64_t one = 1;
      (void)!write(video_evt, &one, sizeof one);  // wake the presentation loop
    }
  };
  sink.on_audio = [&](const ck::AudioFrame& fr) {
    audio_out.submit(fr);
    if (fr.command >= 0) {
      switch (static_cast<ck::AudioCommand>(fr.command)) {
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
  sink.on_command = [&](ck::Command c) {
    if (c == ck::Command::RequestHostUI) {
      std::fprintf(stderr, "[command] RequestHostUI (car logo) — exiting\n");
      g_running = false;
    }
  };

  ck::DongleManager manager(cfg, sink);
  mgr = &manager;
  mgr_ = &manager;  // drives touch output from the input hooks
  std::fprintf(stderr, "waiting for dongle + phone (wifi+BT on, nearby)...\n");
  manager.start();
  std::fprintf(stderr, "carlinkit-wayland: presenting — Ctrl-C to quit\n");

  const bool ok = wl::RunEventLoop(
      display_.Get(), [] { return !g_running; }, "carlinkit-wayland",
      {wl::FdSource{[&] { return video_evt; },
                    [&] {
                      uint64_t v = 0;
                      (void)!read(video_evt, &v, sizeof v);
                      RenderIfReady();
                    }}});

  manager.stop();
  mic.stop();
  audio_out.stop();
  if (video_evt >= 0)
    close(video_evt);
  presentation_.Release();
  dmabuf_feedback_.Release();
  registry_.Reset();
  return ok ? 0 : 1;
}

}  // namespace

int main() {
  std::signal(SIGINT, on_sigint);
  std::signal(SIGTERM, on_sigint);
  App app;
  return app.Run();
}
