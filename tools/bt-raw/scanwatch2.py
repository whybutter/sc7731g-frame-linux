# scanwatch2.py SECS BASE NWORDS -- page scan on; sample a block; report words
# whose value changes, with change times, and how many distinct values
import struct, sys, time, socket
sys.path.insert(0, '/tmp/fbt')
from hcipeek import peek, hci_cmd
s = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_RAW, socket.BTPROTO_HCI)
s.bind((0,)); s.setsockopt(0, 2, struct.pack('<IIIHxx', 0x10, 0xffffffff, 0xffffffff, 0))
hci_cmd(s, 0x0c1c, struct.pack('<HH', 0x0012, 0x0012)); hci_cmd(s, 0x0c1a, bytes([2]))
base = int(sys.argv[2], 0); n = int(sys.argv[3], 0)
hist = {}; t0 = time.time(); end = t0 + float(sys.argv[1]); prev = None; changes = {}
while time.time() < end:
    st, d = peek(base, n)
    w = struct.unpack('<%dI' % n, d)
    t = time.time() - t0
    for i, x in enumerate(w):
        hist.setdefault(i, set()).add(x)
        if prev and prev[i] != x:
            changes.setdefault(i, []).append((round(t, 3), x))
    prev = w
hci_cmd(s, 0x0c1a, bytes([0]))
for i in sorted(changes):
    c = changes[i]
    print('%08x distinct %4d changes %4d first: %s' % (base + 4 * i, len(hist[i]), len(c), ' '.join('%s=%08x' % (a, b) for a, b in c[:8])))
