#!/usr/bin/env bash
# Capture USB traffic on a given bus (or all buses) using dumpcap + usbmon.
# Usage: capture-usb.sh [BUS]   (BUS omitted or 0 => capture ALL buses)
set -euo pipefail

BUS="${1:-0}"
OUTDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/capture"
mkdir -p "$OUTDIR"
OUT="$OUTDIR/usbcap-bus${BUS}-$(date +%Y%m%d-%H%M%S).pcapng"

if ! lsmod | grep -q '^usbmon'; then
  echo "ERROR: usbmon module not loaded. Run: sudo modprobe usbmon" >&2
  exit 1
fi

# Use tcpdump: world-executable (no wireshark-group gate like dumpcap) and reads
# /dev/usbmon${BUS} directly (grant access with: sudo setfacl -m u:$USER:r /dev/usbmon${BUS}).
echo "Capturing usbmon${BUS} -> $OUT"
echo "Plug in / exercise the device now. Stop with Ctrl-C."
exec tcpdump -i "usbmon${BUS}" -s 0 -w "$OUT"
