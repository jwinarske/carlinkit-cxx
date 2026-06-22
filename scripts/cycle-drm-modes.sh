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
  timeout -k 2 --signal=INT "$DWELL" \
    "$BIN" "$CARD" --no-seat --drm-mode "$idx" </dev/null
  sleep 1
done <<<"$modes"

echo "Done — re-run your favorite with: $BIN $CARD --no-seat --drm-mode <index>"
