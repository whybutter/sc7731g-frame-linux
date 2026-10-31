#!/bin/sh
# Builds for SC7731G frames (the VTimes VT10101). Everything runs in Docker:
# a toolchain image (build/Dockerfile) and a cache volume for sources and
# object trees (sc7731g-frame-linux-cache). Outputs land in out/ (gitignored).
#
#   ./build.sh firmware DIR   firmware/ from YOUR frame's partition dumps in DIR
#                             (docs/firmware.md); the kernel step needs it
#   ./build.sh kernel         out/zImage, out/uImage, out/sc7731g-vtimes-vt10101.dtb
#   ./build.sh bootimg        out/boot-linux.img, the boot image cboot loads
#   ./build.sh shell          a shell in the build container
#
# OUT=<dir> builds into another output directory; OBJ=<name> uses a separate
# object tree, so two builds can run at once; WIFI=1 adds the USB-dongle
# test drivers and tools (docs/wifi.md).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
img=sc7731g-frame-linux-build
step=${1:-}
[ -n "$step" ] || { sed -n '6,10p' "$0"; exit 1; }
docker build -q -t $img "$here/build" >/dev/null
vols="-v $here:/work -v $here/build/versions.env:/versions.env:ro -v sc7731g-frame-linux-cache:/cache"
case $step in
  shell) exec docker run --rm -it $vols $img bash ;;
  firmware)
    dir=${2:?usage: ./build.sh firmware DIR holding boot.img, wcnmodem.img, system.img}
    dir=$(cd "$dir" && pwd)
    exec docker run --rm -v "$dir:/in:ro" $vols $img sh /work/firmware/extract.sh /in /work/firmware ;;
  kernel) exec docker run --rm -e OUT="${OUT:-out}" -e OBJ="${OBJ:-}" -e WIFI="${WIFI:-}" $vols $img sh /work/build/kernel.sh ;;
  bootimg)
    o=${OUT:-out}
    exec docker run --rm $vols $img python3 /work/tools/mkbootimg-sprd.py \
      --kernel /work/$o/zImage --dtb /work/$o/sc7731g-vtimes-vt10101.dtb \
      -o /work/$o/boot-linux.img ;;
  *) echo "unknown step: $step"; exit 1 ;;
esac
