#!/bin/sh
# uboot-fastboot.sh IMAGE [PARTITION] -- flash a partition through the vendor
# U-Boot's fastboot, catching U-Boot on the UART at the next (cold) boot.
#
# Why: on 2026-10-27 a write to the boot partition from our own Linux hung
# (the SDHCI write DMA stalls, docs/mainline.md), leaving p16 half-written and
# the card mid-transfer, so a warm restart would leave the boot ROM unable to
# read it. This path doesn't need our Linux, Android or adb: power the frame
# on, stop autoboot on the UART, `cboot fastboot` (the U-Boot help lists
# recovery, fastboot, dloader, charge, normal), then the Mac's fastboot.
#
# Start it, then power-cycle the frame. Defaults to the boot partition.
set -eu
img=${1:?usage: uboot-fastboot.sh IMAGE [PARTITION]}
part=${2:-boot}
port=${VT10101_UART:-/dev/ttyUSB0}

python3 - "$port" <<'EOF'
import re, serial, sys, time
s = serial.Serial(sys.argv[1], 115200, timeout=0.02)
buf = b""; t = time.time()
print("waiting for U-Boot (power-cycle the frame now)", flush=True)
while time.time() - t < 300:
    s.write(b" ")
    buf += s.read(4096)
    if b"U-Boot" in buf and re.search(rb"=> *$", buf[-50:].rstrip(b"\r\n ") + b" "):
        break
else:
    sys.exit("no U-Boot prompt in 5 minutes")
time.sleep(0.5); s.read(65536)
s.write(b"cboot fastboot\r")
t = time.time(); out = b""
while time.time() - t < 8:
    out += s.read(4096)
print(out.decode("utf-8", "replace")[-400:], flush=True)
EOF

for i in $(seq 1 30); do
  fastboot devices | grep -q . && break
  sleep 1
done
fastboot devices | grep -q . || { echo "no fastboot device"; exit 1; }
fastboot flash "$part" "$img"
fastboot reboot
