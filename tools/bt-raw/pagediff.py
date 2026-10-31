# pagediff.py SECS -- with the correlator/UAP already loaded, sample the whole
# 0x40086000 page with the C3 beacon on and off; print words whose value set
# differs between the two (excluding the clock and live RSSI)
import collections, os, struct, sys, time, urllib.request
sys.path.insert(0, '/tmp/fbt')
from hcipeek import peek
SECS = float(sys.argv[1])


def beacon(on):
    urllib.request.urlopen(os.environ.get('C3_URL', 'http://c3-bt-raw.local') + '/switch/advertising/' + ('turn_on' if on else 'turn_off'),
                           data=b'x', timeout=5).read()


vals = {}
for on in (False, True):
    beacon(on)
    h = collections.defaultdict(collections.Counter)
    end = time.time() + SECS
    while time.time() < end:
        _, d = peek(0x40086000, 1024)
        for i, x in enumerate(struct.unpack('<1024I', d)):
            h[i][x] += 1
    vals[on] = h
for i in range(1024):
    if i in (6, 7, 14):
        continue
    a, b = set(vals[False][i]), set(vals[True][i])
    if b - a:
        print('%08x off:%s on-only:%s' % (0x40086000 + 4 * i, ' '.join('%08x' % x for x in list(a)[:3]),
              ' '.join('%08x:%d' % (x, vals[True][i][x]) for x in list(b - a)[:6])))
