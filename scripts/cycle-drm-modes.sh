#!/usr/bin/env bash
# Cycle the connected monitor's DRM modes through carlinkit-kms so you can pick
# the best-looking one. Each mode runs for a few seconds; resolution, fps, and
# DPI all follow the selected mode. Note the index/resolution you like, then
# re-run it directly with `--drm-mode <index>`.
#
# Run on a free VT (Ctrl+Alt+F3) with DRM master, and with the dongle plugged in
# and the phone paired — video only appears once a frame is decoded, so a blank
# window means the link hasn't connected yet within that mode's time slice.
#
#   scripts/cycle-drm-modes.sh [/dev/dri/cardN] [dwell-seconds]
#
# dwell-seconds is how long each mode is shown (default 15); the DWELL env var
# overrides it too.
#
# Env:
#   DWELL=20                       seconds to dwell on each mode (default 15)
#   CAPTURE_DIR=/tmp/shots         save a raw NV12 screenshot per mode (SIGUSR1
#                                  near the end of each dwell); convert with e.g.
#                                  ffmpeg -f rawvideo -pix_fmt nv12 -s WxH -i f.nv12 f.png
#   CARLINKIT_CONNECTOR=HDMI-A-1   target a specific display
#   ALL_MODES=1                    include duplicate resolutions (default: dedup)
#   plus the usual CARLINKIT_AUDIO_DEV etc. (passed through to carlinkit-kms)
set -u

CARD="${1:-/dev/dri/card1}"
DWELL="${2:-${DWELL:-15}}"  # seconds each mode is shown
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$HERE/../build/carlinkit-kms"

[ -x "$BIN" ] || {
  echo "carlinkit-kms not built at $BIN — build it first" >&2
  exit 1
}

# Enumerate modes as "<index> <WxH> <refreshHz>".
modes="$("$BIN" "$CARD" --no-seat --drm-list-modes 2>/dev/null |
  sed -nE 's/^[[:space:]]*\[[[:space:]]*([0-9]+)\][[:space:]]*([0-9]+x[0-9]+)[[:space:]]*@[[:space:]]*([0-9]+)Hz.*/\1 \2 \3/p')"
[ -n "$modes" ] || {
  echo "no modes found on $CARD — run on a VT with DRM master, check the card" >&2
  exit 1
}

# Dedup by resolution unless ALL_MODES=1 (keep the first index per WxH).
if [ "${ALL_MODES:-0}" != 1 ]; then
  modes="$(echo "$modes" | awk '!seen[$2]++')"
fi

count="$(echo "$modes" | wc -l)"
echo "Cycling $count mode(s) on $CARD, ${DWELL}s each. Ctrl-C to stop early."
echo "Note the [index] of the one that looks best, then run:"
echo "  $BIN $CARD --no-seat --drm-mode <index>"
echo

while read -r idx res hz; do
  echo "==== [$idx]  ${res} @ ${hz}Hz  (${DWELL}s) ===="
  if [ -n "${CAPTURE_DIR:-}" ]; then
    mkdir -p "$CAPTURE_DIR"
    CARLINKIT_CAPTURE_DIR="$CAPTURE_DIR" \
      "$BIN" "$CARD" --no-seat --drm-mode "$idx" </dev/null &
    pid=$!
    sleep "$((DWELL > 3 ? DWELL - 2 : 1))"  # let the link connect + render
    kill -USR1 "$pid" 2>/dev/null            # snapshot the current frame
    sleep 2
    kill -INT "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
  else
    timeout -k 2 --signal=INT "$DWELL" \
      "$BIN" "$CARD" --no-seat --drm-mode "$idx" </dev/null
  fi
  sleep 1
done <<<"$modes"

# Convert the raw NV12 captures to JPG (dimensions parsed from the file name).
if [ -n "${CAPTURE_DIR:-}" ]; then
  shopt -s nullglob
  shots=("$CAPTURE_DIR"/*.nv12)
  if [ "${#shots[@]}" -gt 0 ]; then
    echo "Converting ${#shots[@]} NV12 capture(s) to JPG..."
    python3 - "${shots[@]}" <<'PY' || echo "  (skipped: needs python3 + numpy + Pillow)"
import os, re, sys
import numpy as np
from PIL import Image
for path in sys.argv[1:]:
    m = re.search(r'(\d+)x(\d+)', os.path.basename(path))
    if not m:
        print("  skip %s (no WxH in name)" % path); continue
    w, h = int(m.group(1)), int(m.group(2))
    d = np.fromfile(path, dtype=np.uint8)
    if d.size < w * h * 3 // 2:
        print("  skip %s (truncated)" % path); continue
    Y = d[:w * h].reshape(h, w).astype(np.float32)
    uv = d[w * h:w * h + w * h // 2].reshape(h // 2, w // 2, 2).astype(np.float32)
    U = np.repeat(np.repeat(uv[:, :, 0], 2, 0), 2, 1)
    V = np.repeat(np.repeat(uv[:, :, 1], 2, 0), 2, 1)
    c, e, f = Y - 16.0, U - 128.0, V - 128.0   # BT.601 limited range
    R = 1.164 * c + 1.596 * f
    G = 1.164 * c - 0.392 * e - 0.813 * f
    B = 1.164 * c + 2.017 * e
    rgb = np.clip(np.dstack([R, G, B]), 0, 255).astype(np.uint8)
    out = os.path.splitext(path)[0] + ".jpg"
    Image.fromarray(rgb).save(out, quality=90)
    print("  wrote %s" % out)
PY
  fi
fi

echo "Done — re-run your favorite with: $BIN $CARD --no-seat --drm-mode <index>"
