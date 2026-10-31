#!/usr/bin/env python3
"""hciping.py [DEV] -- is hciN (default 0) up and answering? One
Read_BD_ADDR over a raw HCI socket, 2 s for its Command Complete. Prints
`hci0 ok BD_ADDR ms` and exits 0, or `hci0 FAIL why` and exits 1.
For bt-cycle.sh on the frame (busybox, python3, no bluez tools needed)."""
import socket, struct, sys, time

dev = int(sys.argv[1]) if len(sys.argv) > 1 else 0
try:
    s = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_RAW, socket.BTPROTO_HCI)
    s.bind((dev,))
    # HCI_FILTER: events only, all of them
    s.setsockopt(0, 2, struct.pack('<IIIHxx', 0x10, 0xffffffff, 0xffffffff, 0))
    s.settimeout(2)
    t0 = time.time()
    s.send(bytes([1, 0x09, 0x10, 0]))
    while True:
        p = s.recv(300)
        if p[0] == 4 and p[1] == 0x0e and p[4:6] == b'\x09\x10':
            if p[6]:
                raise RuntimeError('status %d' % p[6])
            a = ':'.join('%02X' % b for b in reversed(p[7:13]))
            print('hci%d ok %s %.0f ms' % (dev, a, (time.time() - t0) * 1000))
            sys.exit(0)
except SystemExit:
    raise
except Exception as e:
    print('hci%d FAIL %s' % (dev, e))
    sys.exit(1)
