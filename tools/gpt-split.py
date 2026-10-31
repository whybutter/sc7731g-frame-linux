#!/usr/bin/env python3
"""Split the VT10101's userdata into A/B root partitions (docs/install-plan.md,
Phase 3): new primary and secondary GPT images from the live ones.

  gpt-split.py PRIMARY(34 sectors) SECONDARY(last 33) OUTDIR

The frame's eMMC (30777344 sectors) has 23 partitions, the last being
userdata (p23, 13.4 GiB to the end of the usable area). p1-p22 stay exactly
where they are -- U-Boot's cboot finds boot/recovery by name, our uboot-boot
tool loads from p20 by number -- and p23 shrinks to 1 GiB so the stock Android
kept in recovery still has a /data while it's the way back. After it:

  p24 fl-boot   64 MiB   FAT: FRAMEBOOT (hook, overlay, slot.env)
  p25 fl-a      2 GiB    root slot A
  p26 fl-b      2 GiB    root slot B
  p27 fl-data   the rest persistent state

Partition GUIDs are fixed (5654544d = "VTTM", then the partition number) so
the initramfs and an updater can name them without reading the table.
Writes OUTDIR/gpt-primary.img (LBA 0..33) and gpt-secondary.img (the last 33
LBAs), and refuses to run on anything that isn't the table it expects.
"""
import struct
import sys
import uuid
import zlib

SECTOR = 512
DISK = 30777344
MiB = 2048  # sectors

LINUX_FS = uuid.UUID("0fc63daf-8483-4772-8e79-3d69d8477de4")
BASIC_DATA = uuid.UUID("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7")

prim = bytearray(open(sys.argv[1], "rb").read())
sec = bytearray(open(sys.argv[2], "rb").read())
out = sys.argv[3]
assert len(prim) == 34 * SECTOR and len(sec) == 33 * SECTOR


def header(raw):
    f = struct.unpack("<8sIIIIQQQQ16sQIII", raw[:92])
    assert f[0] == b"EFI PART" and f[2] == 92
    return f


ph = header(prim[SECTOR:])
n, esz = ph[11], ph[12]
assert (ph[5], ph[6], ph[7], ph[8], n, esz) == (1, DISK - 1, 34, DISK - 34, 128, 128)
ents = bytearray(prim[2 * SECTOR:2 * SECTOR + n * esz])
assert zlib.crc32(ents) & 0xFFFFFFFF == ph[13]
# the secondary's entries must be the primary's
assert sec[:n * esz] == ents, "secondary entries differ from primary"


def entry(i):
    return ents[i * esz:(i + 1) * esz]


def name(i):
    return entry(i)[56:128].decode("utf-16le").rstrip("\0")


ud = 22  # p23
first, last = struct.unpack("<QQ", entry(ud)[32:48])
assert name(ud) == "userdata" and (first, last) == (3334144, DISK - 34)
assert all(struct.unpack("<Q", entry(i)[32:40])[0] == 0 for i in range(23, n)), \
    "entries after p23 aren't empty"

layout = [  # (index, name, type, sectors or None for the rest)
    (22, "userdata", BASIC_DATA, 1024 * MiB),
    (23, "fl-boot", BASIC_DATA, 64 * MiB),
    (24, "fl-a", LINUX_FS, 2048 * MiB),
    (25, "fl-b", LINUX_FS, 2048 * MiB),
    (26, "fl-data", LINUX_FS, None),
]
lba = first
for i, nm, typ, size in layout:
    end = (DISK - 34) if size is None else lba + size - 1
    if i == 22:  # keep userdata's type, GUID, attributes and name; resize only
        e = bytearray(entry(i))
        e[32:48] = struct.pack("<QQ", lba, end)
    else:
        guid = uuid.UUID("5654544d-0000-4000-8000-%012x" % (i + 1))
        e = bytearray(typ.bytes_le + guid.bytes_le + struct.pack("<QQQ", lba, end, 0)
                      + nm.encode("utf-16le").ljust(72, b"\0"))
    ents[i * esz:(i + 1) * esz] = e
    print("p%-2d %-9s %9d..%9d  %8.1f MiB" % (i + 1, nm, lba, end, (end - lba + 1) / MiB))
    lba = end + 1

ecrc = zlib.crc32(ents) & 0xFFFFFFFF


def hdr(cur, alt, elba):
    h = bytearray(struct.pack("<8sIIIIQQQQ16sQIII", b"EFI PART", ph[1], 92, 0, 0,
                              cur, alt, 34, DISK - 34, ph[9], elba, n, esz, ecrc))
    h[16:20] = struct.pack("<I", zlib.crc32(h) & 0xFFFFFFFF)
    return bytes(h).ljust(SECTOR, b"\0")


new_prim = prim[:SECTOR] + hdr(1, DISK - 1, 2) + ents
new_sec = ents + hdr(DISK - 1, 1, DISK - 33)
assert len(new_prim) == 34 * SECTOR and len(new_sec) == 33 * SECTOR
open(out + "/gpt-primary.img", "wb").write(new_prim)
open(out + "/gpt-secondary.img", "wb").write(new_sec)
print("entries crc %08x; wrote %s/gpt-primary.img (LBA 0) and gpt-secondary.img (LBA %d)"
      % (ecrc, out, DISK - 33))
