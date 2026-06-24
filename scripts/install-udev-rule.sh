#!/usr/bin/env bash
# Install the Carlinkit dongle udev rule so libusb can claim the device without
# root, and make sure the invoking user is in the rule's access group (dialout).
#
# Run it directly; it re-execs itself with sudo if needed:
#
#   ./scripts/install-udev-rule.sh          # from a checkout
#   ./install-udev-rule.sh                  # alongside 99-carlinkit.rules on a device
#
# After it runs, replug the dongle (or it is re-triggered for you) and the node
# becomes group 'dialout', mode 0660 — claimable by any dialout member.
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
RULE_SRC="$SCRIPT_DIR/99-carlinkit.rules"
RULE_DST=/etc/udev/rules.d/99-carlinkit.rules
TARGET_USER="${SUDO_USER:-$USER}"

if [ ! -f "$RULE_SRC" ]; then
  echo "error: $RULE_SRC not found (keep this script next to 99-carlinkit.rules)" >&2
  exit 1
fi

if [ "$(id -u)" -ne 0 ]; then
  echo "+ sudo $0 $*"
  exec sudo "$0" "$@"
fi

install -m 0644 "$RULE_SRC" "$RULE_DST"
echo "installed $RULE_DST"

# The rule grants the dongle to GROUP=dialout; make sure the user is in it.
if id -nG "$TARGET_USER" | tr ' ' '\n' | grep -qx dialout; then
  echo "$TARGET_USER already in dialout"
else
  usermod -aG dialout "$TARGET_USER"
  echo "added $TARGET_USER to dialout — log out/in (or replug) for it to take effect"
fi

# Reload rules and re-apply to an already-plugged dongle.
udevadm control --reload
udevadm trigger --subsystem-match=usb --attr-match=idVendor=1314 || true
sleep 1

NODE=$(lsusb | awk '/1314:15(20|21)/{printf "/dev/bus/usb/%03d/%03d\n",$2,$4}' | head -1)
if [ -n "${NODE:-}" ] && [ -e "$NODE" ]; then
  printf 'dongle node: '; ls -l "$NODE"
  echo "(should now be group 'dialout', mode 0660)"
else
  echo "no Carlinkit dongle plugged right now — the rule applies on next plug"
fi
echo "done."
