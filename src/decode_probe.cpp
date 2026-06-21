// SPDX-FileCopyrightText: 2026 Joel Winarske
// SPDX-License-Identifier: Apache-2.0
// Headless verification of the zero-copy decode path (no DRM master needed):
// decode an Annex-B H.264 file via libavcodec+VAAPI and print, for each frame,
// the exported DRM-PRIME descriptor (fourcc/modifier/planes) we will hand to
// drm-cxx's ExternalDmaBufSource.
//
//   ./carlinkit-decode-probe out.h264 [/dev/dri/renderD128]
#include <cstdint>
#include <cstdio>
#include <vector>

#include "vaapi_decoder.h"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <file.h264> [render_node]\n", argv[0]);
    return 2;
  }
  const char* path = argv[1];
  const char* node = argc > 2 ? argv[2] : "/dev/dri/renderD128";

  FILE* f = std::fopen(path, "rb");
  if (!f) {
    std::perror("fopen");
    return 1;
  }

  ck::VaapiDecoder dec;
  if (!dec.open(node))
    return 1;

  int frames = 0;
  dec.set_frame_cb([&](const ck::DrmFrame& fr, AVFrame*) {
    if (frames < 5 || frames % 30 == 0) {
      std::printf(
          "frame %3d: %ux%u fourcc=%c%c%c%c modifier=0x%016llx planes=%d\n",
          frames, fr.width, fr.height, (fr.drm_fourcc) & 0xff,
          (fr.drm_fourcc >> 8) & 0xff, (fr.drm_fourcc >> 16) & 0xff,
          (fr.drm_fourcc >> 24) & 0xff, (unsigned long long)fr.modifier,
          fr.nplanes);
      for (int i = 0; i < fr.nplanes; ++i)
        std::printf("           plane %d: fd=%d offset=%u pitch=%u\n", i,
                    fr.planes[i].fd, fr.planes[i].offset, fr.planes[i].pitch);
    }
    ++frames;
  });

  std::vector<uint8_t> buf(64 * 1024);
  size_t n;
  while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) {
    if (!dec.submit(buf.data(), n)) {
      std::fprintf(stderr, "decode error\n");
      break;
    }
  }
  dec.flush();
  std::fclose(f);
  std::printf("decoded %d frames; zero-copy DRM-PRIME export OK\n", frames);
  return frames > 0 ? 0 : 1;
}
