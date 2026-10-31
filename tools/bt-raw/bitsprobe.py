# bitsprobe.py SECS LO HI UAP -- load a correlator window and HEC seed into the
# DUT test link (descriptor 0x8a904cfc sync, 0x8a904d08 UAP|LAP), then with the
# C3 beacon on and off sample the RX buffer head and the BB status words, and
# print every distinct RX-buffer content seen with its count
import collections, os, struct, sys, time, urllib.request
sys.path.insert(0, '/tmp/fbt')
from hcipeek import peek
from poke import poke
SECS = float(sys.argv[1]); LO = int(sys.argv[2], 0); HI = int(sys.argv[3], 0); UAP = int(sys.argv[4], 0)


def beacon(on):
    urllib.request.urlopen(os.environ.get('C3_URL', 'http://c3-bt-raw.local') + '/switch/advertising/' + ('turn_on' if on else 'turn_off'),
                           data=b'x', timeout=5).read()


poke(0x8a904d08, [(UAP << 24) | 0xa5f0c3])
poke(0x8a904cfc, [LO, HI])
time.sleep(0.05)
for on in (True, False, True):
    beacon(on)
    rx = collections.Counter(); st = collections.defaultdict(collections.Counter); n = 0
    end = time.time() + SECS
    while time.time() < end:
        _, d = peek(0x40086400, 16)
        rx[d.hex()] += 1
        _, r = peek(0x40086020, 16)
        w = struct.unpack('<16I', r)
        for i in (0, 4, 8, 10, 12):   # 0x20 0x30 0x40 0x48 0x50
            st[0x40086020 + 4 * i][w[i]] += 1
        n += 1
    print('=== beacon %s, %d samples' % ('ON' if on else 'off', n))
    for k, c in rx.most_common(6):
        print('  rx %s x%d' % (k, c))
    for a in sorted(st):
        if len(st[a]) > 1:
            print('  %08x %s' % (a, ' '.join('%08x:%d' % kv for kv in st[a].most_common(4))))
