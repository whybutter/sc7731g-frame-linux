#!/usr/bin/env python3
"""Build the WCN firmware's Wi-Fi calibration block, offline.

  wcn-nvm.py CONFIGURE.ini CALIBRATION.ini OUT.bin

cp2 is handed a 256-byte `struct wifi_nvm_data` inside WIFI_CMD_DEV_OPEN.
The vendor's sprdwl builds it in the kernel at every open: sprdwl_nvm_init()
parses /system/etc/connectivity_configure.ini, then
connectivity_calibration.ini (/productinfo's copy, which on this frame
doesn't exist and gets created from /system's), into a static struct, and
sets data_init_ok = 1. Both files are in the backup's system.img (/etc).

This replays that parser rather than reading the ini sensibly, because its
quirks are part of the result:
- every token on a line after the name is a number, simple_strtol(base 0),
  letters and all ("pan.yu" -> 0, "7731Gv1.0.4" -> 7731);
- `version = 11 20150928/pan.yu ...` therefore writes ~20 bytes from offset
  0 on, over fields that later lines overwrite again;
- a name starting with a digit ("11b_tx_power_compensation") never matches
  the table's "_11b_...", so those lines are dropped (they are all zeros).

The offsets are the frame's own: g_nvm_table read out of its sprdwl.ko
(.data + 0x11bc, 60 entries) matches the SM-G531H source's
wifi_nvm_data.h field for field, and its sprdwl_mac_open_cmd copies
exactly 0x100 bytes of it into a 279-byte command.
"""
import struct
import sys

# name -> (offset, element size), from the frame's g_nvm_table
TABLE = {
    "version": (0x00, 1), "tpc_enable": (0x02, 1),
    "ref_clk": (0x03, 1), "FEM_status": (0x04, 1), "wifi_tx": (0x05, 1),
    "bt_tx": (0x06, 1), "wifi_rx": (0x07, 1), "bt_rx": (0x08, 1),
    "wb_lna_bypass": (0x09, 1), "gain_LNA": (0x0a, 1),
    "IL_wb_lna_bypass": (0x0b, 1), "Rx_adaptive": (0x0c, 1),
    "up_bypass_switching_point0": (0x0d, 1),
    "low_bypass_switching_point0": (0x0e, 1),
    "data_rate_power": (0x0f, 1), "channel_num": (0x10, 1),
    "channel_range": (0x11, 1),
    "_11b_tx_power_dr0": (0x17, 1), "_11b_tx_power_dr1": (0x1a, 1),
    "_11g_tx_power_dr0": (0x1d, 1), "_11g_tx_power_dr1": (0x20, 1),
    "_11g_tx_power_dr2": (0x23, 1), "_11g_tx_power_dr3": (0x26, 1),
    "_11n_tx_power_dr0": (0x29, 1), "_11n_tx_power_dr1": (0x2c, 1),
    "_11n_tx_power_dr2": (0x2f, 1), "_11n_tx_power_dr3": (0x32, 1),
    "adaptive_tx_power": (0x35, 1), "adaptive_tx_index_offset0": (0x36, 1),
    "adaptive_rx_RSSI0": (0x37, 1), "adaptive_tx_index_offset1": (0x38, 1),
    "adaptive_rx_RSSI1": (0x39, 1),
    "phy_init_reg_num": (0x3c, 1), "init_phy_regs": (0x3e, 2),
    "RF_init_reg_num": (0x52, 1), "init_rf_regs": (0x54, 4),
    "extreme_Tx_config": (0x7c, 1), "temp_index": (0x7d, 1),
    "_11b_tx_power_compensation": (0x88, 1),
    "_11gn_tx_power_compensation": (0x93, 1),
    "extreme_Rx_config": (0x9e, 1), "Rx_RSSI_compensation": (0x9f, 1),
    "Rx_temp_switching_point": (0xaa, 1), "phy_reg_num": (0xac, 1),
    "phy_regs_1": (0xae, 2), "phy_regs_2": (0xc2, 2),
    "e_cali_channel": (0xd6, 1), "e_calibration_flag": (0xd9, 1),
    "e_channel_num": (0xda, 1), "e_channel_range": (0xdb, 1),
    "e_11b_tx_power_dr0": (0xe1, 1), "e_11b_tx_power_dr1": (0xe4, 1),
    "e_11g_tx_power_dr0": (0xe7, 1), "e_11g_tx_power_dr1": (0xea, 1),
    "e_11g_tx_power_dr2": (0xed, 1), "e_11g_tx_power_dr3": (0xf0, 1),
    "e_11n_tx_power_dr0": (0xf3, 1), "e_11n_tx_power_dr1": (0xf6, 1),
    "e_11n_tx_power_dr2": (0xf9, 1), "e_11n_tx_power_dr3": (0xfc, 1),
}
SIZE = 0x100
DATA_INIT_OK = 0x01


def ctype(b):
    """find_type(): 1 name-ish, 2 digit/minus, 3 x/X/., 4 end, 0 other."""
    c = chr(b) if b < 0x80 else "\x80"  # the vendor's char is signed
    if c == "_" or (c.isascii() and c.isalpha() and c not in "xX"):
        return 1
    if c.isdigit() or c == "-":
        return 2
    if c in "xX.":
        return 3
    if c in "\0\r\n#":
        return 4
    return 0


def strtol(s):
    """simple_strtol(s, NULL, 0): sign, base from prefix, stop at junk."""
    neg = s.startswith("-")
    if neg:
        s = s[1:]
    base = 10
    if s[:2].lower() == "0x":
        base, s = 16, s[2:]
    elif s.startswith("0") and len(s) > 1:
        base, s = 8, s[1:]
    v = 0
    for ch in s:
        d = int(ch, 36) if ch.isalnum() else 99
        if d >= base:
            break
        v = v * base + d
    return -v if neg else v


def parse_line(line):
    """get_cmd_par(): the first name-ish token is the name, the rest are
    numbers -- name-ish ones included."""
    name, pars, tok, btype = None, [], b"", -1
    for b in line + b"\0":
        t = ctype(b)
        if t in (1, 2, 3):
            tok += bytes([b])
            if btype == -1:
                btype = 2 if t == 2 else 1
            elif btype == 2 and t == 1:
                btype = 1
            continue
        if btype != -1:
            s = tok.decode()
            if btype == 1 and name is None:
                name = s
            else:
                pars.append(strtol(s))
            btype, tok = -1, b""
        if t == 4:
            break
    return name, pars


def apply(buf, text):
    p = 0
    for i, b in enumerate(text + b"\0"):
        if b in b"\n\r\0":
            if i - p >= 5:
                name, pars = parse_line(text[p:i])
                if name in TABLE:
                    off, sz = TABLE[name]
                    fmt = {1: "<B", 2: "<H", 4: "<I"}[sz]
                    for k, v in enumerate(pars):
                        o = off + k * sz
                        # past the end the vendor wrote over whatever
                        # followed its static struct; nothing does here
                        if o + sz <= SIZE:
                            struct.pack_into(fmt, buf, o, v & (1 << 8 * sz) - 1)
            p = i + 1


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__.split("\n\n")[1])
    buf = bytearray(SIZE)
    for path in sys.argv[1:3]:
        apply(buf, open(path, "rb").read())
    buf[DATA_INIT_OK] = 1
    open(sys.argv[3], "wb").write(buf)
    print(f"{sys.argv[3]}: {SIZE} bytes")
    for i in range(0, SIZE, 16):
        print(f"  {i:02x}: " + " ".join(f"{b:02x}" for b in buf[i:i + 16]))


if __name__ == "__main__":
    main()
