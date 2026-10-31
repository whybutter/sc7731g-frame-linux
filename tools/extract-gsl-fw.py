#!/usr/bin/env python3
"""Pull the gslX680 touch firmware out of the VT10101's own Android kernel.

The vendor driver has it compiled in (GSLX680_FW): an array of {u32 register,
u32 value} pairs, one page select (0xf0) then 0x00..0x7c, 33 pairs a page --
exactly the file format mainline's silead driver loads. The array is found by
that shape alone (4719 pairs, 143 pages on this unit's 2018 build; the last
page holds the firmware's own header and date). The blob is the vendor's, so
it is not in git: build.sh puts it in the initramfs from here.

  extract-gsl-fw.py BOOT_KERNEL OUT.fw
  BOOT_KERNEL: the kernel of the boot partition (uncompressed on this unit),
  e.g. unpacked from your own backup of the boot partition (boot.img) by
  tools/unbootimg.py; firmware/extract.sh does all of this
"""
import struct, sys

d = open(sys.argv[1], 'rb').read()
runs, i, n = [], 0, len(d) // 4 * 4
while i < n - 8:
    if struct.unpack_from('<I', d, i)[0] == 0xf0:
        j, exp = i, None
        while j < n - 8:
            a = struct.unpack_from('<I', d, j)[0]
            if a == 0xf0:
                exp = 0
            elif a == exp:
                exp += 4
            else:
                break
            j += 8
        if (j - i) // 8 > 1000:
            runs.append((i, j))
            i = j
            continue
    i += 4
if len(runs) != 1:
    sys.exit(f'expected exactly one firmware-shaped array, found {len(runs)}')
s, e = runs[0]
# a page is complete: drop a trailing partial page
e -= ((e - s) // 8 % 33) * 8
open(sys.argv[2], 'wb').write(d[s:e])
print(f'{(e - s) // 8} pairs, {(e - s) // 8 // 33} pages, from offset 0x{s:x}')
