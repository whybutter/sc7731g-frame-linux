#!/usr/bin/env python3
"""page.py [BDADDR] -- HCI Create_Connection to BDADDR (default a made-up
00:11:22:33:44:55) on hci0: prints the Connection Complete status and how
long it took. Nobody there answers, so a controller with airtime reports
Page Timeout (0x04) after the page timeout (5.12 s default); a starved one
reports nothing. Cancels a stuck page so the next test starts clean."""
import socket, struct, sys, time

a = sys.argv[1] if len(sys.argv) > 1 else '00:11:22:33:44:55'
bd = bytes(int(x, 16) for x in reversed(a.split(':')))
s = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_RAW, socket.BTPROTO_HCI)
s.bind((0,))
s.setsockopt(0, 2, struct.pack('<IIIHxx', 0x10, 0xffffffff, 0xffffffff, 0))
s.settimeout(1)
# DM1..DH5 packet types, R2 page scan mode, no clock offset, no role switch
par = bd + struct.pack('<HBBHB', 0xcc18, 1, 0, 0, 0)
s.send(bytes([1, 0x05, 0x04, len(par)]) + par)
t0 = time.time()
while time.time() - t0 < 25:
    try:
        p = s.recv(300)
    except socket.timeout:
        continue
    if p[1] == 0x0f and p[3] != 0:
        print('page FAIL: command status %#x' % p[3]); sys.exit(1)
    if p[1] == 0x03:
        print('page done: Connection Complete status %#x after %.1f s' % (p[3], time.time() - t0))
        sys.exit(0)
s.send(bytes([1, 0x08, 0x04, 6]) + bd)       # Create_Connection_Cancel
print('page FAIL: no Connection Complete in 25 s; cancelled')
sys.exit(1)
