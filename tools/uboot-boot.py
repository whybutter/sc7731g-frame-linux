#!/usr/bin/env python3
"""Boot a kernel on the VT10101 from the vendor U-Boot's prompt, over the UART.

The vendor U-Boot (2014.01) sees only the eMMC (no SD driver) and has bootm
but no bootz, and its bootdelay is 0 -- a key held on the console still stops
it. So: the uImage and dtb go into the `cache` partition (p20, ext4 written by
Android, which U-Boot reads; U-Boot numbers partitions in hex, so mmc 0:14),
then this reboots the frame, holds a key until the `=>` prompt, loads both and
boots, and logs the console until --seconds run out or the shell prompt shows.

Nothing is flashed: a power cycle without the key boots whatever the eMMC
boots. Needs the USB-serial adapter on the GND/TX/RX pads (115200 8N1).

  uboot-boot.py [--push DIR] [--no-reboot] [--args "..."] [--seconds N] LOG
    --push DIR   adb-push DIR/uImage and DIR/*.dtb to /cache/vt10101/ first
                 (needs Android up with our adb key)
    --no-reboot  the frame is already at the U-Boot prompt
    --until RE   stop logging this many seconds after RE shows (default: the
                 initramfs shell prompt); with `probe_reboot` in --args use
                 "probe: rebooting" and the log also catches U-Boot coming back
"""
import argparse, glob, os, re, subprocess, sys, time
import serial

PORT = os.environ.get("VT10101_UART", "/dev/ttyUSB0")
KADDR, DADDR = 0x82000000, 0x84000000
DEFAULT_ARGS = "console=ttyS1,115200n8 earlycon ignore_loglevel panic=5"

ap = argparse.ArgumentParser()
ap.add_argument("log")
ap.add_argument("--push")
ap.add_argument("--no-reboot", action="store_true")
ap.add_argument("--args", default=DEFAULT_ARGS)
ap.add_argument("--seconds", type=float, default=60)
ap.add_argument("--dtb", default="sc7731g-vtimes-vt10101.dtb")
ap.add_argument("--until", default=r"~ # $")
ap.add_argument("--after", type=float, default=0)
# --wdt N: arm the SC2713S PMIC watchdog from U-Boot (N seconds) just before
# bootm. The out-3 boot (2026-10-24) hung before /init ran, so initramfs/
# pmic-wdt never armed and the frame sat dead until a replug; armed here, a
# hang anywhere in the kernel resets the frame back to Android. A kernel that
# lives must feed it (`wdt` on the command line runs pmic-wdt, which re-arms
# and feeds), or it is reset after N seconds -- harmless for a probe boot.
ap.add_argument("--wdt", type=int, default=0)
a = ap.parse_args()

if a.push:
    files = [os.path.join(a.push, "uImage")] + glob.glob(os.path.join(a.push, "*.dtb"))
    subprocess.run(["adb", "shell", "mkdir -p /cache/vt10101"], check=True)
    for f in files:
        subprocess.run(["adb", "push", f, "/cache/vt10101/"], check=True)
    subprocess.run(["adb", "shell", "sync"], check=True)

s = serial.Serial(PORT, 115200, timeout=0.02)
log = open(a.log, "wb")
buf = b""

def pump(t, until=None):
    global buf
    end = time.time() + t
    while time.time() < end:
        d = s.read(4096)
        if d:
            buf += d
            log.write(d)
            log.flush()
            sys.stdout.write(d.decode("utf-8", "replace"))
            sys.stdout.flush()
            if until and re.search(until, buf[-2000:]):
                return True
    return False

def cmd(c, t=3.0, until=rb"\n=> ?$"):
    global buf
    buf = b""
    s.write(c.encode() + b"\r")
    return pump(t, until)

if not a.no_reboot:
    subprocess.run(["adb", "reboot"], timeout=30)
    start = time.time()
    while time.time() - start < 90:
        s.write(b" ")
        pump(0.03)
        if b"U-Boot" in buf and re.search(rb"=> *$", buf[-50:].rstrip(b"\r\n ") + b" "):
            break
    else:
        sys.exit("no U-Boot prompt (is the adapter on the pads?)")
pump(0.5)
s.write(b"\r")
pump(0.5)

ok = (cmd(f"ext4load mmc 0:14 0x{KADDR:x} /vt10101/uImage", 20) and
      cmd(f"ext4load mmc 0:14 0x{DADDR:x} /vt10101/{a.dtb}", 10))
if not ok or b"rror" in buf:
    sys.exit("load failed")
cmd(f"setenv bootargs {a.args}")

def adi_rd(reg):
    # the ADI's old-style read (as initramfs/probe's adi()): the slave
    # register's physical address into RD_CMD, the value in RD_DATA[15:0]
    cmd(f"mw.l 0x40030024 0x{0x40038000 + reg:x}")
    cmd("md.l 0x40030028 1")
    m = re.search(rb"40030028: ([0-9a-f]{8})", buf)
    if not m or int(m.group(1), 16) >> 31:
        sys.exit(f"ADI read of 0x{reg:x} failed")
    return int(m.group(1), 16) & 0xffff

def adi_wr(reg, val):
    cmd(f"mw.l 0x{0x40038000 + reg:x} 0x{val:x}")

if a.wdt:
    # same registers and order as the restart handler (kernel/patches/
    # 0001-spi-sprd-adi-sc7731g.patch) and initramfs/pmic-wdt
    adi_wr(0x8e8, (adi_rd(0x8e8) & ~0xf0) | 0x40)  # reset reason: NORMAL
    adi_wr(0x800, adi_rd(0x800) | 4)                # watchdog interface clock
    adi_wr(0x808, adi_rd(0x808) | 4)                # watchdog work clock
    adi_wr(0x60, 0xe551)                            # unlock
    adi_wr(0x48, adi_rd(0x48) | 4)                  # NEW: 32-bit load
    n = a.wdt * 32768
    adi_wr(0x44, n >> 16)
    adi_wr(0x40, n & 0xffff)
    adi_wr(0x48, adi_rd(0x48) | 0xa)                # RUN | RST_EN
    adi_wr(0x60, 0x1aae)                            # lock
    print(f"\n[uboot-boot] PMIC watchdog armed: {a.wdt} s, ctrl 0x{adi_rd(0x48):x}")
buf = b""
s.write(f"bootm 0x{KADDR:x} - 0x{DADDR:x}\r".encode())
if pump(a.seconds, until=a.until.encode()) and a.after:
    pump(a.after)
log.close()
