# hitprobe.py BDADDR SECS -- page BDADDR (HCI Remote_Name_Request) while sampling
# BB 0x40086000..0x4008607f; print value histograms of the words that move
import struct, sys, time, socket, threading, collections
sys.path.insert(0, '/tmp/fbt')
from hcipeek import peek
addr = bytes(int(x, 16) for x in reversed(sys.argv[1].split(':')))
s = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_RAW, socket.BTPROTO_HCI)
s.bind((0,))
s.send(bytes([1, 0x19, 0x04, 10]) + addr + bytes([1, 0, 0, 0]))
h = collections.defaultdict(collections.Counter)
end = time.time() + float(sys.argv[2])
while time.time() < end:
    st, d = peek(0x40086000, 32)
    for i, x in enumerate(struct.unpack('<32I', d)):
        h[i][x] += 1
for i in sorted(h):
    if len(h[i]) > 1 and i not in (6, 7):
        print('%08x' % (0x40086000 + 4 * i), ' '.join('%08x:%d' % kv for kv in h[i].most_common(6)))
