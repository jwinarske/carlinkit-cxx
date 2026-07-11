// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
// carlinkit-wayland — present live CarPlay/Android Auto through a Wayland
// compositor (no DRM master, no free VT — runs as an ordinary client).
//
// This first stage stands up the presentation scaffold and the zero-copy
// negotiation: it connects, binds the compositor / xdg-shell / linux-dmabuf /
// presentation-time globals, opens a fullscreen xdg toplevel, and evaluates the
// per-surface dmabuf-feedback snapshot to report whether the decoder's NV12
// output can be scanned out directly (the zero-copy path) on this compositor.
//
// The decode + present feed (DecoderSource -> WaylandSink -> wl_buffer, frame-
// callback pacing) lands next; it needs the decoder factory to open without a
// KMS device (the plan's "no KMS device at all"), which is a separate change.
#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

#include <drm_fourcc.h>

#include "wayland_sink.h"

// wcs include order is load-bearing: each wl/*.hpp helper needs its generated
// <proto>_client.hpp (and, for dmabuf feedback, the wl/linux_dmabuf.hpp tables)
// included first, so shield the block from the formatter's alphabetical sort.
// clang-format off
#include "wayland_client.hpp"
#include "xdg_shell_client.hpp"
#include "linux_dmabuf_client.hpp"
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

// Empty handlers for the event-less objects we only issue requests on.
class WlCompositorHandler
    : public wayland::client::CWlCompositor<WlCompositorHandler> {};
class WlSurfaceHandler : public wayland::client::CWlSurface<WlSurfaceHandler> {
};

class App {
 public:
  bool Run();

  // ── wcs App hooks (SFINAE-detected / handler callbacks) ────────────────────
  void OnDmabufFeedback(const wl::FeedbackSnapshot& snap);
  void OnPresented(const wl::PresentFeedback& fb);
  void OnDiscarded(uint32_t /*frame*/) {}
  void OnXdgSurfaceConfigure(uint32_t /*serial*/);
  void OnToplevelConfigure(int32_t w, int32_t h);
  void OnToplevelClose() { g_running = false; }

 private:
  bool Connect();
  bool ScanGlobals();
  bool BindGlobals();
  bool CreateSurface();
  void ReportScanout(const wl::FeedbackSnapshot& snap);

  wl::DisplayHandle display_;
  wl::CRegistry registry_;
  wl::DmabufFeedback<App> dmabuf_feedback_;
  wl::PresentationManager<App> presentation_;

  wl::WlPtr<WlCompositorHandler> compositor_;
  wl::WlPtr<wl::XdgWmBaseHandler> xdg_wm_base_;
  wl::WlPtr<WlSurfaceHandler> surface_;
  wl::WlPtr<wl::XdgSurfaceHandler<App>> xdg_surface_;
  wl::WlPtr<wl::XdgToplevelHandler<App>> xdg_toplevel_;

  uint32_t compositor_name_ = 0, compositor_ver_ = 0;
  uint32_t xdg_wm_base_name_ = 0, xdg_wm_base_ver_ = 0;
  bool have_compositor_ = false, have_xdg_ = false, have_dmabuf_ = false;
  bool surface_feedback_started_ = false;
  int32_t width_ = 0, height_ = 0;
};

void App::OnDmabufFeedback(const wl::FeedbackSnapshot& snap) {
  // Per-surface feedback re-advertises on fullscreen/output changes;
  // re-evaluate the scanout verdict every time rather than caching it.
  if (surface_feedback_started_)
    ReportScanout(snap);
}

void App::OnPresented(const wl::PresentFeedback& fb) {
  const bool zero_copy =
      (fb.flags &
       static_cast<uint32_t>(
           presentation_time::client::WpPresentationFeedbackKind::ZeroCopy)) !=
      0;
  std::fprintf(stderr, "[present] frame=%u flags=0x%x zero_copy=%d\n", fb.frame,
               fb.flags, zero_copy ? 1 : 0);
}

void App::OnXdgSurfaceConfigure(uint32_t /*serial*/) {
  // AckConfigure already issued by the handler; nothing else to commit here yet
  // (no buffer attached in this stage).
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
  for (uint64_t mod : scanout) {
    const ck::WaylandSink::Verdict v =
        ck::WaylandSink::negotiate(snap, kVideoFourcc, mod);
    std::fprintf(stderr,
                 "[dmabuf]   modifier 0x%016llx  presentable=%d scanout=%d\n",
                 static_cast<unsigned long long>(mod), v.presentable ? 1 : 0,
                 v.scanout ? 1 : 0);
  }
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

  if (!dmabuf_feedback_.Bind(registry_, this)) {
    std::fprintf(stderr, "bind linux_dmabuf failed\n");
    return false;
  }
  (void)presentation_.Bind(registry_, this);  // optional

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
  if (!surface_feedback_started_)
    ReportScanout(dmabuf_feedback_.Current());  // fall back to default snapshot
  return true;
}

bool App::Run() {
  if (!Connect() || !ScanGlobals() || !BindGlobals() || !CreateSurface())
    return false;

  std::fprintf(stderr,
               "carlinkit-wayland: negotiation complete — decode/present feed "
               "is the next stage. Ctrl-C to quit.\n");

  const bool ok = wl::RunEventLoop(
      display_.Get(), [] { return !g_running; }, "carlinkit-wayland");

  presentation_.Release();
  dmabuf_feedback_.Release();
  registry_.Reset();
  return ok;
}

}  // namespace

int main() {
  std::signal(SIGINT, on_sigint);
  std::signal(SIGTERM, on_sigint);
  App app;
  return app.Run() ? 0 : 1;
}
