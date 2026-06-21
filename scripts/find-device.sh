#!/usr/bin/env bash
# Snapshot USB devices, wait for you to plug one in, then report the new device
# (bus, device addr, VID:PID, description). Useful to know which bus to filter.
set -euo pipefail

echo "Current USB devices snapshotted. Plug the device in now..."
before="$(lsusb | sort)"
while :; do
  sleep 1
  after="$(lsusb | sort)"
  newline="$(comm -13 <(echo "$before") <(echo "$after") || true)"
  if [ -n "$newline" ]; then
    echo "NEW DEVICE:"
    echo "$newline"
    # Parse: "Bus 005 Device 033: ID 1314:1520 ..."
    bus="$(echo "$newline"  | sed -nE 's/^Bus ([0-9]+).*/\1/p' | head -1)"
    echo "Bus number (use as usbmon arg): $((10#$bus))"
    break
  fi
done
