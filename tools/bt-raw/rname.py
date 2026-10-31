# HCI Remote_Name_Request over a raw HCI socket: pages ADDR, reads its name
import socket, struct, sys, time
addr = bytes(int(x, 16) for x in reversed(sys.argv[1].split(":")))
s = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_RAW, socket.BTPROTO_HCI)
s.bind((0,))
# filter: all event types, all events
s.setsockopt(0, 2, struct.pack("<IIIHxx", 0x10, 0xffffffff, 0xffffffff, 0))
s.send(bytes([1, 0x19, 0x04, 10]) + addr + bytes([1, 0, 0, 0]))
s.settimeout(1)
end = time.time() + 20
while time.time() < end:
    try:
        p = s.recv(300)
    except socket.timeout:
        continue
    if p[0] != 4:
        continue
    code = p[1]
    if code == 0x0f and p[5:7] == b"\x19\x04":
        print("command status", p[3])
    elif code == 0x07:
        print("remote name: status", p[3], "addr", ":".join("%02x" % b for b in reversed(p[4:10])),
              "name", p[10:].split(b"\0")[0].decode(errors="replace"))
        break
