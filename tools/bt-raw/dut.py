# dut.py SCEN HOP TXF RXF PTYPE LEN -- local DUT test link via vendor 0xFC03 (magic 0x00010002)
import struct, sys, socket
sys.path.insert(0, '/tmp/fbt')
from hcipeek import hci_cmd
a = [int(x, 0) for x in sys.argv[1:7]]
par = bytes([0, 0, a[0], a[1], a[2], a[3], 0, 0, a[4]]) + struct.pack('<H', a[5]) + struct.pack('<I', 0x00010002)
s = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_RAW, socket.BTPROTO_HCI)
s.bind((0,)); s.setsockopt(0, 2, struct.pack('<IIIHxx', 0x10, 0xffffffff, 0xffffffff, 0))
print('fc03 status', hci_cmd(s, 0xfc03, par))
