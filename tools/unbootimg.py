#!/usr/bin/env python3
"""Split an Android boot image (v0, with Spreadtrum's dt section) into
OUTDIR/kernel, ramdisk, second, dt. The VT10101's kernel comes out
uncompressed; dt is a Spreadtrum dt.img ("SPRD", FDT at 0x800).

  unbootimg.py BOOT.IMG OUTDIR
"""
import struct, sys, os
img = open(sys.argv[1], "rb").read(); out = sys.argv[2]
assert img[:8] == b"ANDROID!", img[:8]
ks, ka, rs, ra, ss, sa, ta, ps, dts = struct.unpack("<9I", img[8:44])
name = img[48:64].rstrip(b"\0"); cmd = img[64:576].rstrip(b"\0")
print("kernel", ks, hex(ka), "ramdisk", rs, hex(ra), "second", ss, "tags", hex(ta), "page", ps, "dt_size(field)", dts)
print("name", name, "cmdline", cmd)
off = ps
def take(n):
    global off
    b = img[off:off+n]; off += (n + ps - 1)//ps*ps; return b
for nm, n in (("kernel", ks), ("ramdisk", rs), ("second", ss), ("dt", dts)):
    b = take(n)
    if n: open(os.path.join(out, nm), "wb").write(b)
