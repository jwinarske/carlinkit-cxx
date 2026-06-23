# carlinkit-cxx — architecture (zero-copy, HW plane)

Target device: Carlinkit "Auto Box" **`1314:1520`/`1314:1521`** (vendor-specific
bulk protocol). It receives the CarPlay/Android Auto H.264 video + PCM audio and
sends touch, with a zero-copy, hardware-plane display path.

## Data flow
```
                 ┌────────────────────────── carlinkit-cxx ──────────────────────────┐
 iPhone ⇢(wifi)⇢ Dongle ──USB bulk IN 0x81──► UsbDevice (libusb, DMA buffers via
                                              libusb_dev_mem_alloc — zero-copy RX)
                                                   │ framed messages (magic 0x55aa55aa)
                                                   ▼
                                              Dongle driver  ── init seq + 2s heartbeat ──► OUT 0x01
                                                   │  VideoData(H.264 Annex-B)  AudioData(PCM)  Touch◄
                              ┌────────────────────┼─────────────────────┐
                              ▼ H.264 NAL          ▼ PCM                  ▲ SendTouch
                  DecoderSource (VAAPI / V4L2 / software)  AudioOutput (ALSA)   TouchMapper
                  (a drm::scene::LayerBufferSource)                             (drm-cxx libinput)
                                                   │ NV12 DMA-BUF (decoded; CPU copy only on software path)
                                                   ▼
                                       drm-cxx LayerScene / atomic commit
                                                   ▼
                                   **HW plane scanout** (drm::Device, KMS)
```

`DongleManager` supervises the dongle: it (re)connects when the device appears and
tears the session down on surprise removal, so the rest of the graph keeps running.

## Zero-copy contract
- **RX (USB):** bulk-IN into `libusb_dev_mem_alloc` DMA buffers (kernel USBDEVFS
  zero-copy). The H.264 payload is handed to the decoder by span/move — never
  deep-copied in our code.
  - libusb exposes **no explicit scatter-gather** per transfer (one contiguous
    buffer per URB); the kernel does SG/page-pinning under the hood for ordinary
    buffers, and `dev_mem_alloc` gives a single DMA-coherent region (no bounce
    copy) capped by `usbfs_memory_mb` (16 MB here).
  - The RX path is a **ring of N async `libusb_submit_transfer`s over
    `dev_mem_alloc` buffers**, re-submitted in the completion callback, so the
    host controller never idles between transfers. (`libusb_alloc_streams` /
    USB-3 bulk streams is a different feature, not SG, and unneeded here.)
- **Coded → decoder:** the one intrinsic copy is the compressed bitstream into
  the decoder's V4L2 OUTPUT buffer (or VAAPI coded buffer). Tiny vs. raw frames;
  unavoidable for stateful decode.
- **Decoded → display:** the decoder's NV12 output is a **DMA-BUF**, imported as
  a KMS framebuffer (`drmPrimeFDToHandle` + `AddFB2WithModifiers`) and shown on a
  HW plane via **atomic commit**. The megabyte-scale raw frame never crosses the
  CPU. Aspect-fit scaling and centering are done by the plane's `src`/`dst` rects
  (the display controller's scaler) — also zero CPU.

## Decoder
The decoder is a `ck::DecoderSource` (a `drm::scene::LayerBufferSource` plus a
`submit_bitstream` feed) the scene scans out directly. `create_decoder_source`
(`src/decoder_source.cpp`) selects a backend: `CARLINKIT_DECODER`
(`vaapi`/`v4l2`/`software`/`auto`, default `auto`) pins one, and `auto` tries
VAAPI, then V4L2, then software, using the first that opens. A pinned backend
that fails to open is an error rather than a fall-through.

- **`VaapiDecoderSource`** (this AMD desktop) — `src/vaapi_decoder*.cpp`.
  libavcodec H.264 + VAAPI hwaccel on `renderD128`; each decoded VASurface is
  exported with `vaExportSurfaceHandle` (`COMPOSED_LAYERS`) as a DRM-PRIME NV12
  DMA-BUF, imported as a KMS framebuffer. An AVFrame ref is retained per imported
  framebuffer (a small ring) so the decoder's surface pool can't overwrite a
  frame still on screen.
- **`V4l2DecoderSourceAdapter`** (embedded SoC, e.g. Rockchip) —
  `src/v4l2_decoder_source_adapter.cpp`. Wraps drm-cxx's stateful V4L2 M2M
  decoder source (NV12 DMA-BUF → KMS framebuffer, no GStreamer). The inner source
  is event-loop shaped (poll `fd()`, `drive()`, `submit_bitstream` returns EAGAIN
  when full); the adapter runs a pump thread that drives the decoder and feeds
  queued coded chunks, so the dongle RX thread just enqueues. The decoder is
  found via `CARLINKIT_V4L2_DEV` or a scan of `/dev/video*`.
- **`SoftwareDecoderSource`** (the always-available fallback) —
  `src/software_decoder_source.cpp`. libavcodec CPU H.264 decode, converted to
  NV12 with libswscale into a pool of LINEAR DRM dumb buffers scanned out on the
  HW plane. No GPU/VAAPI/V4L2, so it works anywhere, but the per-frame decode +
  convert + copy is slow — it prints a loud warning when it engages. A pool slot
  stays reserved (`in_flight`) from `acquire()` until `release()` so a buffer
  still on screen is never refilled. `CARLINKIT_SOFTWARE_ONLY` (CMake) builds
  only this backend, omitting VAAPI/V4L2 and the direct libva link.

## drm-cxx integration facts (from source)
- `drm::scene::LayerBufferSource` is the interface the scene consumes:
  `acquire() -> AcquiredBuffer{fb_id, opaque, …}`, `release()`,
  `format() -> SourceFormat`, `binding_model() == SceneSubmitsFbId`. A live source
  may return EAGAIN before its first frame.
- `ExternalDmaBufSource::create(dev, w, h, drm_fourcc, modifier,
  span<ExternalPlaneInfo{fd, offset, pitch}>, on_release)` dups the fds and
  imports via `drmPrimeFDToHandle` + `AddFB2WithModifiers`. The modifier is
  forwarded to the kernel verbatim (`external_dma_buf_source.cpp:74-79`) — vendor
  TILED modifiers (incl. the AMD GFX `0x0200000000401b03` VAAPI produces) are
  supported; the kernel validates. (The header doc-comment claiming LINEAR-only
  is stale.)
- `LayerScene::create(dev, {crtc_id, connector_id, mode})` → `add_layer(LayerDesc
  {source, display.src_rect/dst_rect, content_type=Video})` → `get_layer(h)
  ->set_src_rect/set_dst_rect`, `test()`, `commit(DRM_MODE_PAGE_FLIP_EVENT,
  &page_flip)`. `Rect` is `{int32 x, y; uint32 w, h}`.
- DRM master is required → must run on a free VT, not inside the desktop or over
  SSH.
- Consumed from the sibling tree `../drm-cxx` via `add_subdirectory`; link target
  `drm-cxx::drm-cxx`. Built at **C++17** (drm-cxx's `drm::span` is `tcb::span` at
  C++17; C++20 selects `std::span` and mismatches its compiled symbols).

## Audio
`AudioOutput` (`src/audio_alsa.cpp`) plays the dongle's PCM through ALSA on a
dedicated thread (so backpressure never stalls USB), reconfiguring the device when
the stream's rate/channels change. `AudioInput` captures 16 kHz mono mic audio on
demand (Siri / phone call) and sends it back with `send_audio`.

## Input
`TouchMapper` (`src/input_touch.h`) translates drm-cxx libinput events into dongle
touch messages: touchscreen coordinates (already normalized) and mouse motion are
mapped into the on-screen video rectangle's local space. A drm-cxx HW cursor plane
follows the mouse on desktop. Input is dispatched on its own thread, decoupled
from the render/commit loop.
