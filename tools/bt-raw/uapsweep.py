# uapsweep.py DWELL -- with the correlator window loaded and the C3 beacon on,
# step the test link's UAP (the BR header's HEC seed) through 0..255 and report
# any UAP under which the RX buffer head changes or 0x40086050 shows a status
# not seen before: the HEC passing should look different from failing
import collections, struct, sys, time
sys.path.insert(0, '/tmp/fbt')
from hcipeek import peek
from poke import poke
DWELL = float(sys.argv[1])
_, base_rx = peek(0x40086400, 16)
seen50 = collections.Counter()
for uap in range(256):
    poke(0x8a904d08, [(uap << 24) | 0xa5f0c3])
    loc = collections.Counter(); rxs = set()
    end = time.time() + DWELL
    while time.time() < end:
        _, d = peek(0x40086040, 8)
        w = struct.unpack('<8I', d)
        loc[(w[2], w[4])] += 1
        _, rx = peek(0x40086400, 4)
        rxs.add(rx)
    new = [k for k in loc if k not in seen50]
    seen50.update(loc)
    changed = [r.hex() for r in rxs if r != base_rx[:16]]
    if new or changed:
        print('uap %3d new 48/50: %s rx changed: %s' % (uap, ' '.join('%08x/%08x:%d' % (a, b, loc[(a, b)]) for a, b in new), changed[:2]), flush=True)
print('done; all 48/50 seen:', ' '.join('%08x/%08x:%d' % (a, b, c) for (a, b), c in seen50.most_common(10)))
