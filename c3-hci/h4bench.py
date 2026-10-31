#!/usr/bin/env python3
"""h4bench.py PORT [SCAN_SECS] [WATCH_ADDR] -- bench test of the c3-hci firmware from a computer.

Wiring (a USB-serial adapter, 3.3 V logic): adapter TX -> C3 GPIO6, adapter RX
<- C3 GPIO5, GND - GND; the C3 powered from its USB (that also gives its log on
USB-Serial-JTAG). The adapter must really do 1,000,000 baud: FT232R/FT231X and
CP2102N do it exactly; a CH340 does 1 Mbaud too; an old CP2102 (not N) rounds to
921600 and will fail the replies -- this script says so.

Steps, each one must pass before the next:
1. silence: press the C3's RESET (or replug it) within the first 5 s; nothing
   may arrive for 6 s -- the frame's U-Boot stops autoboot on any byte;
2. noise: a U-Boot-like banner sent at 115200 (what the C3 hears while the
   frame boots); still nothing may come back;
3. HCI: Reset (the firmware's start signal, answered by the controller
   itself), Read_Local_Version, Read_BD_ADDR;
4. LE scan for SCAN_SECS (default 15): every advertiser once, and
   WATCH_ADDR (a BLE device of yours, aa:bb:cc:dd:ee:ff form) flagged when seen.

Needs pyserial.
"""
import struct
import sys
import time

import serial

PORT = sys.argv[1]
SCAN_SECS = float(sys.argv[2]) if len(sys.argv) > 2 else 15
SCALE = sys.argv[3].upper() if len(sys.argv) > 3 else None


def die(msg):
    print('FAIL:', msg)
    sys.exit(1)


s = serial.Serial(PORT, 1000000, timeout=0.05, rtscts=False, dsrdtr=False)
s.reset_input_buffer()

# 1. silence across a reset
print('1. silence: reset the C3 now (RESET button or replug); listening 6 s ...')
end = time.time() + 6
got = b''
while time.time() < end:
    got += s.read(256)
if got:
    die('the C3 sent %d bytes unprompted: %s' % (len(got), got[:32].hex()))
print('   ok: nothing on the wire')

# 2. U-Boot-like noise at the wrong baud
banner = (b'\r\nU-Boot 2010.12-00001 (Jun 16 2025)\r\nDRAM:  1 GiB\r\n'
          b'MMC:   sdio1: 0\r\nHit any key to stop autoboot:  0 \r\n') * 4
s.baudrate = 115200
s.write(banner)
s.flush()
time.sleep(0.2)
s.baudrate = 1000000
s.reset_input_buffer()
end = time.time() + 3
got = b''
while time.time() < end:
    got += s.read(256)
if got:
    die('after the 115200 noise the C3 sent %d bytes: %s' % (len(got), got[:32].hex()))
print('2. noise: ok, %d bytes of 115200 banner dropped, nothing came back' % len(banner))

# 3. HCI
buf = b''


def packet(timeout=2.0):
    """one H4 packet (event or ACL) or None"""
    global buf
    end = time.time() + timeout
    while time.time() < end:
        buf += s.read(512)
        if len(buf) >= 3 and buf[0] == 4 and len(buf) >= 3 + buf[2]:
            n = 3 + buf[2]
            p, buf = buf[:n], buf[n:]
            return p
        if len(buf) >= 5 and buf[0] == 2:
            n = 5 + (buf[3] | buf[4] << 8)
            if len(buf) >= n:
                p, buf = buf[:n], buf[n:]
                return p
        if buf and buf[0] not in (2, 4):
            die('not H4: %s (wrong baud? the adapter must do exactly 1 Mbaud)' % buf[:16].hex())
    return None


def cmd(op, par=b'', timeout=3.0):
    s.write(bytes([1, op & 0xff, op >> 8, len(par)]) + par)
    end = time.time() + timeout
    while time.time() < end:
        p = packet(end - time.time())
        if p is None:
            break
        if p[1] == 0x0e and struct.unpack_from('<H', p, 4)[0] == op:
            return p[6], p[7:]
        if p[1] == 0x0f and struct.unpack_from('<H', p, 5)[0] == op:
            return p[3], b''
    die('no answer to opcode %#06x' % op)


t0 = time.time()
st, _ = cmd(0x0c03, timeout=5)   # the firmware starts the controller on this
print('3. HCI_Reset: status %d after %.0f ms (controller start included)' % (st, (time.time() - t0) * 1000))
st, r = cmd(0x1001)
hv, hrev, lmp, manuf, sub = struct.unpack_from('<BHBHH', r)
print('   Read_Local_Version: HCI %d rev %#x LMP %d manufacturer %#06x subver %#x' % (hv, hrev, lmp, manuf, sub))
st, r = cmd(0x1009)
print('   Read_BD_ADDR: %s' % ':'.join('%02X' % b for b in reversed(r[:6])))
st, _ = cmd(0x0c01, bytes([0xff] * 5 + [0x1f, 0x00, 0x20]))      # event mask + LE meta
st, _ = cmd(0x2001, bytes([0x1f, 0, 0, 0, 0, 0, 0, 0]))         # LE event mask
st, _ = cmd(0x200b, bytes([1]) + struct.pack('<HH', 0x30, 0x30) + bytes([0, 0]))
if st:
    die('LE_Set_Scan_Parameters status %d' % st)
st, _ = cmd(0x200c, bytes([1, 0]))
if st:
    die('LE_Set_Scan_Enable status %d' % st)

# 4. scan
print('4. LE scan %.0f s:' % SCAN_SECS)
seen = {}
end = time.time() + SCAN_SECS
while time.time() < end:
    p = packet(0.5)
    if not p or p[0] != 4 or p[1] != 0x3e or p[3] != 0x02:
        continue
    d = p[4:]
    pos = 1
    for _ in range(d[0]):
        evt, at = d[pos], d[pos + 1]
        a = ':'.join('%02X' % b for b in reversed(d[pos + 2:pos + 8]))
        ln = d[pos + 8]
        data = d[pos + 9:pos + 9 + ln]
        rssi = struct.unpack_from('b', d, pos + 9 + ln)[0]
        pos += 10 + ln
        if a not in seen:
            seen[a] = 0
            print('   %s %s evt=%d rssi=%d %s%s' % (a, 'random' if at else 'public', evt, rssi,
                                                  data.hex(), '   <-- watched' if a == SCALE else ''))
        seen[a] += 1
cmd(0x200c, bytes([0, 0]))
print('   %d advertisers' % len(seen) + ('' if not SCALE else '; %s %s' % (SCALE, ('seen %d times' % seen[SCALE]) if SCALE in seen else 'NOT seen (awake?)')))
print('PASS' if not SCALE or SCALE in seen else 'PASS (HCI); %s not seen' % SCALE)
