#!/usr/bin/env python3
"""Print the panel's init sequence from the VT10101's Android kernel.

The vendor's sprdfb panel driver for the JD9365 (lcd_id ID936500) keeps it as
LCM_Init_Code entries: u32 tag (1<<24 | len = DCS/generic write of len bytes,
2<<24 | ms = sleep) followed by a fixed 56-byte data field, 60 bytes a step.
The table is found by its first entries, the JD9365's page-0 select and
password (E0 00, E1 93, E2 65, E3 F8); it ends at the first tag that isn't a
send or a sleep. Output is one command per line, for a mainline panel driver
(panel-jadard-jd9365da-h3 takes this shape of sequence).

  extract-lcm-init.py BOOT_KERNEL
"""
import re, struct, sys

d = open(sys.argv[1], 'rb').read()
STRIDE, MAXLEN = 60, 56
m = re.search(rb'\x02\x00\x00\x01\xe0\x00' + b'.' * (STRIDE - 6) +
              rb'\x02\x00\x00\x01\xe1\x93', d, re.S)
if not m:
    sys.exit('no JD9365 init table found')
i = m.start()
while True:
    tag = struct.unpack_from('<I', d, i)[0]
    kind, n = tag >> 24, tag & 0xffffff
    if kind == 1 and 0 < n <= MAXLEN:
        print('send ' + d[i + 4:i + 4 + n].hex(' '))
    elif kind == 2 and n < 5000:
        print(f'sleep {n}')
    else:
        break
    i += STRIDE
