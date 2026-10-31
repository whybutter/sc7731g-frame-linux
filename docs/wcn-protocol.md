# The SC7731G's WCN: a protocol spec for the Wi-Fi driver

The design and protocol spec written before the native Wi-Fi driver
(`kernel/wcn-sc7731g.c`, `kernel/sprdwl-sc7731g.c`) existed: how Android
loads and talks to the Wi-Fi co-processor, what was proven on the hardware
from user space first, and how it maps onto mainline. It is kept because it
is the shortest description of the interfaces; `docs/wifi.md` is the log of
how the driver was actually brought up, and corrects this where they differ.

Sources: the frame's own Android kernel, DT and modules (`sprdwl.ko`,
`wcnd` and the two `.ini` files are in its system partition), and Samsung's
GPL kernel for the SM-G531H (Galaxy Grand Prime VE, also SC7731G, also
3.10.65) for the register and struct layouts. Section numbers below are this
file's own.

## 1. The hardware

- **There's no Wi-Fi chip on a bus.** The SC7731G has an integrated
  connectivity subsystem, **WCN**. It's an ARM core of its own (the
  vendor's "cp2") running Spreadtrum firmware, and it does the 802.11 MAC,
  BT and FM. The radio is an external **SR2351** RF transceiver, which cp2
  drives. The application processor (AP, where Linux runs) never touches the
  radio: it gives cp2 commands and Ethernet frames through shared DRAM.
- **Firmware:** the `wcnmodem` eMMC partition (p9, about 630 KiB used of
  1 MiB). Its version string, read from the running co-processor:
  `Platform Version: MOCORTM_14B_TShark_MPII_W16.19_Debug`,
  `Project Version: CP2_WCN_14D_W16.26.3_Release`,
  `HW Version: sc8830g_modem`, built `06-22-2016`. The image is linked to
  run at **0x8a808000**: its reset vector is the absolute address
  0x8a85aafc.
- **Memory**, all reserved `no-map` in our dts (`kernel/sc7731g.dtsi`,
  from the vendor DT):
  - **0x8a800000**: cp2's region, 2052 KiB. The image is at +0x8000, and
    the smsg rings are in its last page, at **0x8aa00000**.
  - **0x87800000–0x879fffff**: "smem", the shared pool for buffers. The
    Wi-Fi transport's window is **0x87940000**, 768 KiB.
- **Doorbells:** the AON IPI block at **0x402c0000**. The AP rings cp2 by
  writing 0x100 to 0x402c0000. cp2 rings the AP on **SPI 73**, cleared by
  writing 0x100 to 0x402c0004. The block's clock gate is AON_APB EB0 bit 21.
- **cp2 watchdog:** SPI 85.
- **RF power, partly unknown:** the vendor driver's probe calls an RF2351
  GPIO enable (if the board registered one) and a "vddwpa" enable.
  - In the Samsung tree, that vddwpa enable takes **vddsim2 to 2.8 V**. On
    this board vddsim2 is also the I/O rail for the touch controller's
    reset and interrupt pads, which our kernel runs at **1.8 V**
    (`kernel/regulator-sc2713s.c`, the dts). Find out what the frame's
    board code really does before changing that rail.
  - The RF2351 is configured over the MSPI block (AON_APB EB0 bit 23,
    `clk_mspi`). The vendor never turns that clock off. Our clock tree
    turns unclaimed clocks off at boot, so a driver must claim `clk_mspi`
    (or boot with `clk_ignore_unused` while testing).

## 2. Proven on the frame so far

These are user-space tests through `/dev/mem`: our kernel has `CONFIG_DEVMEM`
and no `STRICT_DEVMEM`, and the regions are `no-map`. Full logs are in
`docs/wifi.md`.

1. **The loader sequence works under our kernel.** We loaded p9 to
   0x8a808000 (every word read back right) and ran the start sequence (§3).
   cp2 executed: its RAM changed, it reset the smsg rings itself, and it
   rang the AP's doorbell (SPI 73 pending).
2. **smsg works both ways.** cp2 opened channels 4 and 5 on its own. We
   opened 4, cp2 sent `SBUF_INIT`, and we answered `DONE` with a layout
   address. cp2 consumed every message we sent.
3. **sbuf works.** The command `at+spatgetcp2info\r` on channel 4, ring 5,
   came back as 156 bytes of version text. **Case matters**: the uppercase
   form got no answer. Ring 0 accepted the string and said nothing; it's
   wcnd's loop check and wants some other command.
4. **Stopping is clean.** After stopping cp2 and restoring the gates and
   IRAM words we touched, a warm reboot came back by itself (wifi2).

Not yet attempted: sblock (channels 7 and 8), any `WIFI_CMD_*`, the RF side
(nothing has proven the radio receives anything), interrupts in place of
polling.

## 3. Loader spec (what Android's `sprd_cproc` + `wcnd` do)

**Load:** copy the p9 image to 0x8a808000. Nothing is passed in registers:
cp2 finds its message ring at a fixed address.

**Start:**
1. Write a 3-word trampoline into IRAM at 0x50003000: load the entry
   address into a register, then branch to it, where the entry is
   0x8a808000.
2. In PMU_APB (0x402b0000), clear `PD_CP2_SYS_CFG` (+0x68) bit 25, the
   force shutdown.
3. Set `CP_SOFT_RST` (+0xb0) bit 2.
4. Clear +0x68 bit 28, the force deep sleep.
5. Clear +0xb0 bit 2.
6. Turn on the IPI gate (EB0 bit 21) and the MSPI gate (bit 23) first.

**Observed after start:** `PD_CP2_SYS_CFG` 0x12080006 → 0x00080006, and
`CP_SOFT_RST` 7 → 3. Once cp2 runs, the IRAM window at 0x50003000 shows cp2's
own vector table, so the trampoline can't be read back there.

**Stop:** wait for `SLEEP_STATUS` (+0xd4) bits 12–15 to read 0, then
reverse the sequence. Restore the AON gates and the IRAM words you changed.

In mainline this is either a small platform driver or a **remoteproc**
driver with a raw-binary loader. remoteproc gives you firmware loading,
crash recovery on the SPI 85 watchdog and sysfs control for free. The
firmware should be shipped as a file (`/lib/firmware`, copied from p9 at
install time and never committed), not read from the partition by the
driver.

## 4. Transport spec: SIPC

Vendor reference: `drivers/sipc` (`sipc.h`, `smsg.c`, `smem.c`, `sbuf.c`,
`sblock.c`). It's generic kernel C with few API dependencies. The frame's
own kernel is a newer Spreadtrum tree with an extra `seblock.c`; check its
behaviour against the frame's binary where it matters.

- **smsg (control messages).**
  - Each message is 8 bytes: `{u8 channel, u8 type, u16 flag, u32 value}`.
  - Two rings of 128 entries at 0x8aa00000: the AP's tx at +0, its rx at
    +0x400, and free-running read/write pointers at +0x800.
  - **cp2 initialises the rings; the AP never writes them**, and Android's
    leftover pointers were reset by cp2 at start.
  - A channel is opened by an OPEN message (flag 0xBEEE) from each side.
    Types include OPEN, CLOSE, CMD, DONE and EVENT.
  - After writing, the sender rings the doorbell. Interrupt-driven receive
    is SPI 73; the tests polled instead.
- **smem.** A simple pool allocator over the 0x87940000 window. cp2 is
  always given physical addresses, so there's no IOMMU and no remapping.
- **sbuf (byte-stream rings), channel 4.**
  - 13 rings of 4 KiB in each direction. cp2 requests the layout with
    `CMD SBUF_INIT`, and the AP answers `DONE` with the smem address of a
    layout header it wrote. After that, data moves by advancing ring
    pointers plus an `EVENT` that names the ring.
  - Known rings: 0 is wcnd's loop check, 2 is assert, **5 is the AT
    console**, **7 is the Wi-Fi command channel**.
  - The exchange we saw on ring 5: we send `EVENT WRPTR`, cp2 answers with
    its own `EVENT WRPTR`, and we acknowledge with `EVENT RDPTR`.
- **sblock (fixed-size block pools).**
  - **Channel 7** is the data path: 64 tx and 128 rx blocks of 1664 bytes.
    Each block carries one Ethernet frame plus a small header.
  - **Channel 8** carries Wi-Fi events: scan results, connect results,
    disconnects.
  - The layout handshake is the same as sbuf (`CMD SBLOCK_INIT`, answered
    `DONE` with the pool's address). After it, blocks move between free and
    used rings by pointer updates and events.
  - Android's dmesg shows exactly this: "sbuf 3-4 ring 7", "Recv sblock8
    scan result event".
  - **Zero-copy:** the frame's firmware is the "zero-copy" build. The
    vendor driver ORs 0x80 into the mode it sends at open, and the firmware
    has the string "ap is not zero copy version, but cp2 is zero copy". The
    data path has to follow the zero-copy block convention: read the
    vendor's `sblock.c` and the frame's `sprdwl.ko` for exactly how the
    blocks are owned.

## 5. The Wi-Fi command layer (what sprdwl implements)

**sprdwl** is a **cfg80211 full-MAC** driver: no mac80211, no wext.
- **Commands:** 44 numbered `WIFI_CMD_*`. Each is a header
  (`wlan_sipc_data`: a message header with type and length, and a command
  tag with type, direction, level and id) followed by a packed struct.
  They're sent on sbuf channel 4, ring 7, and answered on the same ring.
  Asynchronous results come as sblock channel-8 events.
- **Source:** `drivers/net/wireless/sprdwl` in the vendor tree (`sipc.h`
  for the structs and ids, `cfg80211.c`, `wlan_*`).

Minimum set for a station:

| command / event | purpose | notes |
|---|---|---|
| `WIFI_CMD_DEV_OPEN` | bring the firmware's Wi-Fi up | mode (station, ORed with 0x80 for zero-copy), the interface MAC, a 256-byte **`wifi_nvm_data`** calibration block (§6) and an 8-byte timestamp; the frame's driver builds a **279-byte** command around it |
| `WIFI_CMD_DEV_CLOSE` | the reverse | |
| `WIFI_CMD_SCAN` (+ scan-done / result events on ch 8) | scanning | start with a wildcard SSID and an empty payload; results arrive as BSS records in channel-8 events, to report through `cfg80211_inform_bss_*` |
| `WIFI_CMD_CONNECT` / `DISCONNECT` (+ connect / disconnect events) | association | full-MAC: cp2 does auth and association itself |
| `WIFI_CMD_KEY_ADD` / `KEY_DEL` (names as in `sipc.h`) | keys | the host's wpa_supplicant does the WPA 4-way handshake over the data path (EAPOL frames go through sblock 7) and installs the resulting keys with these |
| `WIFI_CMD_GET_STATION` | RSSI and rate | for `iw` and NetworkManager |
| data on sblock ch 7 | tx/rx Ethernet frames | |

**Skip at first:** soft-AP, P2P, WAPI, the vendor PSK offload
(`WIFI_CMD_PSK`) and the "NPI" factory-test generic-netlink interface.
They're about half the vendor driver.

**MAC address:** there isn't one in productinfo. Android's `wcnd` makes up a
random one and keeps it in `/data/misc/wifi/wifimac.txt`. Generate a stable, locally administered one per device:
derive it from the SoC serial in the command line (`androidboot.serialno`),
or set it in the DT.

## 6. Calibration: `wifi_nvm_data` (verified, tool ready)

`WIFI_CMD_DEV_OPEN` carries a 256-byte calibration struct.
- **Source data:** the vendor driver parses it, in the kernel at every open,
  from `/system/etc/connectivity_configure.ini` and
  `connectivity_calibration.ini`. Both are in the backup's system.img.
- **`tools/wcn-nvm.py` builds the block offline**. It
  replays the vendor parser exactly, quirks included:
  - every token after the name is parsed as a number with base
    auto-detection, so text becomes 0 or a leading number;
  - the `version` line therefore writes about 20 bytes over fields that
    later lines overwrite again;
  - names starting with a digit never match the table, so those lines are
    dropped.
- **Verified against the frame's own binary:** the offset table in the
  frame's `sprdwl.ko` (`.data + 0x11bc`, 60 entries) matches the Samsung
  source's `wifi_nvm_data.h` field for field. The frame's open command
  copies exactly 0x100 bytes of it into the 279-byte command.
- **Design:** ship the block as a firmware file built at install time, and
  have the driver send it, instead of parsing ini files in the kernel.

## 7. How it maps onto mainline

Three pieces, each testable on its own:

1. **`sprd-wcn` loader:** remoteproc, or a small platform driver.
   - Inputs: firmware file, the reserved-memory region, PMU/AON syscons,
     the watchdog IRQ, and `clk_mspi`.
   - DT node: modelled on the vendor's `scproc@8a800000` ("cpwcn"), written
     as a fresh binding.
   - Size: about 250 lines.
2. **`sprd-sipc` transport:** port the vendor's smsg, smem, sbuf and sblock,
   trimmed to one remote (WCN) and the channels Wi-Fi needs (4, 7, 8).
   - It exposes an in-kernel API: open a channel; read and write an sbuf
     ring; get, send and release sblocks.
   - Doorbell: the IPI block plus SPI 73. Mainline's `mailbox` framework
     fits the doorbell if you want it, but it isn't required.
   - Size: about 1500 lines.
3. **`sprdwl-mainline`:** a cfg80211 full-MAC station driver.
   - Structure: closer to `brcmfmac`'s fweh/cfg80211 split than to any
     mac80211 driver. One netdev. cfg80211 ops for scan, connect,
     disconnect, add/del key, get_station and set_power_mgmt. An event
     worker for channel 8. A NAPI-style receive loop on channel 7.
   - Size: about 2000–2500 lines.
   - **Write it fresh**, using the vendor's `sipc.h` structs and command
     ids as the protocol definition. Forward-porting sprdwl from 3.10 means
     ripping out wakelocks, early_suspend, `filp_open` of /system files and
     the NPI netlink, and chasing many cfg80211 API changes. Half of its 7k
     lines are modes we don't need.

Integration follows the existing pattern: `kernel/*.c`, hooked
by `build/kernel.sh`, DT nodes in `kernel/sc7731g.dtsi`, built in (`=y`).
Userspace is plain wpa_supplicant (nl80211) in the rootfs.

## 8. Open questions

1. What exactly does ring 0 (wcnd's loop check) expect? (It isn't needed
   for Wi-Fi.)
2. Does the frame's board code register an RF2351 power GPIO, and does it
   move vddsim2 to 2.8 V? If so, how does it coexist with the touch pads
   at 1.8 V? Read the frame's own kernel (`tools/unbootimg.py` on your boot.img) for
   `rf2351` and `vddsim2`.
3. The exact sblock zero-copy block ownership rules.
4. The firmware's power-save commands, and how the host should handle
   cp2's sleep (`SLEEP_STATUS`).
5. Can BT and FM in the same firmware be left untouched? Android starts
   them separately; nothing so far suggests Wi-Fi needs them.

