// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#include "decoder_source.h"

#include <strings.h>

#include <cstdio>
#include <cstdlib>

#ifndef CARLINKIT_SOFTWARE_ONLY
#include "vaapi_decoder_source.h"
#endif

namespace ck {
namespace {

DecoderBackend parse_pref() {
  const char* d = std::getenv("CARLINKIT_DECODER");
  if (d == nullptr || *d == '\0')
    return DecoderBackend::Auto;
  if (strcasecmp(d, "auto") == 0)
    return DecoderBackend::Auto;
  if (strcasecmp(d, "vaapi") == 0)
    return DecoderBackend::Vaapi;
  if (strcasecmp(d, "v4l2") == 0)
    return DecoderBackend::V4l2;
  if (strcasecmp(d, "software") == 0 || strcasecmp(d, "sw") == 0)
    return DecoderBackend::Software;
  std::fprintf(stderr, "CARLINKIT_DECODER=%s unrecognized; using auto\n", d);
  return DecoderBackend::Auto;
}

}  // namespace

std::unique_ptr<DecoderSource> create_decoder_source(drm::Device& dev,
                                                     uint32_t coded_w,
                                                     uint32_t coded_h) {
  const DecoderBackend pref = parse_pref();

#ifdef CARLINKIT_SOFTWARE_ONLY
  (void)dev;
  (void)coded_w;
  (void)coded_h;
  if (pref != DecoderBackend::Auto && pref != DecoderBackend::Software)
    std::fprintf(stderr,
                 "built software-only; ignoring CARLINKIT_DECODER override\n");
  std::fprintf(stderr, "software decoder backend unavailable\n");
  return nullptr;
#else
  // VAAPI — the zero-copy HW-plane path.
  if (pref == DecoderBackend::Auto || pref == DecoderBackend::Vaapi) {
    if (auto s = VaapiDecoderSource::create(dev, coded_w, coded_h)) {
      std::fprintf(stderr, "decoder: VAAPI (zero-copy HW plane)\n");
      return s;
    }
    if (pref == DecoderBackend::Vaapi) {
      std::fprintf(stderr, "CARLINKIT_DECODER=vaapi but VAAPI open failed\n");
      return nullptr;
    }
    std::fprintf(stderr, "VAAPI unavailable; trying next backend\n");
  }

  if (pref == DecoderBackend::V4l2) {
    std::fprintf(stderr, "V4L2 decoder backend unavailable\n");
    return nullptr;
  }
  if (pref == DecoderBackend::Software) {
    std::fprintf(stderr, "software decoder backend unavailable\n");
    return nullptr;
  }
  std::fprintf(stderr, "no decoder backend available\n");
  return nullptr;
#endif
}

}  // namespace ck
