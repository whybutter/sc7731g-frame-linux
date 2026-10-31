# Installing Linux on the VT10101: the plan, and how it went

Status: plan, nothing written to the frame yet (2026-10-27). Each phase says
what it changes, how it's checked, and how it's undone. Nothing here touches
U-Boot, SPL or the eMMC boot areas: below them there is only the boot ROM's
download mode (1782:4d00 + spd_dump), and the plan never needs them.

## What the vendor boot path gives us

- U-Boot's `bootcmd=cboot` reads the `boot` partition (p16, 16 MiB) as an
  Android boot image: header `ANDROID!`, page 2048, an uncompressed ARM
  `Image` (not a zImage), a ramdisk, and in the header's dt slot a Spreadtrum
  DT table (`SPRD`, version 1, one entry, platform id 0x227e, blob at 0x800).
  Header cmdline: `console=ttyS1,115200n8`.
- cboot then edits the DT: it requires a `/memory` node and rewrites it, and
  appends to `/chosen/bootargs` (Android got `lcd_id=ID936500 lcd_base=9fdfc000
  mem_cs=2, mem_cs0_sz=40000000 androidboot.*`).
- It boots `recovery` (p21, 18 MiB) instead when the misc partition (p22)
  holds `boot-recovery`, or when the PMIC reset reason says recovery (0x20).
  Our restart handler writes that reason for `reboot recovery`. Android's
  `adb reboot recovery` does the same through the vendor kernel.

So no U-Boot change is needed: our kernel goes into the boot image format,
and recovery is the way back to Android.

## Phase 1 — a boot image, tried from the recovery partition

Build `boot-linux.img`: our `Image`, the initramfs as its ramdisk, our DTB
wrapped in a one-entry SPRD table with the vendor's ids, and the header
cmdline `console=ttyS1,115200n8 console=tty1 fbcon=rotate:1 panic=5`.
(`tools/mkbootimg-sprd.py`, to write; `tools/unbootimg.py` reads the vendor
one for comparison.)

Trial without touching the normal boot: write it to **recovery** (p21) from
Android (`adb shell su -c dd`), then `adb reboot recovery`. Watch the UART.

- Checks: our kernel comes up from cboot with no `uboot-boot.py`; read
  `/proc/cmdline` and `/proc/device-tree/memory` to learn what cboot wrote
  (does it give 2 GiB?); then `reboot recovery` from our kernel should land
  in our kernel again, which proves the reset-reason route works.
- Undo: `dd` the backed-up `recovery.img` back (or let it be: Android boots
  normally from `boot` throughout).
- Risk: cboot rejects the image or edits the DT into something our kernel
  can't boot. The normal boot is untouched, so Android just boots.

## Phase 2 — swap: Linux in boot, Android in recovery

Write `boot-linux.img` to **boot** and the vendor `boot.img` to **recovery**.

- Power-on → our kernel. `reboot recovery` from Linux → stock Android, with
  the UART still there as the last resort.
- Undo: write the backed-up `boot.img` to boot and `recovery.img` to recovery.

## Phase 3 — storage for the rootfs

frame-linux on the A33 has a FAT config partition (`FRAMEBOOT`: the
overlay/fix hook, `update/slot.env`) and two ext4 root slots for A/B updates
(the A33 repository, vphoto-frame-linux, describes it). The same shape here:

| | size | |
|---|---|---|
| userdata (p23, today 13.1 GiB) | shrink to 1 GiB | Android's /data, so recovery-Android still boots |
| fl-boot (new) | 64 MiB, FAT | the FRAMEBOOT equivalent |
| fl-a, fl-b (new) | 2 GiB each | root slots |
| fl-data (new) | the rest, ~8 GiB | persistent state (/var/lib, photos cache, ...) |

Done with a GPT rewrite. Of everything here, that's the step to be careful
with:
- **Back up first:** the backup has the primary GPT (in
  `mmcblk0.head36M.img`), but the secondary GPT image is empty, so read the
  last 33 sectors before anything.
- **Only the tail changes:** only p23 changes and new entries are added
  after it, so every partition U-Boot or Android reads stays where it is.
- **Format userdata afterwards:** the new 1 GiB userdata is formatted ext4,
  leaving Android's 16 KiB crypto footer free.

- Undo: write the backed-up GPT (both copies) back. Android user data is
  lost either way. It wasn't in the backup, and we accepted wiping it.
- **Alternative, not chosen:** the SD card slot. It
  would leave the eMMC alone, but it needs a second SDHCI instance we haven't brought up.

## Phase 4 — the initramfs as the boot script

The A33's `boot.scr` work moves into the initramfs, because cboot can't run
scripts:
- Mount fl-boot and read `update/slot.env`; trial and rollback as on the
  A33 frame.
- Run the FRAMEBOOT hook (`frame-fix.sh`, the overlay tarball).
- `switch_root` into fl-a or fl-b.

The kernel lives in the boot partition, not per slot, at first. Updates to
it are a separate, rarer step (tools/kernel-trial.sh).

## Phase 5 — frame-linux on it

- **rootfs:** the armv7 rootfs.ext4 built for the A33 frame into fl-a.
- **Board config for the userland:**
  - radar: GPIO 217 is sysfs number 729 (chip base 512, pad IIS1CLK). The A33's userland reads its PIR on 204, and Android exposes the radar as an input device (`zed-radar`), so check which interface the hardware gives.
  - backlight: `/sys/class/backlight/backlight`, 0–255.
  - output: rotated for the landscape hold (270° in the end; touch comes in
    already landscape, docs/mainline.md).
- **Kernel modules:** the kernel is built with everything in (`=y`); keep it
  that way until something needs a module.

## Open decisions (at the time)

1. **Network.** The frame needs a network for its UI.
   The frame's Wi-Fi is Spreadtrum's integrated WCN, with no
   mainline driver, and porting it is a big job.
   - **USB Wi-Fi dongle:** a mainline-supported one on the micro-USB port in
     OTG host mode. Possible only if the frame isn't powered through that
     same port.
   - **USB networking to a nearby host:** what we use today.
   - **Port the WCN driver:** weeks of work.
   How is the frame powered: a barrel jack, or the micro-USB?
   (2026-10-27: a barrel jack, so the micro-USB is free for a dongle. The
   Wi-Fi has its own track now: `docs/wifi.md` has the assessment of both
   routes and the first cp2 test.)
2. **Storage:** eMMC split (recommended above) or the SD card.
3. **2 GiB of RAM:** Phase 1 shows whether cboot hands over both banks.
   If not, `mem=` on our command line.

## Done so far

- **Phase 1** (2026-10-27): cboot booted our zImage boot image from recovery
  and wrote 2 GiB into `/memory`. It ignores the header cmdline, so ours now
  lives in the DT's `/chosen/bootargs`, and cboot appends its own after it.
- **Phase 2** (2026-10-27): our image is in `boot` and the vendor `boot.img`
  in `recovery`, both checked by readback. Power-on boots our kernel;
  `reboot-to recovery` (initramfs/reboot-to.c, reboot(2) RESTART2) boots
  stock Android once, and the next restart is Linux again. Cold boot verified
  after our power cycle.
- One unexplained power-off: the warm restart right after the Wi-Fi work's
  cp2 start/stop test left the frame dark until a power cycle. Every other
  restart has come back by itself (docs/wifi.md, wifi1).

## Phase 3/4, prepared (2026-10-27, nothing written yet)

- **GPT:**
  - Both copies were read live: the primary matches the backup, the
    secondary is new. Both went into the backup (`gpt-primary-34s.img`,
    `gpt-secondary-last33s.img`, with md5s).
  - `tools/gpt-split.py` builds the new table: p1-p22 untouched, p23
    userdata to 1 GiB, then fl-boot 64 MiB, fl-a 2 GiB, fl-b 2 GiB and
    fl-data (~8 GiB). Partition GUIDs are fixed: `5654544d-0000-4000-8000-0000000000NN`.
  - `sfdisk --verify` passes on the result.
  - Undo is the backed-up copies, written to LBA 0 and to LBA 30777311.
- **The write**, from our own Linux:
  - `dd` the two images.
  - Re-read the table: busybox `blockdev --rereadpt`, or a reboot.
  - `mkfs.vfat -n FRAMEBOOT` on fl-boot. The rootfs's fstab mounts
    `LABEL=FRAMEBOOT` at `/boot`.
  - `dd` the A33 build's `rootfs.ext4` (1.5 GiB) into fl-a over the USB
    network.
  - `update/slot.env` gets `slot=a`.
  - fl-data stays unformatted until the userland has a use for it.
- **Boot:** `initramfs/frameboot` is the A33 boot script's slot and trial
  logic plus the FRAMEBOOT hook. It also provides `frame.slot=`,
  `frame.trial=` and `root=` through a `/proc/cmdline` bind, since this
  kernel's command line is fixed by cboot. It's switched on by `frameboot`
  in the DT's bootargs. Every failure falls back to the initramfs shell.
- **Before any of it:** the eMMC warm-reset fix (RST_n pulse) has to pass
  its test. The userland would read and write the eMMC on every boot.
- **Later, userland side:** a small helper that reads the slot from a
  file instead of `/proc/cmdline` would make the bind unnecessary.

## Phase 3 done (2026-10-27)

All written from our own Linux (with `nohlt`, eMMC writes are reliable):
- **GPT:** the new table (tools/gpt-split.py) went to LBA 0 and to the
  last 33 LBAs, read back by md5, and was picked up live with `blockdev
  --rereadpt`. p23 userdata is 1 GiB; p24 fl-boot, p25 fl-a, p26 fl-b and
  p27 fl-data are present, with their names.
- **userdata (Android's /data, for the recovery fallback):**
  - ext4, 1 GiB minus the 16 KiB crypto footer its fstab wants
    (`encryptable=footer`).
  - Built in Docker with the old feature set Android 6's e2fsck knows.
  - Streamed over the USB network: gunzip, then dd.
- **fl-boot:** `mkfs.vfat -n FRAMEBOOT`, holding `update/slot.env` =
  `slot=a`.
- **fl-a:** a rootfs image, its sha256 checked after decompressing. It was written at about 10 MB/s and read back from the
  partition with the same sha256.
- **fl-b and fl-data:** untouched (unformatted).
