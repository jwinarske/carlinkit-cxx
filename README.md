# carlinkit-cxx

A C++/libusb receiver for Carlinkit **"Auto Box" CarPlay/Android Auto dongles**
(USB `1314:1520` / `1314:1521`), with a zero-copy, hardware-plane video path:

```
dongle ──USB bulk──► libusb async DMA ring ──► H.264 ──► HW decode (VAAPI / V4L2 M2M)
                                                            │ NV12 DMA-BUF (zero-copy)
                                                            ▼
                                              drm-cxx scene ──► HW overlay plane (KMS)
```

The phone connects to the dongle **wirelessly** (Wi-Fi + Bluetooth); nothing is
plugged into the dongle. See `docs/ARCHITECTURE.md` for the full design.

## Targets

- **Desktop (dev):** H.264 decode via **libavcodec + VAAPI** → NV12 DMA-BUF.
- **Embedded (deploy):** Rockchip / other SoC via drm-cxx **`V4l2DecoderSource`**
  (stateful V4L2 M2M decoder, no GStreamer).

The decoder code is portable; on distros with full ffmpeg (SteamOS/Arch, etc.)
it works out of the box. **Fedora needs two package swaps — see below.**

## ⚠️ Fedora setup (important)

Fedora ships Mesa **and** ffmpeg with the patent-encumbered H.264/H.265 codecs
**removed**. Both must be restored or hardware H.264 decode silently fails (Mesa
won't expose the VA decoder; libavcodec won't expose the VAAPI hwaccel and
**falls back to software** — symptom: `vaExportSurfaceHandle` returns
`invalid VASurfaceID (0x6)` because the frame is software YUV420P, not a VAAPI
surface).

```bash
# 1. Enable RPM Fusion (free is enough for both fixes)
sudo dnf install -y \
  https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm

# 2. Full Mesa VA drivers (adds H.264/H.265 *VA decode* to the radeonsi driver)
sudo dnf swap -y mesa-va-drivers mesa-va-drivers-freeworld

# 3. Full libavcodec (adds the H.264/H.265 *VAAPI hwaccel* to libavcodec)
sudo dnf install -y libavcodec-freeworld

# 4. Verify
vainfo | grep VAProfileH264          # expect: VAProfileH264High : VAEntrypointVLD
./build/carlinkit-decode-probe out.h264   # expect: "... zero-copy DRM-PRIME export OK"
```

`libavcodec-freeworld` overrides the stripped `libavcodec-free`. `vainfo` comes
from `libva-utils`. Non-Fedora distros with full ffmpeg/Mesa can skip all of this.

## Build dependencies

- `cmake`, a **C++17** compiler (C++17 to match drm-cxx's ABI — C++20 selects
  `std::span` and mismatches drm-cxx's `tcb::span` symbols)
- `libusb-1.0` (`libusb1-devel`)
- `libavcodec` + `libavutil` (`libavcodec-free-devel` — headers live in
  `/usr/include/ffmpeg`) and `libva` + `libva-drm` (`libva-devel`) for the
  VAAPI decode probe / desktop video path
- KMS app: **`drm-cxx`** (sibling source tree, consumed via `add_subdirectory`
  at `../drm-cxx`), `libdrm`, `libva`, and ALSA (`alsa-lib-devel`)

```bash
cmake -S . -B build && cmake --build build
```

Targets: `carlinkit-probe` (headless capture), `carlinkit-decode-probe` (decode
check), `carlinkit-audio` (headless audio), `carlinkit-drm-dump` (DRM plane/mode
topology), and `carlinkit-kms` (the full head unit). The KMS app is built only
when `drm-cxx` + libva + libdrm are found.

## USB access (run without root)

The dongle's USB node is root-only by default. Install the udev rule for
non-root access (and so access survives the dongle re-enumerating):

```bash
sudo cp scripts/99-carlinkit.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo udevadm trigger --attr-match=idVendor=1314
```

The dongle also exposes a small USB mass-storage partition that auto-mounts at
`/run/media/$USER/APK`; unmount it before running (`umount /run/media/$USER/APK`).

## Run

```bash
# Headless capture: run init handshake, dump the CarPlay stream.
# Bring your iPhone near with Wi-Fi + Bluetooth on and accept CarPlay.
./build/carlinkit-probe                 # → out.h264 (H.264) + out.pcm (PCM audio)

# Verify the zero-copy HW decode path headlessly (no DRM master needed):
./build/carlinkit-decode-probe out.h264

# Headless audio (no display): play the dongle's PCM + capture mic.
./build/carlinkit-audio [playback-dev] [capture-dev]   # e.g. plughw:0,0

# Dump DRM plane/modifier/CRTC topology (read-only, no DRM master):
./build/carlinkit-drm-dump /dev/dri/card1
```

### The head unit (`carlinkit-kms`)

Full experience — live video on a HW plane + audio + touch/cursor. **Must run on
a free VT (Ctrl+Alt+F3) with DRM master**, not inside a desktop or over SSH (stop
your display manager first if needed: `sudo systemctl stop gdm`).

```bash
./build/carlinkit-kms /dev/dri/card1 --no-seat
# Pair your iPhone (Wi-Fi + Bluetooth on). Quit with Esc / Q / Ctrl-C.
```

- **Video** fills the panel preserving aspect ratio via the HW plane scaler (zero
  CPU); letterboxes when the panel isn't 16:9. The panel's **native mode** is kept
  (forcing a non-native mode makes the monitor rescale and distort).
- **Audio** plays through ALSA (`"default"` → PipeWire when present); mic is
  captured on Siri/phone-call.
- **Touch / mouse** drive the CarPlay UI; a HW cursor follows the mouse.
- **Hotplug**: the dongle is auto-(re)connected; surprise removal is survived
  (the last frame stays on screen until video resumes).

Environment overrides:

| Var | Effect |
|-----|--------|
| `CARLINKIT_CONNECTOR=DP-1` | Target a specific display (e.g. `HDMI-A-1`, `DP-1`) |
| `DRM_FORCE_MODE=2560x1440` | Force a specific mode on the connector |
| `CARLINKIT_CRTC=<id>` | Force a CRTC (debug; see `carlinkit-drm-dump`) |
| `CARLINKIT_POINTER_GAIN=4` | Free mouse cursor speed multiplier (default 2.5) |
| `CARLINKIT_DRAG_GAIN=5` | Cursor speed while a button is held — swipes/flings (default 5.0) |
| `CARLINKIT_TOUCH_HZ=90` | Touch report rate while dragging; coalesces the mouse stream to a touchscreen-like rate so CarPlay flings register (default 90) |
| `CARLINKIT_AUDIO_DEV=plughw:1,3` | ALSA playback device (default `default`) — see Audio setup |
| `CARLINKIT_AUDIO_LATENCY_MS=120` | Playback buffer in ms; larger trades latency for fewer underrun clicks (default 120) |
| `CARLINKIT_MIC_DEV=plughw:2,0` | ALSA capture device for Siri / calls (default `default`) |
| `CARLINKIT_OEM_ICON=assets/logo.png` | PNG to set as the dongle's own launcher tile (sent on connect); unset = leave the dongle's default |
| `CARLINKIT_OEM_LABEL="carlinkit-cxx"` | Optional caption shown under the OEM icon |

> **Known issue:** on some AMD multi-display setups, drm-cxx's allocator rejects
> the decoder's tiled-NV12 modifier on one pipe (e.g. `DP-1`) even though the
> kernel advertises it on every CRTC (`carlinkit-drm-dump` confirms). Use the
> working connector for now.

### Audio setup

Audio plays through ALSA. The app opens the `"default"` PCM, which on a normal
desktop **PipeWire** session routes wherever your system audio goes — that just
works. But on a bare VT (PipeWire usually not running) or when your speakers are
on a non-default card, you must point the app at the right device with
`CARLINKIT_AUDIO_DEV`.

1. **List your playback devices** and find the card your speakers are on:

   ```bash
   aplay -l
   # card 0: Audio [USB Audio] ...        ← a USB DAC / headset
   # card 1: Generic [HD-Audio Generic], device 3: HDMI 0 [LG ...]   ← monitor over HDMI
   ```

   The ALSA name is `plughw:<card>,<device>` (the `plug` prefix lets ALSA convert
   sample rates, which the app's streams need). Examples: `plughw:0,0` (USB),
   `plughw:1,3` (the HDMI/monitor output above).

2. **Not sure which one has your speakers?** Play a test tone to each:

   ```bash
   speaker-test -D plughw:1,3 -c 2 -t sine -f 440 -l 1   # listen, Ctrl-C to stop
   ```

3. **Run with that device:**

   ```bash
   CARLINKIT_AUDIO_DEV=plughw:1,3 ./build/carlinkit-kms /dev/dri/card1 --no-seat
   ```

   Or test audio alone (no display, plays while the phone streams):

   ```bash
   ./build/carlinkit-audio plughw:1,3
   ```

**Muting:** `AudioOutput` clears the card's playback mute switches when it opens
the device, so a muted mixer (common on USB DACs) can't silence playback. If you
still hear nothing, check levels with `alsamixer -c <card>` (M toggles mute, up
arrow raises volume). To keep mixer state across reboots: `sudo alsactl store`.

**HDMI / DisplayPort audio** has no software volume (it is fixed; control it on
the monitor) and cannot capture, so route the mic (`CARLINKIT_MIC_DEV`) to a real
input — a webcam or USB mic — when you want Siri / phone calls.

## Status

- ✅ USB protocol + async DMA-ring receiver (`carlinkit-probe`)
- ✅ libavcodec+VAAPI H.264 → zero-copy NV12 DMA-BUF (`carlinkit-decode-probe`)
- ✅ drm-cxx KMS video sink on a HW plane (`carlinkit-kms`) — aspect-fit, native mode
- ✅ ALSA audio out + mic; touch + mouse cursor input
- ✅ Hotplug / surprise-removal (auto-reconnect)
- 🚧 Embedded SoC `V4l2DecoderSource` path; DP-1 plane-allocation fix
