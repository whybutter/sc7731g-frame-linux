# memfind.py WORD [WORD2] -- find a 32-bit word (optionally followed by WORD2) in cp2's 2 MiB
import mmap, os, struct, sys
base, size = 0x8a800000, 0x200000
fd = os.open('/dev/mem', os.O_RDONLY | os.O_SYNC)
m = mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ, offset=base)
buf = bytes(m[:])
w1 = struct.pack('<I', int(sys.argv[1], 0))
w2 = struct.pack('<I', int(sys.argv[2], 0)) if len(sys.argv) > 2 else None
i = buf.find(w1)
while i >= 0:
    ctx = struct.unpack_from('<8I', buf, max(0, i - 12))
    if w2 is None or buf[i + 4:i + 8] == w2 or buf[i - 4:i] == w2:
        print('%08x: %s' % (base + i, ' '.join('%08x' % x for x in ctx)))
    i = buf.find(w1, i + 1)
