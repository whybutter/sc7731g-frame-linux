#!/usr/bin/env python3
"""Pack our kernel into the boot image the vendor U-Boot's cboot loads.

  mkbootimg-sprd.py --kernel Image --dtb x.dtb [--ramdisk r.cpio.gz]
                    --cmdline "..." -o boot-linux.img

The format is the frame's own boot.img (read with tools/unbootimg.py):
an Android v0 boot image, page 2048, with the kernel (the vendor's is an
uncompressed ARM `Image`; ours is a zImage, because multi_v7 plus the
built-in initramfs is 33 MB uncompressed against a 16 MiB boot partition --
a zImage relocates itself wherever cboot puts it), the ramdisk, and in the header's spare word after page_size the size
of a Spreadtrum DT table appended after the ramdisk:

  "SPRD", version 1, entry count, then per entry
  platform_id, variant_id, soc_rev, offset, size   (u32 each, little endian)

The frame's table has one entry: 0x227e, 1, 0x20000, blob at 0x800. cboot
picks the entry by those ids, so ours copies them. Load addresses are the
vendor header's too (kernel 0x8000, ramdisk 0x1000000, second 0xf00000, tags
0x100; cboot adds its own base). Our initramfs is built into the kernel, so
the ramdisk can be a token one -- cboot has only been seen with a real one.
"""
import argparse
import hashlib
import struct

PAGE = 2048
PLATFORM_ID, VARIANT_ID, SOC_REV = 0x227E, 1, 0x20000


def pad(b, n=PAGE):
    return b + b"\0" * (-len(b) % n)


def dt_table(dtb):
    head = b"SPRD" + struct.pack("<II", 1, 1)
    head += struct.pack("<5I", PLATFORM_ID, VARIANT_ID, SOC_REV, PAGE, len(dtb))
    return pad(head) + pad(dtb)


ap = argparse.ArgumentParser()
ap.add_argument("--kernel", required=True)
ap.add_argument("--dtb", required=True)
ap.add_argument("--ramdisk")
ap.add_argument("--cmdline", default="console=ttyS1,115200n8")
ap.add_argument("-o", "--out", required=True)
a = ap.parse_args()

kernel = open(a.kernel, "rb").read()
if kernel[:4] == b"\x27\x05\x19\x56":
    raise SystemExit("that's a uImage; cboot wants a bare Image or zImage")
# an empty newc cpio (just the trailer) unless one is given
ramdisk = open(a.ramdisk, "rb").read() if a.ramdisk else (
    b"070701" + b"0" * 8 * 11 + b"0000000B" + b"0" * 8 + b"TRAILER!!!\0")
dt = dt_table(open(a.dtb, "rb").read())
if len(a.cmdline) >= 512:
    raise SystemExit("cmdline over 511 bytes")

sha = hashlib.sha1()
for part in (kernel, ramdisk, b"", dt):
    sha.update(part)
    sha.update(struct.pack("<I", len(part)))

# v0 header; the dt size goes in the first of the two spare words
hdr = b"ANDROID!" + struct.pack("<10I", len(kernel), 0x8000, len(ramdisk), 0x1000000,
                                0, 0xF00000, 0x100, PAGE, len(dt), 0)
hdr += b"\0" * 16                               # name
hdr += a.cmdline.encode().ljust(512, b"\0")
hdr += sha.digest().ljust(32, b"\0")
hdr += b"\0" * 1024                             # extra cmdline
img = pad(hdr) + pad(kernel) + pad(ramdisk) + dt
open(a.out, "wb").write(img)
print(f"{a.out}: {len(img)} bytes (kernel {len(kernel)}, ramdisk {len(ramdisk)}, dt {len(dt)})")
