# VT10101: the built-in Bluetooth under mainline

Goal (2026-10-29): Bluetooth on the frame, so BLE software on it works.
The first target was a Timemore scale, a BLE device.

Status (2026-10-30): **Classic Bluetooth is a kernel hci device on
the WCN driver, beside the native Wi-Fi** (bt8; kernel #5 on p16, hci0
appears once user space writes the PSKEY: rootfs/etc/init.d/sc7731g-bt). The
bridge below is
how it ran before that driver owned cp2. Open: cp2 grants Bluetooth little
airtime while the Wi-Fi is associated (bt8, "Coexistence").

Status (2026-10-29, end of day):

- **Classic Bluetooth works as hci0.** `frame-bt`, an OpenRC service in
  the userland at the time (it ran `initramfs/bt.c`'s `bt bridge`; the
  kernel driver replaced it in bt8), loads cp2 from Android's `wcnmodem` partition and
  sends the PSKEY. It then bridges H4 packets between SIPC sbuf channel 4
  ring 10 and `/dev/vhci`.
  - BlueZ powers the adapter at an address derived from the SoC serial.
  - An HCI round trip is about 2.3 ms.
  - A remote-name request to a nearby Mac works.
  - **Not done:** pairing a Classic device (it needs a phone/speaker in
    pairing mode).
  - While `frame-bt` runs it owns cp2 (`/run/frame-bt/owns-cp2`). It
    stands down when the in-kernel WCN driver is bound.
- **The controller is BR/EDR 2.1 with no LE, and not because LE is gated
  off: it isn't in the firmware** (bt1, bt3). No LE-capable SC7731G WCN
  build turned up either.
- **The BR baseband can still be made to receive BLE adverts** (bt4-bt6).
  The setup:
  - the DUT test mode parks it on 2402 MHz (vendor 0xFC03);
  - a patched firmware copy gives a register/memory copy primitive;
  - the test link's descriptor loads any 64-bit pattern into the
    correlator, and any HEC seed.

  With that, it matched an ESP32-C3 beacon's advert, and its RX buffer
  held the raw on-air bits that follow; dewhitened, they read `BERY`.
- **The Timemore scale can't be read that way** (bt7). Its adverts are a
  constant beacon with no weight in them. The weight only comes over an
  encrypted GATT connection, which this baseband can't make. The options
  are the C3 as the scale's central, relaying over Wi-Fi, or a USB BLE
  dongle: a choice to make.

The sections below are the log, in order, dead ends included.

## How Android runs it

The Bluetooth is part of the same WCN co-processor (cp2) as the Wi-Fi
(`docs/wifi.md`), and uses the same firmware (`wcnmodem`, p9) and the same
SIPC transport. Sources: the vendor DT, and the frame's system.img
(`/vendor/lib/libbt-vendor.so`, `/etc/connectivity_configure.ini`,
`/bin/wcnd`, read with debugfs, read only).

- **The transport:** `/dev/sttybt0` is the vendor's `stty4bt` driver
  (vendor DT `stty4bt_cpwcn`: dst 3, channel 4, bufid 0x0a). That means
  **sbuf channel 4, ring 10**, the same sbuf as the AT console (ring 5) and
  sprdwl's commands (ring 7). Bytes pass through raw both ways, H4 framed
  (a packet-type byte, then the HCI packet).
- **The PSKEY block, before any HCI:** libbt-vendor's
  `hw_pskey_vnd_preload` (adapter module +0xc, code at 0x3904) works like
  this:
  - It writes the 144-byte `sr2351_pskey` struct to the tty as it is, with
    no H4 type byte. The struct's own first byte, `pskey_cmd = 5`, marks it.
  - It `select()`s up to 5 s and reads up to 10 bytes. A first byte of 5 is
    success.

  On this firmware the answer is `05 08 08 08`.
- **What's in the PSKEY:** `vnd_load_conf()` parses it from sections 3 and 4
  of `connectivity_configure.ini`, through `sr2351_table` (38 entries of
  `{name, handler, offset, count, size}`). It always overwrites
  `device_addr` (offset 0x50) with `/data/misc/bluedroid/btmac.txt`,
  byte-reversed, or with zeros if that file is missing. The frame has no
  factory address: productinfo has no btmac.txt.
- **`tools/bt-pskey.py`** replays that parser with its quirks:
  - `0x` means hex, anything else is decimal;
  - each value is truncated to its field's size;
  - only `count` values are read per line;
  - the name and value delimiters come from the binary.

  The BD_ADDR is given on the command line, or derived from the SoC serial
  with `--serial`. The block is the device's own config, like the Wi-Fi's
  `wifi-nvm.bin`, so it goes in gitignored `firmware/`.
- **wcnd** only starts cp2 when asked ("wcn BT-OPEN"). Before anything else,
  it sends `at+cp2_enter_user=1` on ring 5, as it does for Wi-Fi.

## bt1 (2026-10-29): HCI works, LE doesn't exist

`initramfs/bt.c` includes `cp2.c` whole, so the two tools share one SIPC
implementation, and adds `bt up PSKEY [SECS]`, `bt hci OPCODE BYTES...` and
`bt listen SECS`. The run was over SSH on the userland, with nothing else using cp2:
`cp2 stop; cp2 load; cp2 start; cp2 hello 3; bt up pskey.bin 15`.

- Log channel 5 and user mode (`+ok:cp2_user_`) come up, then the PSKEY:
  the answer is `05 08 08 08`.
- **HCI_Reset:** `04 0e 04 01 03 0c 00`, success.
- **Read_Local_Version:** HCI version 4 and LMP 4, i.e. **2.1 + EDR**;
  manufacturer 0x01ec (Spreadtrum), revision and subversion 0x1250.
- **Read_BD_ADDR** returns the address from the PSKEY, so the block is
  read and applied.
- **Read_Local_Supported_Features:** `ff ff 8d fe 9b 3d 79 c3`. These are
  the PSKEY's `feature_set` with a few bits changed by the firmware.
- **Read_Local_Supported_Commands:** octets 0-23 are populated, **octets
  24-63 are zero**. The LE commands start at octet 25.
- **Every LE command** (LE_Set_Event_Mask 0x2001, LE_Read_Buffer_Size
  0x2002, LE_Set_Scan_Parameters 0x200b) gets Command Status 0x01,
  *Unknown HCI Command*.
- **LE isn't merely gated off.** A PSKEY with the LE feature bits set
  (`feature_set[4] |= 0x40` for LE Supported (Controller), `[6] |= 0x02`
  for simultaneous LE and BR/EDR) changes what Read_Local_Supported_Features
  reports (`... db 3d 7b c3`). The commands stay unknown, and the
  supported-commands map stays the same.
- **The stock Android agrees:** `/system/etc/permissions` has
  `android.hardware.bluetooth.xml` and no `android.hardware.bluetooth_le.xml`.
- **Classic side:** Write_Inquiry_Mode(2) is OK. An Inquiry (LAP 0x9e8b33,
  10.24 s) completes with status 0 and no results; nothing nearby was
  discoverable.

cp2 was stopped after the run and nothing was left on the frame.

## bt2 (2026-10-29): Classic as hci0, through vhci

- **The bridge:** `bt bridge FIRMWARE PSKEY` (in `bt.c`) does the
  following, in order:
  1. loads cp2 from p9, runs `hello`, sends user mode and the PSKEY;
  2. opens `/dev/vhci` and writes `ff 00`, which asks hci_vhci for a
     primary controller (it answers `ff 00` and the index);
  3. from then on, moves H4 packets between vhci and ring 10.
- **Polling:** nothing in the kernel takes cp2's SPI 73, so the bridge
  polls. It calls `poll()` on vhci with a 2 ms timeout, and checks the
  smsg ring and ring 10 on every pass. Ring-10 writes wait for room, since
  the ring is 4 KiB.
- **Result on kernel #46 (BT core, hci_vhci):**
  - BlueZ 5.82 powers it as a BR/EDR adapter with its derived address.
  - An HCI round trip (Read_BD_ADDR through the kernel, the bridge and cp2)
    is 2.3 ms median and 2.5 ms worst over 200.
  - The bridge sits at ~0% CPU.
  - A Remote_Name_Request paged a nearby Mac and read its name, so the
    radio transmits and receives.
- **Userland:** `frame-bt` (OpenRC, switched on per board) runs
  the bridge. `pskey.sh` builds the block at each start from the ini in
  Android's system partition, mounted `ro,noload`.
- **cp2 ownership:** while frame-bt runs it owns cp2, and
  `/run/frame-bt/owns-cp2` says so on the frame. Stop frame-bt before any
  cp2 test.
- **What comes next:** the kernel SIPC driver will expose
  `/dev/wcn-sbuf-ring10` (byte stream, poll driven by SPI 73, EPIPE when
  cp2 restarts). The bridge will then drop its own load and polling.

## bt3 (2026-10-29): LE is not gated in this firmware, it isn't there

Static analysis of `wcnmodem.bin`, linked at 0x8a808000, mostly Thumb.
The question was whether LE is compiled in and switched off.

- **The controller lives in this image.** Its init routine (around
  0x8a8374a0) builds the version record and the supported-commands mask:
  - the version record: HCI and LMP version 4, revision and subversion
    0x1250, the manufacturer taken from the PSKEY's `comp_id`, ACL buffers
    1021 x 20, SCO 120 x 8;
  - the mask, byte by byte, 28 octets long. LE commands start at
    octet 25 and its octets 24-27 are zero.
- **The HCI dispatcher is at 0x8a89df46.** It works like this:

      ogf = opcode >> 10
      if (ogf == 0x3f)  vendor(...)        /* 0x8a8321f8 */
      else if (ogf < 7) table[ogf](...)    /* RAM table at 0x8a8ca0e0 */
      else              status = 0x01      /* Unknown HCI Command */

  OGF 8 can never reach a handler: there is no table slot and no flag to
  flip. The per-OGF handlers sit next to it: link control (OCF <= 53),
  link policy (OCF <= 17), baseband (OCF <= 105), and two short ones
  (<= 7, <= 4).
- **There is no LE link layer:**
  - the advertising access address 0x8E89BED6 appears nowhere, in any byte
    or bit order;
  - the 0x555555 hits are a fill pattern, not a CRC init;
  - there is no 2402/2426/2480 MHz channel triple.
- **A false lead, noted so nobody follows it:** the tables at
  0x8a892080-0x8a892e7c (pointer array at 0x8a889048) do hold
  0x2000-0x2025 entries, which look like LE opcodes. They are the Wi-Fi
  MAC's configuration-ID tables: char, short, int, string and binary WIDs,
  keyed 0x0xxx to 0x4xxx, next to strings like `set_start_scan_req` and
  `get_site_survey`.
- **Nothing on the Android side enables LE either:**
  - no BT patch or RAM-code file in system.img;
  - no vendor opcode sent after the PSKEY (libbt-vendor only does PSKEY
    and low-power mode);
  - no LE property;
  - no `bluetooth_le` permission.
- **But the silicon is probably LE-capable.** Other SC7731G phones (Blu
  Advance 4.0 L3, Archos 40 Power, Lenovo Vibe A) are sold as Bluetooth
  4.0. This frame's firmware is a 2016 build (`CP2_WCN_14D_W16.26.3`), and
  its controller reports 2.1.
  - **Correction (2026-10-29): that inference was marketing.** We read an
    Archos 40 Power 6.0 OTA. What it holds:
    - boot, recovery and system only; no wcnmodem/CP2 image;
    - no `CP2_WCN` build and no access address in system;
    - `android.hardware.bluetooth` declared, but not `bluetooth_le`, the
      same as this frame's Android.

    So the "Bluetooth 4.0" spec line doesn't mean an LE controller. The
    real test of an LE-capable WCN build is a device whose own Android
    declares `android.hardware.bluetooth_le`, together with a CP2 image in
    its package.
  - The other mirrors found were Google Drive, over its download quota,
    and firmwaredrive.com, which is paid. Neither was used.

## bt4 (2026-10-29): driving the radio ourselves, the map so far

Our idea: take LE advertising in by driving the BR baseband and the
SR2351 directly, then do the dewhitening, CRC24 and parsing in software.
This section is the log of that exploration, dead ends included.

### Reading the firmware properly: it relocates part of itself

The first disassembly of the HCI layer was garbage: branches landed in the
middle of Wi-Fi functions. The reason is that the image carries a
scatter-load table at 0x8a8936a4, and part of the code runs at a different
address from where it sits in the file:

| load (file) | exec (RAM) | size | routine |
|---|---|---|---|
| 0x8a8936fc | 0x8a8b8000 | 0xca8c | copy (0x8a8080c8) |
| 0x8a8a0188 | 0x8a8c4a8c | 0x5680 | 0x8a80806c (zero-init?) |

How it was found:
- Branches in the bad region hit a function prologue only 6% of the
  time, against 50-67% elsewhere.
- A vote over every branch target gave a shift of 0x24904.
- That shift is the distance between the two addresses in the table.

The HCI dispatcher sits at exec 0x8a8c284a (file 0x8a89df46). Its vendor
branch goes to 0x8a856afc, the OGF 0x3f handler. That handler has:
- a jump table for 0xFC02-0xFC33;
- compare chains for 0xFC34-0xFCAF and 0xFCD1-0xFCD5.

All the handler bodies are in one module, 0x8a825574-0x8a8266xx.

### The vendor commands that matter

- **0xFC2D:** a register read-modify-write. It does
  `*(base[sel] + off) = (old & ~mask) | (val & mask)`, with base 0x40086000
  (sel 0) or 0x40088000. The offset is a full 32-bit word, so it reaches
  anywhere in cp2's map.
- **0xFC1A:** write BD_ADDR. It stores the address in the struct at
  0x8a8fc100 and recomputes the sync word with 0x8a836fc8.
- **0x8a836fc8, the BR sync-word routine (LAP to 64 bits):** the PN
  constant 0x83848D96BBCC54FC sits in its literal pool, which is how it was
  found. So the sync word is computed in software and loaded into the
  hardware. `tools/bt-raw/syncword.py` replays it.

### A read primitive: one patched handler, on a copy

`wcnmodem-peek.bin` is the frame's image with 29 bytes changed. 0xFC2D's
body at 0x8a825574 becomes a 32-byte Thumb loop: copy N words from SRC to
DST, then send Command Complete as before. Its parameters are
`sel(1) src(4) n(4) dst(4)`.
- **DST:** smem + 0x7c000 (0x879bc000), above everything Wi-Fi and BT use.
  The AP reads it through `/dev/mem`.
- **The tool:** `tools/bt-raw/hcipeek.py ADDR N [OUT]` drives it over a raw HCI socket
  while the bridge runs.
- **Check:** copying cp2's own first 16 words returned the reset vectors
  (`e59ff000 e59ff000 8a85aafc`), so the primitive is proven.
- **Safety:** the eMMC copy and the firmware are never touched. The
  image is loaded from /tmp only.

### The baseband registers (cp2's map), from snapshots

Pages 0x40086000-0x40089fff were snapshotted idle, during an inquiry and
during a page that times out (to 00:11:22:33:44:55, LAP 0x334455). What
the snapshots show:
- 0x40086000 is the busy block: 747 of 1024 words are non-zero.
- 0x40087000 is all zero.
- 0x40088000 has 11 non-zero words; 0x40089000 has one.

| register | idle | inquiry | page | reading |
|---|---|---|---|---|
| 0x40086008 | ffffffff | 331a3ae2 | e642120c | **sync word, low 32**: the GIAC's and LAP 0x334455's, exactly as the firmware's routine computes them |
| 0x4008600c | ffffffff | 4e7a2cce | b0cd1154 | **sync word, high 32** |
| 0x40086018/1c | 1a14 | 5ab5, 6281, 6a91 | c631... | a running clock (native clock?) |
| 0x40086024 | 80008000 | **b332**8000, **b938**8000, **b130**8000 | 00008000 | upper half changes on every inquiry snapshot: hop channel / frequency word? |
| 0x40086070-7c | 0 | 860a0000 860a0801 806c0000 806c0000 | same | per-mode setup |
| 0x40086700 | 3001 | 5e01 | 5e01 | mode / scheduler? |

**The correlator is a 64-bit register pair the firmware loads.** That is
the key fact for LE. A BLE advertising packet on air is: preamble 0xAA,
then the access address 0x8E89BED6 (32 bits, LSB first), then the whitened
PDU header and payload. The 64 bits after the preamble are therefore AA +
16 header bits + 16 bits of AdvA, all whitened per channel. For one known
advertiser (the scale), those 64 bits are constant on a given channel, so
the BR correlator can be loaded with them and match that device's packets.
Both PHYs run at 1 Mb/s GFSK, BR with modulation index ~0.32 and LE with
0.5. The 3 advertising channels (2402/2426/2480) are BR channels 0/24/78.

More of the block, read the same way:
- 0x40086000/04 hold our own BD_ADDR: the UAP and LAP,
  the rest the NAP.
- 0x40086060 is 0x09111708 in every state, so it's a candidate for the
  correlator thresholds.
- 0x40086024 during inquiry is 0xB332, 0xB938, 0xB130. The high byte is
  always `0x80 | (low + 1)`, which reads as two consecutive 7-bit hop
  channels (inquiry sends two IDs per slot on f(k) and f(k+1)), with an
  enable bit.

Dead end: the vendor handler at 0x8a825a5a XORs its 9 parameter bytes with
0x55 and computes a sync word, so it looked like a non-signalling RX test.
It is a test *link*: a descriptor built from a fixed LAP, with the sync
word at +12/+16 as the scheduler uses. It is not a raw receive mode.

Still open:
- how the hop register encodes a channel;
- RX enable outside the scheduler;
- where the bits go after a match, given that a BLE packet fails BR's
  header HEC;
- whether the correlator threshold can be lowered.

### Bit order, the RX buffer, and where the channel isn't (2026-10-29)

- **The correlator's bit order is settled.** An independent
  implementation of the spec's BR sync-word algorithm (Vol 2 Part B
  6.3.3, PN 0x83848D96BBCC54FC with p_i = bit i, BCH g = 0o260534236651)
  gives `4e7a2cce331a3ae2` for the GIAC, exactly 0x4008600c:0x40086008.
  So the pair is the 64 on-air sync bits, LSB first: 0x40086008 bit 0 is
  the first bit after the preamble. For LE, the pattern is
  `AA(0x8E89BED6, LSB first) + whitened header (16) + whitened AdvA[0..1]
  (16)`, packed the same way.
- **The RX payload buffer is at 0x40086400.** After a remote-name request
  to the Mac, a 64 KiB snapshot (0x40080000-0x4008ffff) contained
  `04 00 05 xx xx xx xx xx` there. That is LMP_name_res as decoded:
  opcode, offset 0, length 5, "<the Mac's 5-letter name>". So the buffer holds a payload after
  FEC and dewhitening, and only after a good header (HEC) does it get
  there. The block's other non-zero runs:
  - 344-byte buffers at 0x40086214 and 0x40086414;
  - 1060-byte buffers at 0x40086754 and 0x40086b94, ACL-sized;
  - a packet descriptor at 0x40086200, mirrored at 0x40086740;
  - link state at 0x400860d0-dc, with our LAP at 0x400860d4.

  0x4008f000 is a second busy block, 704 words. 0x40080000-0x40085fff and
  0x4008a000-0x4008efff read as all zero.
- **0x40086018 is CLKN.** It advances about 10 ticks every 3 ms, i.e.
  3.2 kHz.
- **0x40086038 jitters on every sample, even in page scan with nobody
  paging** (222 distinct values in 6 s), so it's a live RSSI/AGC reading.
- **The channel isn't in any register we can read.** In continuous page
  scan (window = interval = 0x12, `scanwatch.py`/`scanwatch2.py`) over 6 s,
  i.e. about 4 boundaries of 1.28 s:
  - nothing in 0x40086000-0x400861ff, 0x40086700-0x4008677f or the first
    1 KiB of 0x4008f000 stepped;
  - 0x40086024 sat at 0x00008200 the whole time.

  So the page-scan hop is most likely computed by the baseband
  **hardware**, from the BD_ADDR in 0x40086000/04 and CLKN. The firmware's
  mod-79 hop kernel (0x8a83bcac, reached through a pointer) and the other
  `#79` sites would then serve AFH and other special cases. The way to pin
  a channel is the DUT single-frequency mode (LMP_test_control: hopping
  off, fixed RX/TX frequency), which every qualified BR controller has.
  Its handler is the next thing to find.
- **The per-slot commit is 0x8a834ce6.** Every slot it copies a shadow
  struct (0x8a8fc0d8) into the baseband: the sync pair to +0x08/+0x0c, the
  control word to +0x20, the hop/enable word to +0x24 (masked with
  0x40ffff unless a flag in 0x8a8fc110 is set), and a mode word to
  0x40086700+0x20. It is the place to hook a correlator override. The
  dead test-link handler at 0x8a825a5a (about 290 bytes, nothing calls it
  or points to it) is free space for that hook.

### DUT test mode: one fixed channel, and a correlator we can load (2026-10-29)

- **The test mode is a vendor command, 0xFC03.** Its parameter bytes are:

  | bytes | meaning |
  |---|---|
  | 0-1 | 0 |
  | 2 | scenario |
  | 3 | hopping (0 = single frequency) |
  | 4 | TX frequency |
  | 5 | RX frequency |
  | 6 | power |
  | 7 | poll period |
  | 8 | packet type |
  | 9-10 | length |
  | 11-14 | magic 0x00010002 |

  The magic selects the local DUT path (0x8a825a5a, which is not dead
  code after all). It XORs the nine LMP_test_control bytes with 0x55
  itself, as LMP whitens them on air. It then builds a test link on the
  fixed LAP **0xA5F0C3** and applies the control through 0x8a870daa, the
  LMP_test_control handler. The parsed state lives in a struct T at
  0x8a8f18fc: +4 scenario, +8 hopping, +12/+13 TX/RX frequency, +20
  packet type, +24 length. Magic 0x00010000 is the *tester* side instead:
  it sends LMP_test_control to a connection handle. No Command Complete
  reaches a raw socket for 0xFC03, so `dut.py` doesn't wait for one.
- **Run: scenario 5 (loop-back, which only transmits what it receives),
  single frequency, TX = RX = channel 0 (2402 MHz = LE channel 37).**
  - T read back exactly that.
  - The correlator showed `7c1791bb`, i.e. `syncword(0xA5F0C3)`.
  - 0x40086024 isn't a channel field: in test mode it cycles
    0x07/0x17/0x27/0x37 every 50 ms. The RF channel is set somewhere not
    visible in these blocks, so the fixed channel is taken on the
    firmware's word.
- **The correlator can be loaded with anything.** The test link's
  descriptor is at 0x8a904cf0: sync low/high at +12/+16, LAP at +24. The
  scheduler copies it every slot into the shadow at 0x8a8fc0d8, and the
  per-slot commit copies the shadow into 0x40086008/0c. Writing
  `12345678 9abcdef0` into the descriptor (`poke.py`) put exactly that
  pattern in the correlator, on a fixed channel.
- **cp2's RAM is write-back cached.** After the poke, the AP's
  `/dev/mem` view of the descriptor was still the old value while cp2
  already used the new one. So cp2 RAM has to be read through the peek
  primitive (`hcipeek.py`), not `memrd.py`. memrd/memfind are fine for
  locating things.
- **Next:**
  - a "the correlator fired" observable. 0x40086030 is the candidate:
    0x00382602 after a real exchange, 0 in every quiet state.
    `hitprobe.py` compares paging the Mac (guaranteed hits) with paging a
    non-existent address. That run was cut short by the kernel trial.
  - the meaning of the threshold candidate 0x40086060 (09111708). If it
    can be relaxed, an AA-only pattern will see the phones and Macs that
    advertise on channel 37 constantly, which needs neither the scale nor
    a person.
  - after that, the scale's patterns (AA + whitened header for ADV_IND /
    NONCONN / SCAN_IND with TxAdd 0/1 and plausible lengths + AdvA 17 D0,
    whitened for channel 37) and the attended test.
- **Trap, twice:** `kill $(ps | grep "bt bridge")` over SSH matches the
  remote shell's own command line and kills it. Use `pidof bt`.

### The ESP32-C3 as a known transmitter (2026-10-29)

We plugged an ESP32-C3 Super Mini into a Mac. It runs `tools/bt-raw/esp32c3/c3-bt-raw.yaml`:
- ESPHome is used only to build and boot it, with ESP-IDF and Bluedroid.
- `bt_raw_c3.h` takes over the GAP callback, so every byte on air is ours.

It does two things at once:
- **Advertiser:** static random address C3:BE:12:00:00:01, ADV_NONCONN_IND
  (header byte 0x42: type 2, TxAdd 1), length 18, payload
  `02 01 06 | 08 ff ff ff 'BERYL'`, **channel 37 only**, every 100 ms.
  The C3 logs "advertising on" at boot.
- **Scanner (passive):** it logs the raw bytes of any advert that looks
  like the scale (OUI C8:47:8C, "TIMEMORE", or company 0xFFFF), with the
  address type and event type. That settles the scale's real headers
  instead of guessing them.

**On Wi-Fi, driven from the frame (2026-10-29).** We took the Mac
away, so the C3 now runs from any USB power near the frame, and the
experiment is driven over SSH to the frame:
- **Address:** whatever DHCP gives it; mDNS c3-bt-raw.local.
- **Network:** the Super Mini needs `output_power: 8.5dB`; at full power it
  failed to associate (a known antenna problem of that board).
- **Power save:** left at its default. ESP-IDF requires Wi-Fi modem sleep
  when BLE is on, so `none` would abort.
- **Web server (no auth, a lab beacon on the home LAN):**
  - `GET /switch/advertising`
  - `POST /switch/advertising/turn_on|turn_off`: the body must not be
    empty (411 otherwise). From the frame's busybox:
    `wget -q -O- --post-data=x http://<c3>/switch/advertising/turn_off`.
  - `GET /events`: the live log, including the scanner's `scale?` lines.
- **OTA:** password from ESPHome's secrets.yaml.
- The bt-raw scripts take the C3's URL from `C3_URL` (default
  `http://c3-bt-raw.local`).
- **The probe toggles the beacon itself.** `leprobe.py` compares the C3's
  pattern with the beacon on against the same pattern with it off over
  Wi-Fi. A word that moves only in the first case is the hit signal.

`tools/bt-raw/lepattern.py HDR0 LEN ADDR [CH]` gives the correlator
pattern. Its BLE whitening is checked against Dmitry Grinberg's known-good
`BTLEWhiten`. A "reversed(ch)<<1|1, ^0x88" form we wrote first was wrong.

For the C3 on channel 37 the correlator needs `0x40086008 = 8e89bed6`
(the AA itself, because of the LSB-first packing) and
`0x4008600c = a156c0cf`. The scale's ADV_IND/NONCONN candidates come out
as `7140xx8d/8f`; they differ only in the type and length bits.

### bt5 (2026-10-29): first correlation -- the BR receiver hears a BLE advert

Setup, all driven over SSH to the frame (`frame-bt` stopped for the run):
1. the bridge on `wcnmodem-peek.bin`;
2. `dut.py 5 0 0 0 3 17`: single frequency, BR channel 0 = 2402 MHz =
   LE channel 37;
3. `leprobe.py 8 0x8a904cfc`.

The C3 beacon advertises ADV_NONCONN on channel 37 every 100 ms (address
C3:BE:12:00:00:01, length 18), about 0.5 m from the frame. Each condition
ran for 8 s, about 1400 samples:

| correlator | beacon | 0x40086024 distinct | 0x40086028 / 48 / 50 |
|---|---|---|---|
| dummy `12345678 9abcdef0` | on | 4 (the test-mode cycle) | still |
| **C3 `8e89bed6 a156c0cf`** | **on** | **32** | **8000a020 / 0001e000 / 00060000, 00020000 appear** |
| C3 | **off** (over Wi-Fi) | 4 | still |
| C3 with 4 bits flipped (`a157d0de`) | on | 4 | still |
| **C3, again** | **on** | **32** | 0001e000 appears |

**The correlator fires on the C3's advert, only with the right pattern,
only while the beacon transmits, and reproducibly.** That confirms:
- the bit order;
- the whitening (channel 37) and the PDU header encoding;
- the channel: the DUT single-frequency mode really is on 2402 MHz;
- the descriptor route into the correlator;
- that the BR GFSK demodulator takes LE's 1 Mb/s, h = 0.5 modulation.

A 4-bit mismatch is already enough to miss, so the threshold is strict, a
few bits at most. The hit signals:
- **0x40086024** leaves its 4-value cycle; its third byte looks like a
  slot/sequence counter that the RX path advances;
- **0x40086048** bit 0-1 goes `e003` → `e000`;
- **0x40086050** shows `00060000` / `00020000`, likely RX status (sync
  found, header failed);
- **0x40086028** bit 13 sets.

The RX payload buffer (0x40086400) did **not** change. After the sync the
baseband decodes a BR header (FEC 1/3 plus HEC) out of the BLE PDU, the HEC
fails, and nothing is stored.

**Next: getting the bits.** Options, cheapest first:
- a register that latches the raw 18-bit header or the demodulated bits
  around the sync (look at 0x40086030/0x50 and the 0x4008f000 block at the
  moment of a hit);
- a baseband option to ignore HEC, or to treat the packet as a fixed
  test-mode type with whitening off (scenario 7 "ACL without whitening");
- correlating further into the packet, i.e. on the AdvA bytes, to
  confirm the scale's address.

Afterwards `frame-bt` restarted clean: hci0 up, `owns-cp2`.

### bt6 (2026-10-29): the first BLE bytes, read through the BR receiver

**Idea.** After the sync, the baseband reads a 4-bit trailer and a 54-bit
BR header (18 bits, FEC 1/3), and checks its 8-bit HEC against a seed, the
UAP. For any 54 bits that come in, exactly one UAP makes the HEC pass. And
the C3's packet is known exactly. So `brview.py` simulates, for every
window offset k into the packet:
- the 64 correlator bits;
- the header the baseband will decode;
- the UAP that passes.

The pieces that made it work:
- **The UAP can be set.** The link-control word 0x40086020 is
  `UAP << 24 | ... | LT_ADDR`. It read 0x00800841 in test mode and
  0x64800002 after paging as master (0x64 is our own UAP). It comes from
  the test link's descriptor: the top byte of +24 (0x8a904d08), the word
  that also holds the LAP 0xA5F0C3. Writing 0x4B there gave 0x4B800841
  in hardware within a slot.
- **The window doesn't need a real preamble in front of it.** At k=7 (7
  bits into the AA) the correlator still fires, only with the beacon on.
- **k=7** decodes as LT_ADDR 1 (the test link's) and type 11 (DH3, no
  FEC). Test scenario 7 is ACL without whitening, so nothing de-whitens
  the payload.
- **The UAP was found by sweep, not by calculation** (`uapsweep.py`, 0.7 s
  per value, beacon on). At **UAP 210 = 0xD2** two things happened:
  - 0x40086050 showed new statuses (`04017686`, `02000086`, i.e. header
    accepted);
  - the RX buffer at 0x40086400 filled and kept its contents.

  0xD2 is our computed 0x4B bit-reversed, so the hardware seeds the HEC the
  other way round from our `hec()`. `brview.py` now prints the hardware
  value.
- **What's in the buffer:** `83d0815d790e0817 88...`. That is the C3's
  on-air bits, starting 16 bits after the BR header (after the DH3's
  2-byte payload header). 70 of 71 bits match the simulation up to the end
  of the packet; noise follows, and differs between two captures.
- **Dewhitened** for channel 37 (`capdecode.py 7 <hex>`), PDU bytes 15-18
  read **`BERY`**. Byte 19 should be `L` (0x4c) and came back 0xcc: one
  bit error.

**So the VT10101's built-in BR radio receives BLE payload bits.** The
correlator matches on a known prefix (AA plus about 32 more bits). The
next 74 bits are spent on the BR trailer and header, which have to come
out as an ACL type with LT_ADDR 1 and a HEC that passes with some UAP.
After that, raw bits are written to 0x40086400 until the packet ends.
Software then dewhitens them.

The constraints, for the scale:
- the prefix and the "header" bits have to be constant: AA, PDU header,
  AdvA, the flags and the start of the manufacturer data are;
- the useful payload (weight) has to come after them;
- one UAP per device and packet layout, found once by sweep;
- one channel at a time (37 now).

Next:
- the scale's real advert, from the C3's scanner when the scale is awake;
- choosing k for the scale so the header decodes as ACL on LT_ADDR 1;
- a UAP sweep with the scale awake, or computed with the corrected bit
  order;
- then a frame-side loop: watch for a new RX buffer, dewhiten, parse,
  publish.

The frame ran Classic again afterwards (`frame-bt` restarted, hci0 up).

### bt7 (2026-10-29): the scale's real advert -- and why receive-only stops here

We put the scale, awake, next to the C3. Its scanner logged a dozen
identical adverts in 30 s:

    xx:xx:xx:xx:xx:xx public, ADV_IND, rssi -45..-67
    02 01 05 | 03 03 f0 ff | 05 ff ff ff 01 01      (13 bytes, always the same)

What this shows:
- **The address is a public one,** not in the OUI the scale's vendor
  registered.
- **The advert carries no weight.** It is flags, service FFF0 and
  manufacturer data `ffff 01 01`, constant. The name `TIMEMORE_Dot` is in
  the scan response.
- **Weight only flows over a connection:** a central connects, bonds
  with Just Works encryption (subscribing to `FFF1` without it fails with
  ATT 0x0F, Insufficient Encryption), enables notifications on `FFF1`,
  and gets about 10 Hz of weight frames.

**So the raw-radio receiver (bt4-bt6) can't read the scale.** Passive
reception works and is proven, but this scale has nothing in its adverts
to receive. A connection would need the following, and the BR baseband
does none of it:
- the LE link layer itself: the connect request, the connection-event
  timing with T_IFS = 150 µs turnarounds, channel selection and
  hopping, and ack/flow control;
- transmitting LE-formatted packets;
- SMP pairing;
- AES-CCM encryption with a MIC on every PDU. The baseband's hardware
  cipher is BR's E0.

That is a BLE controller written from scratch on undocumented hardware: a
real wall, not an effort one.

What the raw receiver is still good for: listening to LE devices that
*broadcast* their data in adverts (many sensors, beacons, some scales and
thermometers), each with a known prefix, one channel at a time.

The ways to give the frame the scale (a choice to make):
1. **An ESP32 as the scale's central, relaying over Wi-Fi.** The C3
   already sits next to the scale. It can connect, bond, subscribe to
   `FFF1` and forward the weight frames (UDP or HTTP) to the frame. Hours of work. The scale's
   single-host lock means it has to be released from its paired phone,
   or put in pairing mode (hold the power button).
2. **A USB BLE dongle (btusb is in kernel #46)** on a hub, while the
   micro-USB port still carries the Wi-Fi dongle.

**What the next step needs: a BLE transmitter we control.** The correlator
needs the exact 64 bits that follow the preamble. Those are constant only
for an advertiser with a fixed address and payload. Phones and the Mac
rotate their random addresses, and the frame has no second radio to learn
them with. The cheapest candidate is our ESP32s: an ESPHome
`esp32_ble_beacon` advertises a fixed public address and payload on all
three channels. With that:
1. the expected 64 bits on channel 37 come from the AA plus the whitened
   header and AdvA;
2. a firmware copy loads them into 0x40086008/0c, while a page-scan or
   inquiry-scan window holds the radio in RX;
3. the cp2 RAM state is diffed for a sync hit;
4. then comes the search for where bits land after a match.

## bt8 (2026-10-30): Bluetooth in the kernel, beside the Wi-Fi

the kernel WCN driver (docs/wifi.md wifi6-wifi8) took cp2 and the frame's
only network with it, so the user-space bridge stood down and there was no hci0.
`kernel/bt-sc7731g.c` puts the Bluetooth back as an hci device of its own, on
the same core, the same cp2 and the same sbuf ring 10 the bridge used:

- **Transport:** H4 both ways on ring 10. cp2's notify (from its interrupt
  thread, no sleeping) queues rx work on an ordered workqueue; tx is queued
  too, since `wcn_sbuf_write` can wait for room.
- **PSKEY from user space:** it is still built at boot from Android's ini and
  the serial (`pskey.sh`), now written to the device's `pskey` sysfs
  attribute (`/sys/bus/platform/drivers/bt-sc7731g/*/pskey`). The first write
  registers hci0, so no hci device exists that would fail its first power-on.
  `rootfs/etc/init.d/sc7731g-bt` does the write.
- **Once per cp2 start:** the core counts starts (`wcn_boot_count`, its one
  addition). hci0's open sends the PSKEY only if cp2 has started since the last
  one. Reopened on a cp2 the Wi-Fi kept running, it doesn't; on a fresh cp2 it
  does.

Trials #4 and #5 (the Bluetooth trial builds' own numbering, on top of
kernel #62; kernel-trial, recovery p21), tools in `tools/bt-kernel/`:

| test | result |
|---|---|
| touch, input, cage, the UI's health check, sound, wlan0 up, a download | pass |
| PSKEY taken (`05 08 08 08`), hci0 answers with its derived address | pass |
| wlan0 down/up with hci0 open (DEV_OPEN on a cp2 that never stopped) | pass, gateway in 2 s, SBLOCK_INIT re-handshakes |
| hci0 off/on with wlan0 up | pass, no PSKEY resent, Wi-Fi unaffected |
| cold start by BT (PSKEY on cp2 start 2), BT alone +40 s, then wlan0 up | pass, gateway in 2 s |
| BT alone (wlan0 down, or up and `wpa_cli disconnect`): inquiry, page | inquiry 6.4 s, page timeout 6.4 s: every time |
| wlan0 associated: inquiry | about 1 in 4 completes (6.4 s); the rest never send Inquiry Complete until cancelled |
| wlan0 associated: page | 0/3 one run, 4/4 (Page Timeout after 11.2 s) another |
| Wi-Fi while BT inquires/pages back to back | 30% ping loss, spikes to 1 s; with BT idle, clean |

`wifi.md` wifi7's deaf trap (cp2 goes deaf 10-20 s after DEV_OPEN without a
scan) didn't show on a BT-only cp2: no DEV_OPEN is ever sent there, and BT
answered at +30 s and +40 s.

### Coexistence

While the station is associated, cp2 gives the BT link controller airtime only
some of the time, and when it doesn't, BT's timers don't run at all: an
inquiry that starts without airtime never ends. Ruled out:
- **The Wi-Fi driver:** the vendor's sprdwl.ko has no coex command at all
  (every id was mapped). Its only power commands, ids 32/33 (early
  suspend/late resume, now `iw ... set power_save on/off`), change nothing
  here.
- **Our settings:** the PSKEY and the Wi-Fi NVM carry Android's own values
  (antenna switch fields wifi_tx/bt_tx 4, wifi_rx/bt_rx 1, gpio_cfg 3).
- **sprdwl's "unknown event id=14":** it is the vendor's CQM report (ids 12-14
  all go to `sprdwl_event_report_cqm`), a symptom, nothing to answer. It's
  dev_dbg now.

cp2's firmware has `wifi_bt_coexist_enable` and `wifi_bt_coex_send_start/
stop` (their `__func__` names at 0x8a8615c8 and 0x8a861570; send_stop is the
function at 0x8a86deda), so the arbitration is there to be found.
We also don't know whether Android ever did better.

Later reading of the firmware (not yet acted on): cp2 asks the Wi-Fi for
airtime only when Bluetooth has ACL data backed up. The coex flag
(0x8a8c8fb5) is raised from just two baseband sites, both gated on buffer
counts, so inquiry and paging, which carry no data, run only in the gaps
an associated Wi-Fi leaves: the 1 in 4. No AT command, WIFI_CMD or
PSKEY/NVM field reaches it, so the fix is a firmware patch. That patch
would be applied the Nexmon way, to the firmware extracted from your own
frame (`firmware/patches/`, docs/firmware.md); it doesn't exist yet.

**Promoted:** #5 has been on p16 since 2026-10-30,
after a plain reboot came back green on it. It is #62 plus this driver,
plus the Wi-Fi driver's change (CQM silenced, set_power_mgmt). Until user
space writes the PSKEY no hci0 exists, so it behaves like #62.

**What it means today:** BLE software scanning with `Transport: le` is
refused by this adapter, so it doesn't start BR/EDR inquiries that cost the
Wi-Fi.
Pairing a Classic device while associated works only some of the time.

## What this means

- **BLE can't run on the built-in radio with this
  firmware build** (bt3: no LE dispatch and no LE link layer). The
  options:
  1. **A USB BLE dongle with a mainline driver** (btusb). The micro-USB port
     is the Wi-Fi dongle's until native Wi-Fi works; until then a hub is
     needed. This is the one that gets the scale working.
  2. A newer SC7731G WCN firmware with a 4.x controller, taken from another
     device. Nobody has looked for one yet. It would also replace the Wi-Fi
     firmware that `docs/wifi.md` proved.
- **Classic Bluetooth works** if it's ever wanted, for example for A2DP to
  a speaker or for SPP. The Linux side would be an `hci_dev` over SIPC ring
  10. That needs an in-kernel SIPC core exposing sbuf ch 4 ring 10, with
  the PSKEY sent from `setup()` and loaded with `request_firmware`. That
  core doesn't exist yet and nobody is writing it at the moment (as of
  2026-10-29); its protocol is in `docs/wifi.md` and `initramfs/cp2.c`. That's a small driver, shaped like btsdio, but it
  isn't worth writing for BLE.
