# poke.py DST WORD... -- write words into cp2's view of memory: stage them in
# smem (MB+0x3000 over /dev/mem), then have cp2 copy them with patched 0xFC2D
import mmap, os, socket, struct, sys
sys.path.insert(0, '/tmp/fbt')
from hcipeek import hci_cmd, MB
STAGE = MB + 0x3000


def poke(dst, words):
    fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
    m = mmap.mmap(fd, 0x1000, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=STAGE)
    m[0:4 * len(words)] = struct.pack('<%dI' % len(words), *words)
    m.close(); os.close(fd)
    s = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_RAW, socket.BTPROTO_HCI)
    s.bind((0,)); s.setsockopt(0, 2, struct.pack('<IIIHxx', 0x10, 0xffffffff, 0xffffffff, 0))
    st = hci_cmd(s, 0xfc2d, bytes([0]) + struct.pack('<III', STAGE, len(words), dst))
    s.close()
    return st


if __name__ == '__main__':
    print('status', poke(int(sys.argv[1], 0), [int(x, 0) for x in sys.argv[2:]]))
