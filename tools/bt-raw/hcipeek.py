# hcipeek.py -- read cp2-side memory/registers through the patched 0xfc2d
# vendor command (wcnmodem-peek.bin: copy N words from SRC to DST), DST in
# the free top of the SIPC smem window, then read it back over /dev/mem.
#   hcipeek.py ADDR NWORDS [OUT.bin]      hex dump or raw file
import mmap, os, socket, struct, sys, time

MB = 0x879bc000            # smem + 0x7c000: above sbuf/sblock/log (docs/wifi.md)
MBSIZE = 0x4000


def hci_cmd(s, op, par):
    s.send(bytes([1, op & 0xff, op >> 8, len(par)]) + par)
    end = time.time() + 2
    s.settimeout(0.5)
    while time.time() < end:
        try:
            p = s.recv(300)
        except socket.timeout:
            continue
        if p[0] == 4 and p[1] == 0x0e and p[4:6] == struct.pack('<H', op):
            return p[6]
        if p[0] == 4 and p[1] == 0x0f and p[5:7] == struct.pack('<H', op):
            return p[3]
    print('no completion for', hex(op), 'events:', seen) if False else None
    raise TimeoutError(hex(op))


def peek(addr, n):
    assert n * 4 <= MBSIZE
    s = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_RAW, socket.BTPROTO_HCI)
    s.bind((0,))
    s.setsockopt(0, 2, struct.pack('<IIIHxx', 0x10, 0xffffffff, 0xffffffff, 0))
    fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
    m = mmap.mmap(fd, MBSIZE, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=MB)
    m[0:n * 4] = b'\xa5' * (n * 4)          # so a copy that never ran shows
    st = hci_cmd(s, 0xfc2d, bytes([0]) + struct.pack('<III', addr, n, MB))
    data = bytes(m[0:n * 4])
    m.close(); os.close(fd); s.close()
    return st, data


if __name__ == '__main__':
    a = int(sys.argv[1], 0); n = int(sys.argv[2], 0)
    st, data = peek(a, n)
    if len(sys.argv) > 3:
        open(sys.argv[3], 'wb').write(data)
        print('status', st, 'bytes', len(data))
    else:
        print('status', st)
        for i in range(0, len(data), 16):
            print('%08x: %s' % (a + i, ' '.join('%08x' % w for w in struct.unpack_from('<4I', data, i))))
