# memrd.py ADDR NWORDS -- read cp2 RAM straight from the AP (/dev/mem, no-map region)
import mmap, os, struct, sys
a = int(sys.argv[1], 0); n = int(sys.argv[2], 0)
pg = a & ~0xfff
fd = os.open('/dev/mem', os.O_RDONLY | os.O_SYNC)
m = mmap.mmap(fd, (a - pg) + n * 4 + 4096, mmap.MAP_SHARED, mmap.PROT_READ, offset=pg)
w = struct.unpack_from('<%dI' % n, m, a - pg)
for i in range(0, n, 4):
    print('%08x: %s' % (a + 4 * i, ' '.join('%08x' % x for x in w[i:i + 4])))
