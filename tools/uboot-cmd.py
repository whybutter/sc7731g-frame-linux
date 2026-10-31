#!/usr/bin/env python3
"""uboot-cmd.py "COMMAND[;;COMMAND...]" [SECONDS] -- wait for the vendor U-Boot
on the UART (power-cycle the frame after starting this), stop its autoboot,
run the COMMANDs one by one (";;" separates them; each waits for the `=>`
prompt, except the last), and show the output for SECONDS (default 10).

`ext4load mmc 0:14 0x82000000 /vt10101/uImage;;ext4load mmc 0:14 0x84000000
/vt10101/sc7731g-vtimes-vt10101.dtb;;setenv bootargs ...;;bootm 0x82000000 -
0x84000000` boots whatever uboot-boot.py last staged in the cache partition,
with any command line -- the way into a debug shell when the installed boot
goes straight into the userland and the userland can't be reached.

The usual one is `cboot recovery`: it boots the recovery partition, which
holds stock Android since the install's Phase 2, whatever state the boot
partition is in -- the way back when our Linux can't boot or can't write
(2026-10-27: a write from our Linux hung and left the boot partition
half-written). From Android, adb writes partitions as in Phase 2.
"""
import os
import sys
import time

import serial

port = os.environ.get("VT10101_UART", "/dev/ttyUSB0")
command = sys.argv[1]
show = float(sys.argv[2]) if len(sys.argv) > 2 else 10
s = serial.Serial(port, 115200, timeout=0.02)
buf = b""
t = time.time()
print("waiting for U-Boot (power-cycle the frame now)", flush=True)
# Spaces only after U-Boot's banner -- nothing the boot ROM or SPL could take
# for input. U-Boot's bootdelay is 0 here ("Hit any key to stop autoboot:
# 0"): it stops only on a key already waiting, so the spaces start early.
#
# Twice on 2026-10-27 the frame sat on a black screen with this waiting, and
# the first guess was that the spaces stopped it booting (it booted when the
# adapter was unplugged). Wrong: it was at the `=>` prompt both times, which
# is black because U-Boot hasn't lit the panel yet. The prompt went
# unnoticed, most likely because the spaces queued since the banner
# overflow U-Boot's command line buffer and it answers each one with a BEL
# (\a), which ended the buffer after the prompt. Now BELs and spaces are
# dropped before looking.
while time.time() - t < float(os.environ.get("UBOOT_WAIT", 600)):
    if b"U-Boot 20" in buf:
        s.write(b" ")
    buf += s.read(4096)
    tail = buf[-2000:].replace(b"\a", b"").rstrip(b"\r\n ")
    if b"U-Boot" in buf and tail.endswith(b"=>"):
        break
else:
    sys.exit("no U-Boot prompt in time (UBOOT_WAIT seconds, default 600)")
# Ctrl-C drops whatever spaces are still on U-Boot's command line
s.write(b"\x03")
time.sleep(0.3)
s.read(65536)
cmds = command.split(";;")
for c in cmds[:-1]:
    s.write(c.strip().encode() + b"\r")
    got = b""
    t = time.time()
    while time.time() - t < 30 and not got.rstrip().endswith(b"=>"):
        got += s.read(4096)
    print(got.decode("utf-8", "replace"), flush=True)
s.write(cmds[-1].strip().encode() + b"\r")
out = b""
t = time.time()
while time.time() - t < show:
    out += s.read(4096)
print(out.decode("utf-8", "replace"), flush=True)
