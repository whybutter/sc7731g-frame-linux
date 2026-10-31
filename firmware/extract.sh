#!/bin/sh
# extract.sh IN OUT -- the vendor firmware this tree needs, cut out of YOUR
# frame's own partition dumps (docs/firmware.md). Run by `./build.sh
# firmware DIR`, in the build container. Nothing here is ever committed:
# OUT's results are gitignored, and the repository carries only the tools
# and the checksums below (the Nexmon way: extract from the device you own,
# patch, check every step).
#
# IN must hold three dumps of the frame's eMMC partitions, by name:
#   boot.img      Android's boot partition (p16) as the frame shipped it:
#                 its kernel carries the touch firmware
#   wcnmodem.img  the WCN co-processor's firmware partition (p9)
#   system.img    Android's system partition (p17): the WCN calibration ini
#
# Out come:
#   silead/gsl1680.fw  the GSL1680 touch firmware (tools/extract-gsl-fw.py)
#   wcnmodem.bin       cp2's firmware for Wi-Fi and Bluetooth, the partition
#                      as is, then any patches/*.py applied in name order
#   wifi-nvm.bin       the 256-byte Wi-Fi calibration block (tools/wcn-nvm.py)
#
# The Bluetooth PSKEY is not made here: it embeds a BD_ADDR derived from each
# frame's serial, so the frame builds it at boot (rootfs/usr/lib/sc7731g-bt).
#
# Checksums: each input and output is compared with what the known firmware
# build (VTimes_VT10101_V01_20250617) gives. A different input is a warning --
# another build may work, but its outputs are unverified; a known input that
# gives a different output is an error.
set -eu
in=${1:?usage: extract.sh IN OUT}
out=${2:?usage: extract.sh IN OUT}
here=$(cd "$(dirname "$0")" && pwd)
tools=$here/../tools
sums=$here/SHA256SUMS
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$out/silead"
warn=0

known() {	# known NAME -> the expected sha256, or nothing
  awk -v n="$1" '$2 == n { print $1 }' "$sums"
}
check_in() {	# check_in NAME FILE: is FILE the known input NAME?
  want=$(known "in/$1"); got=$(sha256sum "$2" | cut -d' ' -f1)
  if [ "$got" = "$want" ]; then echo "  in  $1: known ($got)"; return 0; fi
  echo "  in  $1: UNKNOWN ($got) -- not the tested firmware build"; warn=1; return 1
}
check_out() {	# check_out NAME FILE INPUT_WAS_KNOWN
  want=$(known "out/$1"); got=$(sha256sum "$2" | cut -d' ' -f1)
  if [ "$got" = "$want" ]; then echo "  out $1: verified"; return 0; fi
  if [ "$3" = 1 ]; then echo "  out $1: MISMATCH ($got, expected $want)"; exit 1; fi
  echo "  out $1: unverified ($got)"
}
for f in boot.img wcnmodem.img system.img; do
  [ -f "$in/$f" ] || { echo "missing $in/$f (docs/firmware.md says how to dump it)"; exit 1; }
done

echo "touch firmware (from boot.img's kernel)"
k=1; check_in boot.img "$in/boot.img" || k=0
mkdir -p "$tmp/boot"
python3 "$tools/unbootimg.py" "$in/boot.img" "$tmp/boot" >/dev/null
python3 "$tools/extract-gsl-fw.py" "$tmp/boot/kernel" "$out/silead/gsl1680.fw"
check_out silead/gsl1680.fw "$out/silead/gsl1680.fw" $k

echo "WCN firmware (wcnmodem partition)"
k=1; check_in wcnmodem.img "$in/wcnmodem.img" || k=0
cp "$in/wcnmodem.img" "$tmp/wcnmodem.bin"
check_out wcnmodem.bin "$tmp/wcnmodem.bin" $k
# patches/NN-name.py: each checks its own input and output (patches/README.md)
for p in "$here"/patches/*.py; do
  [ -e "$p" ] || continue
  echo "  patch $(basename "$p")"
  python3 "$p" "$tmp/wcnmodem.bin" "$tmp/wcnmodem.patched" || exit 1
  mv "$tmp/wcnmodem.patched" "$tmp/wcnmodem.bin"
done
cp "$tmp/wcnmodem.bin" "$out/wcnmodem.bin"

echo "Wi-Fi calibration (system.img's connectivity ini files)"
k=1; check_in system.img "$in/system.img" || k=0
# read without mounting: debugfs dumps a file out of the ext4 image
for f in connectivity_configure.ini connectivity_calibration.ini; do
  debugfs -R "dump /etc/$f $tmp/$f" "$in/system.img" 2>/dev/null
  [ -s "$tmp/$f" ] || { echo "no /etc/$f in system.img"; exit 1; }
  check_in "$f" "$tmp/$f" || k=0
done
python3 "$tools/wcn-nvm.py" "$tmp/connectivity_configure.ini" \
  "$tmp/connectivity_calibration.ini" "$out/wifi-nvm.bin" >/dev/null
check_out wifi-nvm.bin "$out/wifi-nvm.bin" $k

[ $warn = 0 ] && echo "firmware/: all inputs known, all outputs verified" ||
  echo "firmware/: done, with unknown inputs (see above); test before you trust it"
