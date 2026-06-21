// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
// Read-only DRM topology dumper: connectors -> CRTCs, and per-plane the CRTCs
// it can drive plus which modifiers it advertises for NV12. Answers "which pipe
// can scan out the decoder's tiled NV12". Runs without DRM master.
//
//   ./carlinkit-drm-dump [/dev/dri/cardN]
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
uint64_t prop_blob_id(int fd, uint32_t obj, uint32_t type, const char* name) {
  drmModeObjectProperties* p = drmModeObjectGetProperties(fd, obj, type);
  uint64_t v = 0;
  if (p != nullptr) {
    for (uint32_t i = 0; i < p->count_props; ++i) {
      drmModePropertyRes* pr = drmModeGetProperty(fd, p->props[i]);
      if (pr != nullptr) {
        if (strcmp(pr->name, name) == 0)
          v = p->prop_values[i];
        drmModeFreeProperty(pr);
      }
    }
    drmModeFreeObjectProperties(p);
  }
  return v;
}
}  // namespace

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : "/dev/dri/card1";
  int fd = open(path, O_RDWR);  // NOLINT(cppcoreguidelines-pro-type-vararg)
  if (fd < 0) {
    std::perror("open");
    return 1;
  }
  drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
  drmModeRes* res = drmModeGetResources(fd);
  if (res == nullptr) {
    std::fprintf(stderr, "no DRM resources\n");
    return 1;
  }

  std::printf("=== CRTCs (index -> id) ===\n");
  for (int i = 0; i < res->count_crtcs; ++i)
    std::printf("  crtc[%d] id=%u\n", i, res->crtcs[i]);

  std::printf("=== Connectors ===\n");
  for (int i = 0; i < res->count_connectors; ++i) {
    drmModeConnector* c = drmModeGetConnector(fd, res->connectors[i]);
    if (c == nullptr)
      continue;
    char name[64];
    std::snprintf(name, sizeof name, "%s-%u",
                  drmModeGetConnectorTypeName(c->connector_type),
                  c->connector_type_id);
    uint32_t pc = 0;
    for (int e = 0; e < c->count_encoders; ++e) {
      drmModeEncoder* enc = drmModeGetEncoder(fd, c->encoders[e]);
      if (enc != nullptr) {
        pc |= enc->possible_crtcs;
        drmModeFreeEncoder(enc);
      }
    }
    std::printf("  %-12s %s  possible_crtcs=0x%x  modes=%d\n", name,
                c->connection == DRM_MODE_CONNECTED ? "connected" : "disconn",
                pc, c->count_modes);
    drmModeFreeConnector(c);
  }

  std::printf("=== Planes (NV12 modifiers) ===\n");
  drmModePlaneRes* pr = drmModeGetPlaneResources(fd);
  for (uint32_t i = 0; pr != nullptr && i < pr->count_planes; ++i) {
    drmModePlane* pl = drmModeGetPlane(fd, pr->planes[i]);
    if (pl == nullptr)
      continue;
    bool nv12 = false;
    for (uint32_t f = 0; f < pl->count_formats; ++f)
      if (pl->formats[f] == DRM_FORMAT_NV12)
        nv12 = true;
    std::printf("  plane id=%u possible_crtcs=0x%x nv12=%d", pl->plane_id,
                pl->possible_crtcs, nv12 ? 1 : 0);

    uint64_t blob_id =
        prop_blob_id(fd, pl->plane_id, DRM_MODE_OBJECT_PLANE, "IN_FORMATS");
    if (blob_id != 0) {
      drmModePropertyBlobRes* blob =
          drmModeGetPropertyBlob(fd, static_cast<uint32_t>(blob_id));
      if (blob != nullptr) {
        const auto* h =
            static_cast<const drm_format_modifier_blob*>(blob->data);
        const auto* fmts = reinterpret_cast<const uint32_t*>(
            static_cast<const char*>(blob->data) + h->formats_offset);
        const auto* mods = reinterpret_cast<const drm_format_modifier*>(
            static_cast<const char*>(blob->data) + h->modifiers_offset);
        std::printf("  NV12-modifiers:");
        for (uint32_t m = 0; m < h->count_modifiers; ++m) {
          for (uint32_t fi = 0; fi < h->count_formats; ++fi) {
            if (fmts[fi] != DRM_FORMAT_NV12)
              continue;
            if (fi < mods[m].offset || fi >= mods[m].offset + 64)
              continue;
            if ((mods[m].formats & (1ULL << (fi - mods[m].offset))) != 0)
              std::printf(" 0x%llx",
                          static_cast<unsigned long long>(mods[m].modifier));
          }
        }
        drmModeFreePropertyBlob(blob);
      }
    }
    std::printf("\n");
    drmModeFreePlane(pl);
  }
  return 0;
}
