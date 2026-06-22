// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#pragma once
// Apply CARLINKIT_* environment overrides for resolution and box settings onto
// a DongleConfig. The caller seeds sensible defaults first (carlinkit-kms
// derives them from the active DRM mode); these variables then override
// individual fields. Per-event protocol values are not configured here.
#include <strings.h>

#include <cstdio>
#include <cstdlib>

#include "protocol.h"

namespace ck {

inline void apply_box_env(DongleConfig& c) {
  if (const char* r = std::getenv("CARLINKIT_RESOLUTION"); r != nullptr) {
    unsigned w = 0;
    unsigned h = 0;
    // NOLINTNEXTLINE(cert-err34-c) -- return value checked on the next line
    const int n = std::sscanf(r, "%ux%u", &w, &h);
    if (n == 2 && w != 0 && h != 0) {
      c.width = w;
      c.height = h;
    }
  }
  if (const char* a = std::getenv("CARLINKIT_AA_RESOLUTION"); a != nullptr) {
    unsigned w = 0;
    unsigned h = 0;
    // NOLINTNEXTLINE(cert-err34-c) -- return value checked on the next line
    const int n = std::sscanf(a, "%ux%u", &w, &h);
    if (n == 2 && w != 0 && h != 0) {
      c.aaWidth = w;
      c.aaHeight = h;
    }
  }
  if (const char* f = std::getenv("CARLINKIT_FPS"); f != nullptr) {
    const auto v = static_cast<unsigned>(std::strtoul(f, nullptr, 10));
    if (v != 0)
      c.fps = v;
  }
  if (const char* d = std::getenv("CARLINKIT_DPI"); d != nullptr) {
    const auto v = static_cast<unsigned>(std::strtoul(d, nullptr, 10));
    if (v != 0)
      c.dpi = v;
  }
  if (const char* d = std::getenv("CARLINKIT_DRIVE_POSITION"); d != nullptr)
    c.hand = (strcasecmp(d, "right") == 0 || d[0] == '1') ? 1U : 0U;
  if (const char* g = std::getenv("CARLINKIT_GNSS"); g != nullptr)
    c.gnssCapability = static_cast<uint32_t>(std::strtoul(g, nullptr, 0));
  if (const char* d = std::getenv("CARLINKIT_DASHBOARD"); d != nullptr)
    c.dashboardInfo = static_cast<uint32_t>(std::strtoul(d, nullptr, 0));
  if (const char* b = std::getenv("CARLINKIT_BT_PHONE"); b != nullptr)
    c.useBtPhone = static_cast<uint32_t>(std::strtoul(b, nullptr, 0));
  if (const char* hc = std::getenv("CARLINKIT_HICAR"); hc != nullptr)
    c.hiCarConnectMode = static_cast<uint32_t>(std::strtoul(hc, nullptr, 0));
  if (const char* m = std::getenv("CARLINKIT_MEDIA_DELAY"); m != nullptr)
    c.mediaDelay = static_cast<uint32_t>(std::strtoul(m, nullptr, 0));
}

}  // namespace ck
