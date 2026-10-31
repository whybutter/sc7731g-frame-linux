#!/usr/bin/env python3
"""fastboot-mini.py getvar VAR | flash PARTITION IMAGE | reboot

A minimal fastboot client for the vendor U-Boot's fastboot gadget (18d1:0fff,
"fastboot Gadget", interface ff/42/03, bulk IN 0x85 / OUT 0x06).

Why: on 2026-10-27, with the boot partition half-written by a hung write from
our Linux, `cboot fastboot` at the U-Boot prompt brought the gadget up, but
the Mac's platform-tools fastboot never listed it (the gadget STALLs a vendor
control request during enumeration, which that client seems to choke on).
The protocol is just ASCII commands on the bulk pipe: "getvar:x",
"download:%08x" + the data, "flash:<part>", "reboot", answered with
OKAY/FAIL/DATA/INFO packets.
"""
import sys
import usb.backend.libusb1 as lb
import usb.core

be = lb.get_backend(find_library=lambda _: "/opt/homebrew/lib/libusb-1.0.dylib")
dev = usb.core.find(idVendor=0x18D1, idProduct=0x0FFF, backend=be)
if dev is None:
    sys.exit("no fastboot gadget (18d1:0fff) -- `cboot fastboot` at the U-Boot prompt")
# the gadget comes up unconfigured (the STALL above); a USB reset, then
# set_configuration, gets it going (seen on the frame 2026-10-27)
for attempt in range(2):
    try:
        dev.set_configuration(1)
        break
    except usb.core.USBError:
        dev.reset()
EP_IN, EP_OUT = 0x85, 0x06


def cmd(c, timeout=10000):
    dev.write(EP_OUT, c.encode(), timeout=timeout)
    while True:
        r = bytes(dev.read(EP_IN, 512, timeout=timeout))
        kind, body = r[:4], r[4:].decode(errors="replace")
        if kind == b"INFO":
            print("INFO", body)
            continue
        return kind, body


a = sys.argv[1:]
if not a:
    sys.exit(__doc__)
if a[0] == "getvar":
    print(cmd("getvar:" + a[1]))
elif a[0] == "flash":
    part, data = a[1], open(a[2], "rb").read()
    kind, body = cmd("download:%08x" % len(data))
    if kind != b"DATA":
        sys.exit(f"download refused: {kind} {body}")
    for off in range(0, len(data), 1 << 20):
        dev.write(EP_OUT, data[off:off + (1 << 20)], timeout=60000)
    print("download", dev.read(EP_IN, 512, timeout=60000).tobytes())
    print("flash", cmd("flash:" + part, timeout=120000))
elif a[0] == "reboot":
    print(cmd("reboot"))
else:
    sys.exit(__doc__)
