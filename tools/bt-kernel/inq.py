#!/usr/bin/env python3
"""inq.py [SECS_UNITS] -- one HCI Inquiry (GIAC, length N x 1.28 s, default
4) on hci0 over a raw socket: prints each result and whether Inquiry
Complete came within length + 15 s. Cancels a stuck one so the next test
starts clean. Exit 0 when it completed."""
import socket, struct, sys, time

n = int(sys.argv[1]) if len(sys.argv) > 1 else 4
s = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_RAW, socket.BTPROTO_HCI)
s.bind((0,))
s.setsockopt(0, 2, struct.pack('<IIIHxx', 0x10, 0xffffffff, 0xffffffff, 0))
s.settimeout(1)
s.send(bytes([1, 0x01, 0x04, 5, 0x33, 0x8b, 0x9e, n, 0]))
t0 = time.time()
found = 0
while time.time() - t0 < n * 1.28 + 15:
    try:
        p = s.recv(300)
    except socket.timeout:
        continue
    if p[1] == 0x0f and p[3] != 0:
        print('inquiry FAIL status %#x' % p[3]); sys.exit(1)
    if p[1] in (0x02, 0x22, 0x2f):
        found += 1
        print('  result %s' % ':'.join('%02X' % b for b in reversed(p[4:10])))
    if p[1] == 0x01:
        print('inquiry ok: complete after %.1f s, %d results, status %d' % (time.time() - t0, found, p[3]))
        sys.exit(0)
s.send(bytes([1, 0x02, 0x04, 0]))
print('inquiry FAIL: no Inquiry Complete in %.0f s (%d results); cancelled' % (time.time() - t0, found))
sys.exit(1)
