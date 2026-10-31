# scanwatch.py SECS -- continuous page scan (window = interval), then sample
# BB regs 0x40086000..0x4008607f as fast as the peek primitive allows
import struct, sys, time, socket
sys.path.insert(0, '/tmp/fbt')
from hcipeek import peek, hci_cmd
s = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_RAW, socket.BTPROTO_HCI)
s.bind((0,)); s.setsockopt(0, 2, struct.pack('<IIIHxx', 0x10, 0xffffffff, 0xffffffff, 0))
print('psa', hci_cmd(s, 0x0c1c, struct.pack('<HH', 0x0012, 0x0012)))
print('scan', hci_cmd(s, 0x0c1a, bytes([2])))
end = time.time() + float(sys.argv[1])
t0 = time.time()
while time.time() < end:
    st, d = peek(0x40086000, 32)
    w = struct.unpack('<32I', d)
    print('%6.3f clk %08x sync %08x %08x 20 %08x 24 %08x 30 %08x 38 %08x' % (time.time() - t0, w[6], w[2], w[3], w[8], w[9], w[12], w[14]))
print('scan off', hci_cmd(s, 0x0c1a, bytes([0])))
