# leprobe.py SECS -- does the BR correlator fire on a BLE advert?
#
# Run on the frame with the bridge up on wcnmodem-peek.bin and the DUT test
# link already running (dut.py 5 0 0 0 3 17: single frequency, BR channel 0 =
# 2402 MHz = LE channel 37). For each pattern in turn -- a dummy, the C3
# beacon's (tools/bt-raw/lepattern.py 0x42 18 C3:BE:12:00:00:01), and that one
# with 4 bits flipped, and the C3's pattern with the beacon switched off over
# Wi-Fi -- load it into the test link's descriptor (where the
# scheduler copies it into 0x40086008/0c every slot), then sample the baseband
# words 0x40086000..7f and the head of the RX buffer (0x40086400..1f) for SECS,
# and print, per word, how many distinct values and samples it had. A word that
# only moves under the right pattern is the hit signal.
import collections, os, struct, sys, time, urllib.request
sys.path.insert(0, '/tmp/fbt')
from hcipeek import peek
from poke import poke

SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 8
DESC_SYNC = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x8a904cfc
C3 = os.environ.get('C3_URL', 'http://c3-bt-raw.local')   # the ESP32-C3 beacon (esp32c3/c3-bt-raw.yaml)


def beacon(on):
    """the C3's advertising switch; a POST body must not be empty (411)"""
    urllib.request.urlopen(C3 + '/switch/advertising/' + ('turn_on' if on else 'turn_off'),
                           data=b'x', timeout=5).read()


# (name, correlator low, high, beacon on): the decisive pair is c3 with the
# beacon on vs off -- same pattern, only the transmitter changes
PATTERNS = [
    ('dummy', 0x12345678, 0x9abcdef0, True),
    ('c3, beacon on', 0x8e89bed6, 0xa156c0cf, True),
    ('c3, beacon OFF', 0x8e89bed6, 0xa156c0cf, False),
    ('c3^4bits, beacon on', 0x8e89bed6, 0xa156c0cf ^ 0x00011011, True),
    ('c3, beacon on again', 0x8e89bed6, 0xa156c0cf, True),
]

for name, lo, hi, on in PATTERNS:
    beacon(on)
    poke(DESC_SYNC, [lo, hi])
    time.sleep(0.05)
    st, d = peek(0x40086008, 4)
    cur = struct.unpack('<2I', d[:8])
    h = collections.defaultdict(collections.Counter)
    rx = collections.Counter()
    n = 0
    end = time.time() + SECS
    while time.time() < end:
        st, d = peek(0x40086000, 32)
        for i, x in enumerate(struct.unpack('<32I', d)):
            h[i][x] += 1
        st, d = peek(0x40086400, 8)
        rx[d.hex()] += 1
        n += 1
    print('=== %s: correlator %08x %08x, %d samples' % (name, cur[0], cur[1], n))
    for i in sorted(h):
        if len(h[i]) > 1 and i not in (6, 7, 14):  # clock x2, live RSSI
            print('  %08x %3d distinct: %s' % (0x40086000 + 4 * i, len(h[i]),
                  ' '.join('%08x:%d' % kv for kv in h[i].most_common(4))))
    print('  rxbuf %d distinct: %s' % (len(rx), ' '.join('%s:%d' % kv for kv in rx.most_common(3))))
