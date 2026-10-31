#!/bin/sh
# pskey.sh OUT -- the 144-byte PSKEY block cp2's Bluetooth wants before its
# first HCI command (docs/bluetooth.md), built at every start from the
# frame's own Android: connectivity_configure.ini is in the system partition,
# which stays on the eMMC untouched, and is mounted read-only without journal
# replay (noload) just long enough to read it. Nothing from Android is copied
# into the image.
#
# The BD_ADDR: the frame has no factory one (no btmac.txt in productinfo),
# so bt-pskey.py derives it from the SoC serial on the kernel command line,
# the same address on every boot and different on every frame.
# Needs python3 and tools/bt-pskey.py installed as /usr/lib/sc7731g-bt/bt-pskey.py.
set -e
out=${1:?usage: pskey.sh OUT}
d=$(dirname "$out")
mkdir -p "$d/system"
serial=$(sed -n 's/.*androidboot.serialno=\([^ ]*\).*/\1/p' /proc/cmdline)
[ -n "$serial" ] || { echo "pskey.sh: no androidboot.serialno" >&2; exit 1; }
mount -t ext4 -o ro,noload /dev/disk/by-partlabel/system "$d/system"
trap 'umount "$d/system" 2>/dev/null; rmdir "$d/system" 2>/dev/null' EXIT
python3 /usr/lib/sc7731g-bt/bt-pskey.py "$d/system/etc/connectivity_configure.ini" \
	"$out" --serial "$serial"
