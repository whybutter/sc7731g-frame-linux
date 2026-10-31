#!/usr/bin/env python3
"""Build the WCN firmware's Bluetooth PSKEY block, offline.

  bt-pskey.py CONFIGURE.ini OUT.bin [BDADDR | --serial SERIALNO]

Before the first HCI command, the frame's Android hands cp2's Bluetooth a
144-byte `sr2351_pskey` struct: libbt-vendor.so (system.img
/vendor/lib/libbt-vendor.so) parses section 3 and 4 of
/system/etc/connectivity_configure.ini into it in vnd_load_conf(), and
hw_pskey_vnd_preload() (its adapter module's +0xc, at 0x3904) writes the 144
bytes as they are to /dev/sttybt0 -- no H4 packet type in front of them, the
struct's own first byte, pskey_cmd = 5, is what tells cp2 it isn't HCI --
then select()s up to 5 s for an answer and reads up to 10 bytes. An answer
whose first byte is 5 is success; anything else is "received unknow
response". /dev/sttybt0 is the vendor's stty4bt: sbuf channel 4 ring 10
(vendor DT stty4bt_cpwcn, bufid 0x0a), raw bytes both ways.

This replays that parser rather than reading the ini sensibly, because its
quirks are part of the result (all read from the disassembly):
- a line starting with '#' is skipped; the name is the first token of
  " =\\n\\r\\t", the value text the next token of "=\\n\\r\\t#", and the
  values are the tokens of that text split by " ,=\\n\\r\\t#";
- each value is strtoul() base 16 when it starts "0x"/"0X", base 10
  otherwise (so a leading 0 is not octal), and is stored truncated to the
  field's element size, little-endian;
- only the table's element count is read (the "# 0xd0418CFE,..." comments
  after g_aPowerValue never get parsed), and a line with fewer values leaves
  the rest as they were;
- names not in the table (every Wi-Fi line) are ignored, and the file is read
  top to bottom, so the last line of a name wins.

Then, whatever the ini said, device_addr (offset 0x50) is overwritten with
/data/misc/bluedroid/btmac.txt ("%02X:%02X:..."), byte-reversed (BD_ADDR is
little-endian on the wire), or with zeros if that file doesn't exist. The
frame has no factory address (productinfo holds no btmac.txt), so we pick
one: BDADDR as given, or one derived from the SoC serial
(androidboot.serialno) with --serial, stable per device. The persist.sys.bt.
non.ssp property that clears feature_set[6] bit 3 isn't set on the frame and
isn't replayed.

The table is the frame's own (sr2351_table at .data.rel.ro 0x8a8c, 38 entries
of {name, handler, offset, count, size}; sizeof(sr2351_pskey) = 0x90).
"""
import hashlib
import struct
import sys

# name -> (offset, count, element size), from the frame's sr2351_table
TABLE = {
    "pskey_cmd": (0, 1, 1),
    "g_dbg_source_sink_syn_test_data": (1, 1, 1),
    "g_sys_sleep_in_standby_supported": (2, 1, 1),
    "g_sys_sleep_master_supported": (3, 1, 1),
    "g_sys_sleep_slave_supported": (4, 1, 1),
    "default_ahb_clk": (8, 1, 4),
    "device_class": (12, 1, 4),
    "win_ext": (16, 1, 4),
    "g_aGainValue": (20, 6, 4),
    "g_aPowerValue": (44, 5, 4),
    "feature_set": (64, 16, 1),
    "device_addr": (80, 6, 1),
    "g_sys_sco_transmit_mode": (86, 1, 1),
    "g_sys_uart0_communication_supported": (87, 1, 1),
    "edr_tx_edr_delay": (88, 1, 1),
    "edr_rx_edr_delay": (89, 1, 1),
    "g_PrintLevel": (92, 1, 4),
    "uart_rx_watermark": (96, 1, 2),
    "uart_flow_control_thld": (98, 1, 2),
    "comp_id": (100, 1, 4),
    "pcm_clk_divd": (104, 1, 2),
    "half_word_reserved": (106, 1, 2),
    "pcm_config": (108, 1, 4),
    "ref_clk": (112, 1, 1),
    "FEM_status": (113, 1, 1),
    "gpio_cfg": (114, 1, 1),
    "gpio_PA_en": (115, 1, 1),
    "wifi_tx": (116, 1, 1),
    "bt_tx": (117, 1, 1),
    "wifi_rx": (118, 1, 1),
    "bt_rx": (119, 1, 1),
    "wb_lna_bypass": (120, 1, 1),
    "gain_LNA": (121, 1, 1),
    "IL_wb_lna_bypass": (122, 1, 1),
    "Rx_adaptive": (123, 1, 1),
    "up_bypass_switching_point0": (124, 1, 2),
    "low_bypass_switching_point0": (126, 1, 2),
    "bt_reserved": (128, 4, 4),
}
SIZE = 0x90
FMT = {1: "<B", 2: "<H", 4: "<I"}


def tokens(s, delims):
    """strtok: runs of delimiters separate, empty tokens never come out"""
    out, cur = [], ""
    for c in s:
        if c in delims:
            if cur:
                out.append(cur)
            cur = ""
        else:
            cur += c
    if cur:
        out.append(cur)
    return out


def strtoul(s, base):
    """C strtoul: the longest valid prefix, 0 if there is none, wrapped to
    32 bits (bionic saturates at ULONG_MAX; nothing in the file is that big)"""
    if base == 16 and s[:2].lower() == "0x":
        s = s[2:]
    v = 0
    for c in s:
        d = int(c, 36) if c.isalnum() else 99
        if d >= base:
            break
        v = v * base + d
    return min(v, 0xFFFFFFFF)


def build(ini, bdaddr):
    blob = bytearray(SIZE)
    for raw in ini.splitlines():
        line = raw.decode("latin-1")[:255]      # fgets into 0x100 bytes
        if line.startswith("#"):
            continue
        first = tokens(line, " =\n\r\t")
        if not first:
            continue
        name = first[0]
        rest = line[line.index(name) + len(name):]
        # strtok_r's second call starts after the delimiter that ended the
        # name, and its own delimiters lack ' ': " = 1, 2" -> " 1, 2"
        rest = rest[1:] if rest else rest
        value = tokens(rest, "=\n\r\t#")
        if not value:
            continue                            # "missing value for name"
        if name not in TABLE:
            continue
        off, count, size = TABLE[name]
        for tok in tokens(value[0], " ,=\n\r\t#")[:count]:
            base = 16 if tok[:2].lower() == "0x" else 10
            v = strtoul(tok, base) & ((1 << (8 * size)) - 1)
            struct.pack_into(FMT[size], blob, off, v)
            off += size
    blob[80:86] = bytes(reversed(bdaddr))
    return bytes(blob)


def addr_from_serial(serial):
    """a stable address per device: a hash of the SoC serial. The two top
    bits clear keep it out of the range LE reserves for random addresses,
    for whatever reads it as one"""
    h = hashlib.sha256(b"vt10101-bt:" + serial.encode()).digest()
    return bytes([h[0] & 0x3F]) + h[1:6]


def main():
    if len(sys.argv) not in (3, 4, 5):
        sys.exit(__doc__.split("\n\n")[1])
    ini = open(sys.argv[1], "rb").read()
    if len(sys.argv) == 5 and sys.argv[3] == "--serial":
        addr = addr_from_serial(sys.argv[4])
    elif len(sys.argv) == 4:
        addr = bytes(int(x, 16) for x in sys.argv[3].split(":"))
        assert len(addr) == 6, "BDADDR is six hex bytes, aa:bb:cc:dd:ee:ff"
    else:
        addr = bytes(6)                         # what a missing btmac.txt gives
    blob = build(ini, addr)
    open(sys.argv[2], "wb").write(blob)
    print("bt-pskey: %d bytes, pskey_cmd=%d, bdaddr %s" % (
        len(blob), blob[0], ":".join("%02x" % b for b in addr)))


if __name__ == "__main__":
    main()
