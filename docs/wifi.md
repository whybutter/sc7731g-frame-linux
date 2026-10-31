# VT10101: Wi-Fi under mainline

Goal (2026-10-27): the frame reaches the network over Wi-Fi, without the
USB cable. This file is the assessment and then the log,
in the style of `mainline.md`.

Status (2026-10-27): **cp2 boots from our kernel and answers over SIPC**.
In wifi2 it replied to `at+spatgetcp2info` with its version, through the
whole chain: loader, doorbell, smsg, sbuf. That is all from user space,
with no kernel driver yet. Next: `WIFI_CMD_DEV_OPEN` and a scan (below).

## What the Wi-Fi is

There is no Wi-Fi chip on a bus. The SC7731G has an integrated connectivity
subsystem, **WCN**. It is an ARM core of its own, the vendor's "cp2", that
runs Spreadtrum's firmware and does the 802.11 MAC (and BT and FM). The radio is
an external **SR2351** RF chip, under the can next to the SoC. The AP never
touches the radio itself. It sends cp2 commands and Ethernet frames through
shared DRAM. Android's `getprop` says `ro.wcn.hardware.product=sr2351`, and
the firmware reports Project Version `CP2_WCN_14D_W16.26.3_Release` (built
2016-06-22).

## How Android brings it up (vendor kernel 3.10 + wcnd)

Sources: the frame's own vendor DT (`scproc@8a800000` "cpwcn", `sipc_wcn`,
`spipe_cpwcn`, `sprd_wlan`, `sprd_rf2351`), the SM-G531H GPL kernel
(`drivers/misc/sprd_cproc`, `drivers/sipc`, `drivers/net/wireless/sprdwl`,
`drivers/misc/sprd_2351`), and the frame's `/system/bin/wcnd` and
`/lib/modules/sprdwl.ko`. The frame's kernel is a newer Spreadtrum tree
(`sc7731_m_6.0`, with an extra `seblock.c` and an `at+switchclk` hook in
sprd_2351). Its sprdwl.ko exports the same functions as the Samsung source,
command for command, so the Samsung source is a fair description of the
protocol. The frame's binary is the ground truth where they differ.

1. **Loader, `sprd_cproc`** (`/proc/cpwcn/{modem,start,stop,wdtirq}`).
   `wcnd` copies the `wcnmodem` partition (p9; 630 KiB used of 1 MiB) through
   `/proc/cpwcn/modem` to **0x8a808000**. The image is linked there: its reset
   vector is an absolute 0x8a85aafc. `start` then:
   - writes a 3-word trampoline (`ldr r0,[pc]; bx r0; .word 0x8a808000`) into
     IRAM at 0x50003000;
   - in PMU_APB, clears `PD_CP2_SYS_CFG` (0x402b0068) bit 25 (force
     shutdown);
   - sets `CP_SOFT_RST` (0xb0) bit 2;
   - clears 0x68 bit 28 (force deep sleep);
   - clears 0xb0 bit 2.

   `stop` waits for `SLEEP_STATUS` (0xd4) bits 12-15 to be 0, then does the
   reverse. cp2's watchdog is SPI 85. That is the whole loader: nothing is
   handed over in registers, and cp2 finds its message ring at a fixed address
   (its `MEM_GetSmsgCfg`).
2. **SIPC, the transport** (`drivers/sipc`). It has four layers, all over
   shared memory:
   - **Doorbells.** The AON IPI block 0x402c0000: writing 0x100 there rings
     cp2. Ours is SPI 73 (`sipc_wcn`, `sprd,ap2cp` 0x402c0000, `sprd,cp2ap`
     0x402c0004, trig/clr 0x100), cleared by writing 0x100 to 0x402c0004.
     The gate is AON_APB EB0 bit 21.
   - **smsg.** 8-byte messages `{u8 channel, u8 type, u16 flag, u32 value}` in
     a pair of 128-entry rings at **0x8aa00000**, the last page of cp2's
     region. The AP's tx is at +0, its rx at +0x400, and the free-running
     rd/wr pointers at +0x800. Each channel is opened with an OPEN (flag
     0xBEEE) from both sides.
   - **smem.** A gen_pool over 0x87800000-0x879fffff; `sipc_wcn`'s window is
     0x87940000 + 768 KiB. cp2 is always told the physical address it is
     given, so no remapping is involved.
   - **sbuf and sblock**, on smem:
     - *sbuf* is byte-stream rings. Channel 4 has 13 rings of 4 KiB each way:
       ring 0 is wcnd's loop check, ring 2 assert, ring 5 AT commands, and
       ring 7 sprdwl's commands.
     - *sblock* is fixed-size block pools: channel 7 carries data, 64 tx and
       128 rx blocks of 1664 bytes; channel 8 carries events. cp2 asks for
       the layout with a CMD (`SBUF_INIT`) and the AP answers DONE with the
       smem address. After that, data moves by pointer updates plus an EVENT
       naming the ring.

   Android's dmesg shows exactly this: `sbuf 3-4 ring 7`, `Recv sblock8 scan
   result event`.
3. **sprdwl**: a cfg80211 **full-MAC** driver, not mac80211, and not wext.
   - **Station path.** cp2 does scanning, auth and association. The driver
     maps cfg80211's `scan`, `connect`, `disconnect`, `add_key`, `del_key`
     and `get_station` to 44 numbered commands (`WIFI_CMD_*`, a header plus
     a packed struct, over sbuf ring 7). Results come back as sblock events.
     Frames are Ethernet frames in sblocks. EAPOL goes through the data
     path, so the host's wpa_supplicant runs the 4-way handshake and
     installs keys with `add_key`. There is also a vendor shortcut where the
     PSK is handed to cp2 (`WIFI_CMD_PSK`).
   - **Other code.** The rest is soft-AP, P2P, WAPI, and "NPI" factory test
     over generic netlink.
   - **Calibration.** `WIFI_CMD_DEV_OPEN` carries a `wifi_nvm_data` struct.
     sprdwl parses it in the kernel from
     `/system/etc/connectivity_configure.ini` and
     `connectivity_calibration.ini` (both in the backup's system.img). It
     also checks for a zero-copy mismatch: the firmware has "ap is not zero
     copy version, but cp2 is zero copy".
   - **MAC address.** `wcnd` makes up a random MAC into
     `/data/misc/wifi/wifimac.txt`; nothing is in productinfo.
4. **RF power.** sprdwl's probe calls `rf2351_gpio_ctrl_power_enable()`
   (a board GPIO, if the board registered one) and
   `rf2351_vddwpa_ctrl_power_enable()`. In the Samsung tree the second one
   takes **vddsim2 to 2.8 V**, the rail our touch needs at 1.8 V. Whether
   the frame's board code registers either is not known yet; its kernel has
   both functions. The RF2351's SPI (MSPI, AON_APB EB0 bit 23, `clk_mspi`)
   is never turned off by the vendor "because BT can't work" then. Our
   clock tree has `clk_mspi` and nobody claims it, so the clock core turns
   it off at boot.

Nothing in mainline covers any of this. Spreadtrum's later chips (SC9860,
UMS) got a mailbox and an SDIO/PCIe `sprdwl-ng` out of tree, but this
generation never went upstream.

## What mainline needs

| piece | size (new code) | risk |
|---|---|---|
| cp2 loader: firmware into the reserved region, the PMU sequence, watchdog IRQ. A platform driver, or remoteproc with a raw-binary `load` | ~250 lines | low: 4 registers and a copy, and the user-space test below exercises exactly that |
| SIPC: IPI doorbell, smsg, smem, sbuf, sblock. The vendor code is generic kernel C with few API dependencies, so it ports nearly as is, trimmed to one remote and the channels Wi-Fi uses | ~1500 | medium: the protocol is all in the source, but the frame's tree has newer variants (seblock) |
| Wi-Fi: a cfg80211 full-MAC STA driver over sbuf ring 7 / sblock 7+8 | ~2000-2500 | medium-high: cfg80211 has changed a lot since 3.10; the command structs must match this firmware (the frame's sprdwl.ko settles any doubt) |
| Calibration: `wifi_nvm_data` built **offline** from the two .ini files and loaded as a firmware file, instead of an ini parser in the kernel | ~300 (a tool) | medium: struct layout per firmware version |
| RF power: whatever GPIO/LDO the SR2351 needs | small | unknown until cp2 runs and we see whether a scan finds anything |

About 4-5k lines all told. Each layer can be tested on its own (cp2 boots, a
channel opens, an AT command answers, `WIFI_CMD_DEV_OPEN` returns, a scan
returns BSSes), so it goes step by step, not as one big bang. Expect a few
weeks of work with many short hardware boots, mostly of the
`cp2test` kind.

### Port the vendor driver, or write a fresh one?

- **Forward-port sprdwl as is** (3.10 to 6.18). It means removing
  wakelocks and early_suspend; replacing `filp_open` of /system files and
  the NPI netlink; and following cfg80211 from `cfg80211_send_deauth`,
  `cfg80211_inform_bss` and `cfg80211_scan_done(bool)` to today's APIs.
  That is mechanical, but it is 7k lines of a vendor's shape, half of it
  modes we don't need (AP, P2P, WAPI, NPI).
- **Fresh minimal driver**, using the vendor's `sipc.h` structs and command
  ids as the protocol spec: STA only, no P2P/AP/WAPI/NPI. It is smaller,
  readable, and ours to debug.

**Recommendation:** port SIPC (it is the transport, and generic), write the
Wi-Fi driver fresh against the vendor's command definitions, and keep the
loader small. Everything is built into the kernel like our other drivers
(`kernel/*.c` hooked in by `build/kernel.sh`). The firmware stays the
frame's own: read from p9 at install time into `/lib/firmware`, never
committed.

## The fallback: a USB Wi-Fi dongle on the micro-USB port

The frame runs off its barrel jack, so the micro-USB port is free for OTG host
mode. dwc2 is built dual-role and works with our PHY driver.
- **The vendor DT wires up OTG:** `usb@20200000` has `id-gpios` (GPIO 72),
  `vbus-gpios` (on the PMIC EIC), and **`boost-gpios` = GPIO 69**, a 5 V
  boost enable for VBUS. That is the reference design. The ODM's Android
  declares no `android.hardware.usb.host` feature (only accessory), so it was
  never their use case. Whether the boost is fitted on this board is the
  question.
- **What it would take:** `dr_mode = "host"`, VBUS as a GPIO regulator on
  GPIO 69, and a dongle with a mainline driver and firmware: mt7601u (cheap,
  firmware in linux-firmware), or an RTL8188EU/8188FU (rtl8xxxu). That is days
  of work, not weeks.
- **Costs:** the port can't also be the USB network link, so the frame
  would be reachable only over Wi-Fi and the UART. And there would be a
  dongle sticking out of a photo frame.
- **One boot answers it:** host mode, a dongle plugged in through an OTG
  adapter, and see whether it enumerates. If VBUS isn't there with GPIO 69
  driven high, a powered OTG hub (a Y-cable) still works.

So the dongle is a credible plan B, and it can be tested independently of
the native work.

## Step 1: does cp2 run from our kernel and answer over SIPC?

**Test it without writing a driver.** Everything the loader and smsg need
is reachable from user space through `/dev/mem` (`CONFIG_DEVMEM=y`, no
`STRICT_DEVMEM`). The regions are reserved `no-map` in the dts, and that is
what makes `/dev/mem` allow them. cp2's interrupt can be replaced by
polling the ring.

**`initramfs/cp2.c`** (bin/cp2, freestanding like devmem) has these
commands:
- `regs`: the PMU/IPI/IRAM state, the ring pointers, cp2's two interrupts
  as GIC pending bits, and a checksum of cp2's RAM past the image.
- `stop` and `start`: the vendor sequences above, with the IPI and MSPI
  gates turned on first.
- `load FILE`: the copy, with a read-back check.
- `hello`: open channel 4, answer cp2's `SBUF_INIT` with an sbuf layout
  written into 0x87940000, send `AT+SPATGETCP2INFO` on ring 5, and print
  what comes back.
- `watch`: print anything cp2 sends.

**`initramfs/cp2test`**, run by `cp2test` on the command line, runs the
sequence: find the `wcnmodem` partition by name, then regs, stop, load,
regs, start, 2 s, regs, hello, regs, watch, stop. It reads p9 and writes
nothing to the eMMC.

What each outcome means:
- `tailsum` changes after start: cp2 executed.
- `channel 4 open` and `sbuf init answered`: the transport works as the
  source says.
- `ring 5 says: +SPATGETCP2INFO: ... CP2_WCN_14D_W16.26.3`: the whole
  path works.
- Nothing at all: cp2 didn't start (check `pd_cp2_*`, `cp_soft_rst`, and
  `gic_pend_wdt_spi85`).
- Starts but never opens: the ring address, the doorbell, or cp2 wanting
  something we haven't given it (a clock: `clk_ignore_unused` is on the
  test's command line for that reason).

Next after a good result: the first Wi-Fi commands (`WIFI_CMD_DEV_OPEN`, a
scan) can still be driven from `cp2` (sbuf ring 7, sblock 8 for the result
event). A scan that returns BSSes would also settle the RF power question
before any kernel driver exists.

## wifi1 (2026-10-27, the boot, unattended, clean reboot after)

- **Load:** 1 MiB from p9 to 0x8a808000, every word read back right.
- **cp2 runs.** After `start`:
  - `PD_CP2_SYS_CFG` went 0x12080006 -> 0x00080006 (shutdown and deep
    sleep cleared), and `CP_SOFT_RST` 7 -> 3.
  - The RAM checksum past the image changed at +2 s and again by +12 s.
  - cp2 reset the smsg ring itself: Android's leftover pointers (0x56/0x5a)
    were 0, and the rx write pointer was 2. So cp2 owns the ring's
    initialisation, as the vendor AP code (which never writes it) implied.
  - SPI 73 was pending in the GIC: cp2 rang our doorbell.
- **IRAM:** reading 0x50003000 after start gives 0xe59ff018 in all three
  words, i.e. `ldr pc, [pc, #0x18]`, a vector table. Once cp2 runs, that
  window shows cp2's own memory, not the trampoline we wrote. Harmless,
  but `regs` can't verify the trampoline after start.
- **SIPC up to sbuf.** cp2 opened channels 4 and 5 (spipe and slog) on its
  own before we did. We sent OPEN on 4; cp2 sent `CMD SBUF_INIT`; we
  answered DONE with 0x87940000. cp2 consumed every message we sent (the
  tx ring ended 3/3).
- **No answer to `AT+SPATGETCP2INFO\r` on ring 5**, in 15 s of polling
  all 13 rings. Suspects:
  - Case: wcnd sends `at+spatgetcp2info\r`, lowercase.
  - The ring: that string sits next to wcnd's loop-check code, whose
    interface is `/dev/spipe_wcn0`.
  - cp2 never read our bytes at all. wifi1 didn't dump the ring
    pointers, which would have said.

  wifi2 sends the lowercase string on ring 0, then on ring 5, and dumps
  every ring's rd/wr pointers.
- **The warm restart after wifi1 never came back.** We found the
  frame dark and power-cycled it; the cold boot was fine. The UART shows
  the reset happened (`reboot: Restarting system`). Then the boot ROM
  printed `MCI ERF` twice and `ROM`: it couldn't read the eMMC. That is
  the signature of the 2026-10-24 warm reset after our eMMC driver had
  been used (`mainline.md`). wifi1 did two new things before that
  restart:
  - a raw 1 MiB read of p9;
  - starting and stopping cp2, which left AON_APB EB0 bits 21-23 set (IPI
    and MSPI, set by us; SPLK came on while cp2 ran) and our trampoline in
    IRAM.

  The vendor reboots with cp2 *running*, its trampoline in IRAM and the
  same gates on, so cp2 is the less likely cause. wifi2 separates the two:
  - The firmware comes from the initramfs (`firmware/wcnmodem.bin`, p9
    copied from the backup, gitignored). `cp2emmc` would bring back the
    partition read.
  - After stop, EB0 bits 21-23 and the three IRAM words go back to what
    the boot left.

## wifi2 (2026-10-27): cp2 answers

- **It came back from the warm restart by itself** (`MCI ERS`, then U-Boot).
  So starting and stopping cp2 is not what killed the restart after wifi1.
  The raw eMMC read is the suspect, chased as an eMMC driver issue
  (`kernel/sdhci-sprd-sc7731g.c`, patch 0002).
- **Ring 5 is the AT ring, and case matters.** wifi1's uppercase
  `AT+SPATGETCP2INFO\r` got nothing. The lowercase string wcnd uses got
  156 bytes back on ring 5:

      Platform Version: MOCORTM_14B_TShark_MPII_W16.19_Debug
      Project Version:   CP2_WCN_14D_W16.26.3_Release
      HW Version:        sc8830g_modem
      06-22-2016 09:35:02

  cp2 also read the same string on ring 0 (tx rd 18/18) and said nothing
  there. Ring 0 is wcnd's loop check, which must want some other command.
- **The exchange,** as the vendor source describes it:
  - cp2 opens channels 4 and 5;
  - we open 4;
  - cp2 sends `CMD SBUF_INIT`;
  - we answer `DONE` with the layout's address;
  - an `EVENT WRPTR` on ring 5 from us is answered by an `EVENT WRPTR` on
    ring 5 from cp2;
  - we acknowledge with `EVENT RDPTR`.

  Only sprdwl's own rings are left: commands on 7, and sblock data and
  events on channels 7 and 8.

### Next: DEV_OPEN and a scan, still from user space

What `cp2` needs for that, all from the vendor source:
- **sblock:** the AP lays out channel 7 and channel 8 pools in smem and
  answers `CMD SBLOCK_INIT`, the same handshake as sbuf.
- **Commands** on sbuf ring 7: a `wlan_sipc_data` header (msg_hdr type/len,
  cmd_tag = type/dir/level/id) plus a packed struct, answered on ring 7.
- **`WIFI_CMD_DEV_OPEN`:** mode, MAC, and a `wifi_nvm_data` built from the
  two .ini files, plus an 8-byte timestamp. The struct layout has to match
  this firmware: build it offline (a tool), and check its size against the
  DEV_OPEN length in the frame's sprdwl.ko.
- **`WIFI_CMD_SCAN`:** the result arrives as an event in a channel-8
  sblock.

A scan that lists the neighbourhood's BSSes proves the RF side, and with it
the power question, before any kernel code exists. After that comes the
kernel work in the order of the table above.

## The USB dongle (2026-10-28): the native path is written up

We moved Wi-Fi to a USB dongle for a while; the native WCN work was
written up as a spec first (`docs/wcn-protocol.md`). The dongle is a TP-Link
TL-WN821N **v4.2**: AR7010 + AR9287, so **ath9k_htc** with
`htc_7010-1.4.0.fw`. Its interface will be `wlan0`.

- **Host mode is a runtime switch.** dwc2 stays a gadget by default (the
  debug link). The `hosttest` and `wifitest` initramfs tests write `host`
  to `/sys/class/usb_role/*/role`. dwc2 then forces host mode and enables
  its `vbus-supply`, the OTG boost on GPIO 69 (the vendor DT's
  `boost-gpios`, a fixed regulator in our dts). A reboot goes back to
  gadget. The vendor's host mode uses the same PHY tune word, so the PHY
  glue is unchanged. Whether the boost is fitted on this board is the
  first test's question; a powered OTG hub is the way around it if not.
- **`WIFI=1 ./build.sh kernel`** merges `kernel/wifi.config` (cfg80211,
  mac80211, ath9k_htc; mt7601u and rt2800usb as stand-ins; zstd firmware
  loading) and adds Alpine v3.22's wpa_supplicant and iw, their
  libraries, the firmware and the signed regulatory.db to the initramfs
  (`build/wifi-userland.py`). The default build is unchanged, because it
  has to fit the 16 MiB boot partition. No credentials go into an image:
  since 2026-10-28 wifitest reads `FRAMEBOOT/wpa_supplicant.conf` (fl-boot,
  mounted read-only), the same file the userland reads at /boot.
- **`hosttest`** lists the USB bus (vid:pid, product, driver) and the VBUS
  regulator every 5 s for 90 s, then reboots. **`wifitest`** waits up to
  90 s for a wireless interface, scans, and, if the build has credentials,
  associates, runs DHCP and pings the gateway, then reboots. Both report
  over the UART only (`host:` / `wifi:` lines), since host mode takes the
  USB network link away, and neither touches the eMMC.

### hosttest on out-wifi1 (2026-10-28): host mode works, and the dongle is Realtek

- **Host mode works.** The role went none -> host at 5 s, and `otg-vbus`
  came on at 15 s with GPIO 69 driving high. So the OTG boost is fitted,
  and it powered the dongle plugged straight into the port, without a
  powered hub. `new high-speed USB device number 2 using dwc2` came at
  45 s, and the device stayed at 480 Mb/s until the reboot at 96 s, with
  no over-current and no disconnects. Then it came back into the userland
  from the eMMC cleanly.
- **The dongle is not what the label said.** The "v4.2" TL-WN821N
  enumerates as **0bda:8178 "802.11n USB WLAN"**, an RTL8192CU, so
  ath9k_htc had nothing to bind. out-wifi2 brings back **rtl8xxxu**
  (0x8178 is in its `RTL8XXXU_UNTESTED` table) with
  `rtlwifi/rtl8192cufw{,_A,_B,_TMSC}.bin`. ath9k_htc stays in.

### wifitest on out-wifi2 (2026-10-28): the dongle scans

- rtl8xxxu reports `RTL8192CU rev A (TSMC) romver 0, 2T2R, TX queues 2`. It
  loaded `rtlwifi/rtl8192cufw_TMSC.bin`, firmware 88.2 (signature 0x88c1).
- **The dongle can be plugged in at any time.** It went in at 58 s;
  wlan0 was up at 63 s and the scan found 9 BSSes from -40 to -82 dBm.
- **The log formatting is fixed.** `iw dev`'s fields are tab-indented, and
  a UART viewer dropped them; `wifitest` now turns tabs into spaces. The
  BSSIDs lost a stray "(on" from `iw`'s `BSS xx(on wlan0)`.
- **Next:** association, with the credentials from FRAMEBOOT, then the
  userland itself: the kernel config now has cfg80211, mac80211 and
  rtl8xxxu, and a small service there does the role switch after
  switch_root (where the firmware is), with a fall back to the gadget link
  when wlan0 gets no address in time.

### wifitest on out-wifi3 (2026-10-28): associated, leased, pinged

**The frame is on Wi-Fi.** This run took its credentials from
`FRAMEBOOT/wpa_supplicant.conf`, written there over SSH.
- **Association:** 5 s after wpa_supplicant was up, to the -34 dBm AP on
  2422 MHz, tx 144.4 Mbit/s MCS 15 short GI.
- **DHCP and ping:** a lease from the router (7200 s), and the gateway answered 3/3 pings in 33-103 ms. Then a clean reboot.

**The 29 s before wpa_supplicant started was entropy, not
wpa_supplicant.** Its first read logged `random: wpa_supplicant:
uninitialized urandom read` at 17.7 s. It initialized only when the kernel
logged `crng init done` at 46.3 s. This board has no hardware RNG driver,
and an initramfs carries no seed. In the userland the fix is a seed kept
across boots: OpenRC's `seedrng` must run before wpa_supplicant, which
credits the saved seed and initializes the CRNG at once on every boot after
the first.

- **The p16 kernel (2026-10-28) is out-p16b,** the kernel #12, with
  cfg80211, mac80211, rtl8xxxu and regulatory.db in its initramfs. It went
  in through the `kernel-trial.sh`: one boot from recovery, checked
  healthy, then promoted. regulatory.db now loads at boot.
  Host mode in the userland came with its next update.

## wifi3 (2026-10-28): the WCN command path, over SSH

The frame runs its userland on Wi-Fi (the dongle) so cp2 can be
exercised from a computer without a boot cycle: `scp cp2` to the frame,
`ssh root@<frame> 'cp2 scan'`, iterate. The firmware and the
calibration block live in `firmware/` (gitignored, built once from the
backup's p9 and the two `.ini` files in system.img via
`tools/wcn-nvm.py`), and `build/kernel.sh` already copies them into
`/lib/firmware/` in the initramfs.

`initramfs/cp2.c` gained the next layer of the stack:
- `sblock_layout`, laying out an sblock pool in smem as
  `drivers/sipc/sblock.c sblock_create()` does (two ring_header structs,
  the tx and rx block areas, the ring and pool blks arrays, all
  initialised to "all blocks in the pool");
- `pump`, a single routine for every smsg type cp2 sends: it records the
  `T_OPEN` ack for channels 7 and 8 and answers each channel's
  `CMD SBLOCK_INIT` with `DONE` carrying the pool's smem address;
- `sbuf_write_bytes` / `sbuf_read_bytes`, binary sbuf access that uses
  `pump` to make progress and reads the ring 7 response bytes;
- `wifi_cmd`, the sprdwl message layer: msg_hdr | cmd_tag | seq, 8-byte
  command header. The response's `u16 status_code` is read at offset 8;
- `dev_open` and `wifi_scan`, with `scan_step` driving the sequence from
  user space.

cp2 is lazy: it only sends `T_OPEN` / `CMD SBLOCK_INIT` on channels 7 and
8 **once we poke it with a write on sbuf ring 7** (our DEV_OPEN command).
So the order is: `cp2 start`, `cp2 hello`, then `cp2 scan` which lays out
the sblock pools in smem, sends `T_OPEN` on 7 and 8, and only then sends
DEV_OPEN; the pump answers `CMD SBLOCK_INIT` while DEV_OPEN waits for its
reply.

### DEV_OPEN: zero-copy flag is required, and the frame returns 0xff01 with it

- With `mode = DEV_MODE_STA` (no FW_ZEROCOPY), cp2 answers status
  **0xfe = ERR_CP_ZEROCOPY_AP_NOT**: the firmware is zero-copy, we aren't.
  That confirms `docs/wcn-protocol.md`'s reading of the firmware string and that
  our command framing is byte-perfect: cp2 decodes the mode byte, matches
  it against its own build, and returns the vendor-documented error.
- With `mode = DEV_MODE_STA | FW_ZEROCOPY` (0x80), the frame returns
  **status 0xff01** instead of 0. Not one of sprdwl's documented codes
  (`0xfd` / `0xfe`) and not generic `WLAN_SIPC_STATUS_ERR = 0xffff`. The
  high byte is 0xff and the low byte is 0x01, which has the shape of a
  firmware sub-error code. Despite the status, **the next command (SCAN)
  is accepted with status 0** -- so DEV_OPEN is partially effective and
  the firmware isn't rejecting commands over this.
  - *Corrected in wifi4:* it isn't partial. 0xff01 is the firmware's
    "error mode" exit: mode 0 isn't station on this firmware, station is 1.
    SCAN is acknowledged by the sbuf command task whether or not a STA task
    exists, which is why it still "worked".

### SCAN: accepted, no result event

`WIFI_CMD_SCAN` with an empty payload (wildcard, as vendor does when no
SSIDs are provided) returns status 0. Then nothing: channel 8's sblock
ring stays empty for 60 s. `cp2 sblk` dumps show
`pool.rx_rd = ring.rx_wr = 0` -- cp2 never took a free rx block from the
pool nor queued a filled block to the ring. No `WIFI_EVENT_READY` and no
`WIFI_EVENT_SCANDONE` arrive.

This matches open question 2 in `wcn-protocol.md` (§8):
"Does the frame's board code register an RF2351 power GPIO, and does it
move vddsim2 to 2.8 V?" The whole SIPC command path works -- `pump` sees
every expected handshake -- so the gap is on the RF side. Likely one of:
- the SR2351 power rail isn't on (vddwpa / vddsim2);
- a GPIO enable the vendor driver asserts isn't being driven here;
- `clk_mspi` is on (AON_APB EB0 bit 23 is set after `cp2 start`) but the
  MSPI block isn't being driven to configure the RF.

The native driver work ahead now has a shape: step 1 and step 2 of the
protocol spec are done up to the SIPC transport, and the next thing to
settle is RF power. That is kernel-side (regulators / GPIOs / DT), not
userspace -- so there is no further user-space test to run from here.

*Wrong, see wifi4:* it was never RF power. The scan works with the PMIC
as the userland leaves it. The empty sblock 8 had two causes: the wrong
DEV_OPEN mode, and an sblock pool header written at the wrong offset.

## wifi4 (2026-10-29): the native radio scans

`cp2 scan` from user space over SSH, the userland on slot a, the dongle
still carrying the SSH. DEV_OPEN returns 0, cp2 sweeps channels 1-13, and
the scandone event on sblock 8 lists the neighbourhood (neighbours' names
and our own addresses masked here):

    cp2: DEV_OPEN ok
    cp2: scandone, event bytes=2641
    cp2:   bssid xx:xx:xx:xx:xx:xx ch=1 rssi=-33 ssid="<neighbour>"
    cp2:   bssid xx:xx:xx:xx:xx:xx ch=1 rssi=-34 ssid="<neighbour>"
    cp2:   bssid xx:xx:xx:xx:xx:xx ch=3 rssi=-7 ssid="<our AP, 2nd SSID>"
    cp2:   bssid xx:xx:xx:xx:xx:xx ch=3 rssi=-7 ssid="<our AP>"
    cp2:   bssid xx:xx:xx:xx:xx:xx ch=3 rssi=-32 ssid="<our AP>"
    cp2:   bssid xx:xx:xx:xx:xx:xx ch=8 rssi=-55 ssid="<neighbour>"
    cp2: scandone, 6 BSSes

The first record is a probe response (frame control 0x0050) addressed to
our MAC, so cp2 transmitted probe requests: **TX works as well as RX**.
Reproducible: two runs, 4 and then 6 BSSes.

### What was wrong, and how it was found

1. **Station is mode 1, not 0.** The Samsung source's `DEVICE_MODE_STA = 0`
   doesn't hold for this firmware. The frame's own `sprdwl.ko` (in the
   vendor boot ramdisk, `lib/modules/sprdwl.ko`, not stripped;
   system.img's `/lib/modules/sprdwl.ko` is only a symlink to it)
   decides it in `sprdwl_open_mac`: a jump table on the nl80211 iftype
   sends STATION and P2P_CLIENT to 1, AP and P2P_GO to 2.
   `sprdwl_mac_open_cmd` then ORs 0x80 in and builds the same 279 bytes
   we send. The firmware's handler (Thumb code at ~0x8a81ec80, reaching
   its strings PC-relative, which is why no literal pointer to them
   exists) reads `mode & 0x7f` and `mode >> 7`:
   - the zero-copy check gives 0xfe or 0xfd;
   - then `wifi_open_seth`, whose failure is `~2` = 0xfffd;
   - then the MAC, the timestamp at offset 271, and a switch on the
     mode: 1, 2 or 3 creates a task, anything else is
     "%s: error mode=%d, ap_task_id=%x, sta_task_id=%x". With 0x81: `Create
     sta thread ok`, status 0.
2. **The sblock pool header is at word 12.** `struct sblock_header` is two
   12-word ring headers. `cp2.c` wrote the pool at word 24 (the header's
   total size), i.e. into the first tx block, and cp2 read an empty pool.
   It takes its zero-copy rx buffers from there and logged `sblock_get:
   channel is 7 is err !` / `wlan_sipc_mem_alloc sblock_get blk error!`
   in a loop.
3. **cp2's own log made (2) obvious,** and is the tool for everything that
   follows. smsg channel 5 is the vendor's `slog_wcn`: one sbuf ring, 32 KiB
   AP -> cp2 and 256 KiB cp2 -> AP (vendor DT `slog_cpwcn`). cp2 opens it
   itself at boot. `cp2 scan` opens our side, answers its SBUF_INIT
   (layout at smem + 0x71000) and sends `at+armlog=1` on AT ring 5. It
   then prints everything cp2 traces as `cp2log:` lines: the sblock
   headers as cp2 sees them, the STA task, every channel of the scan.
   Before a working DEV_OPEN, the log stays silent.

Also from the frame's `wcnd`: on a user build it sends
`at+cp2_enter_user=1` on ring 5 before anything else ("in user version,
need to config CP2 to enter user mode!!!"). cp2 answers `+ok:cp2_user_`.
`cp2 scan` sends it too (`cp2 scan SECS MODE NVM eng` skips it). It is
not what fixed the open, but the driver should do what wcnd does.

### Power: not needed for this, and what the vendor does

- With the scan failing for the reasons above, we tried the rails first.
  `cp2 adibits REG SET CLR` is a bit-only read-modify-write of one
  SC2713S register, done the way spi-sprd-adi does it:
  - read: the physical address into RD_CMD 0x40030024, wait for RD_DATA
    0x40030028 bit 31 to clear;
  - write: a store to the window at 0x40038000 + reg, behind the write
    FIFO at 0x4003002c.

  Never a constant: 0x810/0x814 also hold the eMMC core and I/O rails
  (bits 8 and 7), and 0x81c holds vddsim2 (bit 4).
- On, one at a time, each changing nothing:
  - vddwpa (0x81c bit 11 clear) and vddwrf (0x814 bit 13 set, 0x810 bit
    13 clear), the vendor DT's two WCN DCDCs, both off under our kernel;
  - vddsim2 at 2.8 V (0x824 bits 9:8 = 0).

  All put back after, and the working scan ran with none of them.
- What the vendor does for Wi-Fi: `rf2351_vddwpa_ctrl_power_enable` takes
  **vddsim2 to 2.8 V**. The SP7731G reference boards all call
  `sprd_sr2351_vddwpa_ctrl_power_register()`, and none registers an RF
  GPIO. The vendor Wi-Fi path never uses the MSPI ops (FM and GPS do).
  Scanning and probe TX work at 1.8 V. Whether 2.8 V matters for TX
  power at range is open, and so is the RSSI that reads -7 dBm for the AP
  the dongle sees at -35 (the neighbours look plausible). It costs the
  touch pads nothing: the stock product ran both, and the touch chip
  itself is on vdd28.

### Next

- The kernel driver can start: everything it needs at the protocol level
  is now proven against this firmware:
  - the loader;
  - smsg, sbuf, sblock (with the pool at word 12);
  - user mode, DEV_OPEN mode 1 | 0x80, SCAN, and the scandone format;
  - cp2's log on channel 5.
- From user space still: CONNECT to an open network, then the data path on
  sblock 7 (zero-copy: the first byte of an rx block is the offset to the
  frame, `main.c` sprdwl_rx_handler).
- **Bluetooth**, wanted for BLE (2026-10-29): the same firmware
  and transport. The vendor DT's `stty4bt_cpwcn` is HCI over sbuf
  channel 4, ring 10 (bufid 0x0a), so mainline could run it as an HCI UART
  (hci_uart / serdev over a small tty, or a direct `hci_dev`) once SIPC is
  in the kernel.

## wifi5 (2026-10-29): associated with our AP, from user space

`cp2 connect 30 <ssid> <bssid> 3 /tmp/pmk` sends what the
frame's `sprdwl_cfg80211_connect` sends for WPA2-PSK. Ids and payloads
are read from its `sprdwl.ko`, all answered status 0:

| command | id | payload |
|---|---|---|
| P2P_ENABLE | 43 | u8 0 |
| WPA_VERSION | 3 | u32, nl80211's `wpa_versions` as is (2) |
| AUTH_TYPE | 2 | u32, 0 = open system |
| PAIRWISE, GROUP | 4, 5 | u32: WEP40 1, WEP104 2, TKIP 3, CCMP 4, SMS4 7 |
| AKM | 6 | u8: PSK 1, 802.1X 2 |
| PSK | 27 | `sme->key`, at most 32 bytes (the PMK) |
| CHANNEL | 7 | u32 |
| BSSID | 8 | 6 bytes |
| ESSID | 9 | the SSID, raw |
| DISCONNECT | 13 | u16 reason |

There is no CONNECT command: the ESSID starts the join. cp2 logged
auth, then assoc ("Association Successful!"), and set up block-ack both
ways. The connect event (id 0) came on sblock 8.

The PMK came from the frame's own `/boot/wpa_supplicant.conf`.
`mkpmk.py` (not kept) was a one-off: it derives the
PMK on the frame into a 0600 file in /tmp, which is deleted after. Key
material was never printed, and cp2 log lines mentioning keys were
redacted on the frame.

**cp2 does not run the 4-way handshake here, despite WIFI_CMD_PSK.**
Right after the connect event, the AP's EAPOL-Key message 1 arrives on
sblock 7 as an Ethernet frame (type 0x888e), retried every second. cp2
logs "Dropping the non-1x pkt" until the port opens. So the host runs
the handshake, as for any full-MAC driver: wpa_supplicant over the
netdev, keys installed with add_key. That needs the kernel driver.

**The rx block layout** (sblock 7, zero-copy):

    0       u8 offset (0x16 here)
    2+off   struct wlan_sblock_recv_data: u8 is_encrypted, 13 reserved,
            then the Ethernet header (dst, src, type) and payload

cp2 doesn't fill the Ethernet dst of this frame: it reads ff:ff:ff:ff:ff:ff
over stale bytes. The 802.11 header with our MAC as addr1 sits earlier in
the block, a leftover. The driver should treat dst the way the frame's
sprdwl_rx_handler does (to check), not trust it.

## wifi6 (2026-10-29): the kernel driver -- WIP, DEV_OPEN hangs

(Resolved by wifi7 below: this section is kept as what it looked like then.
The DEV_OPEN hang was the pool blks arrays written to the wrong physical
address. The MAC is no longer the FNV-1a one described at the end.)

Twelve commits on top of the bring-up kernel bring the native WCN radio into
the kernel: `drivers/soc/sprd/wcn-sc7731g.c` (loader + SIPC: smsg, sbuf
channels 4+5, sblock channels 7+8) and `drivers/soc/sprd/sprdwl-sc7731g.c`
(cfg80211 full-MAC on top: scan, connect, disconnect, add_key, data
path). A `wcn0` netdev appears on boot alongside the dongle's `wlan0`
(the name `wlan%d` was renamed to `wcn%d` so the userland's wlan agents
don't grab it and retry).

**Open issue:** `WIFI_CMD_DEV_OPEN` hangs the first time `ip link set
wcn0 up` runs. The SIPC transport is proven byte-for-byte: cp2 reads
the 279-byte DEV_OPEN from sbuf ring 7 (`ring7 unread=0`), opens sblock
channels 7+8, sends `CMD_SBLOCK_INIT` for both, and our dispatcher
answers `T_DONE` with the right pool addresses (0x8795b000, 0x879ab000 --
same as user-space wifi5). Then cp2 goes silent. No `EVT_WRPTR` for
ring 7 ever arrives.

Peculiar: `at+cp2_enter_user=1` gets its "+ok:" response (so the sbuf
command/response path works on ring 5), but `at+armlog=1` right before
it never does. The slog ring stays empty, so cp2's internal "Create
sta thread" / error trace isn't visible from the kernel side.

Next step is **not** more guessing: run the user-space `cp2.c` wifi5
path again on a trial kernel (sprdwl unbound, the Bluetooth bridge stopped) with
every smsg logged in both directions, and compare with the kernel
driver's log of the same point. The first divergence after
`sblock ch 8 ready, DONE sret=0` is the bug. The commits are small and
each builds green, so anyone can bisect from there.

### What the kernel driver does today

- `wcn-sc7731g`: loader (request_firmware, PMU dance), smsg transport
  (SPI 73 threaded IRQ, drained in a loop per IRQ), sbuf layouts for
  channel 4 (13 rings) and channel 5 (slog), sblock pools for channels
  7 and 8 (ring/pool headers 48 bytes each, pool header at word 12;
  wifi4's trap -- fixed), smem phys = 0x87940000, cp2 image at
  0x8a808000. API in `wcn-sc7731g.h` (wcn_power_up/down, wcn_sbuf_*,
  wcn_sblock_*, wcn_register_notify).
- `sprdwl-sc7731g`: wiphy + netdev, 2.4 GHz single-band, four ciphers,
  station-only. cfg80211 ops wired for scan/connect/disconnect/add_key/
  del_key/set_default_key. Reader kthreads for sbuf ring 7 (cmd
  responses) and sblock 8 (events: scandone -> cfg80211_inform_bss,
  connect/disconnect -> cfg80211 ops). start_xmit uses sblock 7 at
  offset 50 (SIPC_TRANS_OFFSET), zero-copy. rx_data kthread parses the
  vendor zero-copy block (offset byte, 14 reserved bytes, then
  Ethernet) and netif_rx's it.
- DT child `sprdwl: wifi` inside `wcn@8a800000`. MAC derived from
  androidboot.serialno with FNV-1a as a locally-administered address.
  NVM loaded at ndo_open (not probe, because at probe time
  /lib/firmware isn't yet mounted).

## wifi7 (2026-10-30): WPA2, DHCP, real traffic on the native radio

`wcn0` associates to both of our networks over WPA2-PSK, takes a
DHCP lease (the same address Android had) and passes traffic
next to the dongle. Numbers on #57: ping to the gateway 1.4 ms minimum, a
3.2 MB wget in about a second, an 11 MB UDP flood at about 2.2 MB/s with the
link answering after it. Nine lifecycle rounds clean (`wpa_cli` reconnect,
`ip link` down/up with the supplicant running, supplicant killed then
down/up); one set of threads afterwards, no oops, down in 0 s.

The bugs, in the order they hid each other. Each has its why in the code:

1. **The 4-way "MIC failure" was never about the MIC.** The AP kept
   retransmitting msg 1 because msg 2's RSN element didn't match the
   AssocReq. cp2 builds that AssocReq itself: the vendor's connect forwards
   only a WPS IE from `sme->ie`. cp2 sends RSN caps `00 00`, wpa_supplicant
   assumed `0c 00`. The vendor passes `req_ie` from the CONNECT event to
   `cfg80211_connect_result`, wpa_supplicant adopts cp2's element from it,
   and msg 3 came back on the first try. The event is
   `{u16 status_len, u8 status, u16 bssid_len, bssid, u8 req_ie_len, req_ie,
   u8 resp_ie_len, 3 bytes, resp_ie}`; the SSID we had decoded at p[13] was
   the first element of `req_ie`. Lesson for next time: "MIC failure" was an
   inference from retransmits. An AP is free to drop msg 2 for other reasons.
   (`resp_ie`, the first suspect, is wired too but wasn't it: an assoc
   response carries no RSN.)
2. **Key commands were all wrong.** `WIFI_CMD_KEY` is one id, 10, and the
   cmd_tag's type says what to do: SET 1 (default key), ADD 2, DEL 3. ADD
   went out as SET, so a COMPLETED 4-way still left cp2 with no keys, logging
   `set key idx = 0x52` (mac[0] of the AP) and dropping every data frame in
   `filter_host_rx_11i_supp_sta`. Ids 11 and 12 were invented.
3. **Nothing restarted a stopped tx queue.** `start_xmit` stopped it on an
   empty pool waiting for a notify nobody registered. 64 frames faster than
   the air and the link was dead with wpa_supplicant still COMPLETED.
4. **`ip link set wcn0 down` hanging for half an hour** was `kthread_stop()`
   on threads that had already returned on `sw->stopping`, a use-after-free
   that waits forever holding RTNL. No panic, so no pstore. The flag is gone.
   `wcn_power_down` also stops the slog thread (a second up used to add
   another) and masks the SIPC irq before cp2_stop gates the IPI block, since
   touching it gated would stall the bus.
5. **cp2 goes deaf 10-20 s after DEV_OPEN if no scan followed**: no response,
   no interrupt back, until a restart. +10 s fine; +20, +30, +45, +70 dead;
   after a first scan it sleeps and wakes on demand, 300 s idle included.
   P2P_ENABLE as first command did not help. We don't know the firmware's
   reason; ndo_open just scans.

An eight-hour soak on #57 (a sample a minute, 6 pings and a wget every
fifth): 2887 of 2892 pings, single losses only, 96 of 96 wgets, never left
the AP. #57 was then promoted to p16 and a plain reboot came back on it with
all of the above working; the p16 it replaced (#50) was kept as an image.

### Debugging tools that paid off

- cp2's own trace: `echo 1 > /sys/module/wcn_sc7731g/parameters/trace`. Off by
  default now (it was a printk per frame through fbcon). `set key idx = 0x52`
  in there is what solved number 2 in one look.
- `sprdwl.ko`, not stripped: a table of `(type, id)` per `*_cmd` function
  found number 2's real meaning, and `sprdwl_event_connect_result` number 1.

### Still open

- **Firmware on the root filesystem.** `/lib/firmware/sprd/{wcnmodem,wifi-nvm}.bin`
  were pushed by hand to slot a. An A/B update loses them and
  wcn0 won't come up (`ip link set wcn0 up` fails cleanly). The rootfs has to ship them,
  or the kernel has to find them on the FAT.
- **The name.** `wcn%d` keeps the userland's wlan agents away during bring-up,
  so nothing starts it at boot: it takes `ip link set wcn0 up` and a
  wpa_supplicant by hand. Moving the frame to it means naming it `wlan%d` or
  teaching the userland's network manager, and retiring the dongle.
- **The MAC is hardcoded** to this frame's own address from Android's
  wifimac.txt. Right here, wrong for a second board.
- **No `get_station`.** No RSSI or bitrate in `iw`/`wpa_cli` (vendor: GET type
  0, ids 16 and 17). No power save tuning, WEP/802.1X/AP/P2P untested.
- No DHCP-managed routing tested alongside the dongle: tests here used host
  routes over wcn0 so SSH kept the dongle. Both on the same subnet will need
  a decision when the dongle goes.

## wifi8 (2026-10-30): native only, the dongle retired

A deliberate choice. The frame's Wi-Fi is now the SoC's own WCN as `wlan0`, run by
the userland's networking, wpa_supplicant and DHCP like any other board. #59 is
on p16; a plain reboot came up on it at its usual address with the dongle
unpowered (gone from `lsusb`, no wlan1).

This settles most of wifi7's open list:

- **Name:** `wlan%d`. It probes long before USB host mode, so a dongle would be
  wlan1, unmanaged.
- **Firmware:** in the kernel image (`CONFIG_EXTRA_FIRMWARE`, build/kernel.sh),
  in the boot partition both slots share, so an A/B update can't lose it. Proven
  with /lib/firmware/sprd moved aside (`sprd.pushed` on slot a, unused). About
  0.45 MB in the image: 15.1 MB of the partition's 16 MiB, which is getting close.
- **MAC:** derived from `androidboot.serialno` (a locally administered
  02:fa:... address), stable and per board. FRAMEBOOT/wifi-mac overrides it,
  applied by frameboot so it travels with the kernel and not a root slot.
  A unit can keep a chosen address this way. **Don't use a cloned address on a
  second device on the same LAN.**
- **USB:** host mode off in the userland, so the port is the gadget link again
  and usb0 is the way in when Wi-Fi is down. Not yet proven with a cable: the
  dongle's adapter was still in the port (role reads `none`, usb0 has its address).

### The touch regression this caused

#57, #58 and #59 booted with no touch and no UI: `firmware/` is gitignored
and per checkout, and the one they were built in held only the Wi-Fi's blobs,
so silead_ts failed for good at 1 s, cage found no input and the UI stopped.
#59 was promoted like that, and the next userland trial failed its health
check on it and rolled back. Our checks only covered networking, so a kernel
is now green only when the UI's health check passes too
(tools/bt-kernel/green.sh). kernel.sh refuses to build without the file. #62 has it, promoted and proven
across a plain reboot.

### Still open

- **Bluetooth is off.** The user-space Bluetooth bridge stands down when this
  driver owns cp2, and that has been so since the driver landed, not since
  this section: no hci0 on #50, #57 or #59. It needs a kernel path to sbuf ring
  10 (`WCN_RING_BT`), and the trap in wifi7's number 5 applies: cp2 goes deaf
  unless a scan follows DEV_OPEN, so a Bluetooth-only power-up can't skip it.
- `get_station` and `dump_station` are in since #61: signal (-26 dBm here) and
  counters, so `iw` reports signal. **No tx bitrate**: the rate word
  doesn't map to anything `rate_to_info` knows, even under ping; not looked into.
- `rtl8xxxu` is still in vt10101.config, as a stopgap for a dongle.

