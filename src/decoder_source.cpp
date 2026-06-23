// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
#include "decoder_source.h"

#include <strings.h>

#include <cstdio>
#include <cstdlib>

#include "software_decoder_source.h"
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

void software_warning() {
  std::fprintf(
      stderr,
      "\n"
      "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n"
      "!!!  SOFTWARE DECODE WARNING\n"
      "!!!  No hardware video decoder is in use. H.264 is being\n"
      "!!!  decoded on the CPU and converted to NV12 every frame,\n"
      "!!!  which is slow and may not keep up at higher resolutions.\n"
      "!!!  Prefer a VAAPI or V4L2 decoder where one is available.\n"
      "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n\n");
}

std::unique_ptr<DecoderSource> make_software(drm::Device& dev,
                                             uint32_t coded_w,
                                             uint32_t coded_h) {
  if (auto s = SoftwareDecoderSource::create(dev, coded_w, coded_h)) {
    software_warning();
    std::fprintf(stderr, "decoder: software (CPU H.264 -> NV12 dumb buffer)\n");
    return s;
  }
  std::fprintf(stderr, "software decoder open failed\n");
  return nullptr;
}

}  // namespace

std::unique_ptr<DecoderSource> create_decoder_source(drm::Device& dev,
                                                     uint32_t coded_w,
                                                     uint32_t coded_h) {
  const DecoderBackend pref = parse_pref();

#ifdef CARLINKIT_SOFTWARE_ONLY
  if (pref != DecoderBackend::Auto && pref != DecoderBackend::Software)
    std::fprintf(stderr,
                 "built software-only; ignoring CARLINKIT_DECODER override\n");
  return make_software(dev, coded_w, coded_h);
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

  // V4L2 — a stateful SoC decoder (added to the chain separately).
  if (pref == DecoderBackend::V4l2) {
    std::fprintf(stderr, "V4L2 decoder backend unavailable\n");
    return nullptr;
  }

  // Software — the always-available CPU fallback (pinned, or the end of Auto).
  return make_software(dev, coded_w, coded_h);
#endif
}

}  // namespace ck
