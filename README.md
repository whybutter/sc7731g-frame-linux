# sc7731g-frame-linux

Mainline Linux for Spreadtrum (Unisoc) SC7731G digital photo frames, starting
with the VTimes VT10101: a 6.18 kernel with new drivers for the SoC (clocks,
timer, I2C, eMMC, display takeover, PWM, USB PHY, PMIC regulators and
watchdog, audio), a native Wi-Fi and Bluetooth driver for the SoC's WCN
co-processor, and the boot tooling to install it next to stock Android with
A/B root slots and a trial slot for kernels. The vendor U-Boot is never
touched, and stock Android stays one reboot away in the recovery partition.

This is the sibling of
[vphoto-frame-linux](https://github.com/whybutter/vphoto-frame-linux), the same
idea for Allwinner A33 frames. The two share a userland (an armv7 Alpine
root); this repository is the board side: kernel, initramfs, boot images,
firmware extraction and the board's own services.

How it got here, every wrong turn included, is in `docs/`. This file is the
short version.

## Tested hardware

One VTimes VT10101 (10.1" 1280x800, Android 6.0 stock). `docs/bring-up.md`
has the full hardware table; in short:

| | |
|---|---|
| SoC | SC7731G (SC8830 family), 4x Cortex-A7 1.2 GHz, Mali-400 MP2, 2 GiB |
| PMIC | SC2713S |
| Panel | Fitipower JD9365 MIPI-DSI, 800x1280 native, mounted landscape |
| Touch | Silead GSL1680 on i2c1 |
| Accelerometer | MiraMEMS DA213 on i2c2 |
| Wi-Fi / BT | the SoC's WCN co-processor (cp2) + SR2351 RF: 2.4 GHz b/g/n, Bluetooth 2.1 + EDR |
| Sound | one speaker, codec HPR + external amp |

The board (`si006asb`, `sp7731g_1h10`) is a Spreadtrum reference design sold
under several brands; other SC7731G frames are likely close. See "Other
models".

## What works

- **Boot** from the eMMC through the vendor's own U-Boot/cboot: our kernel in
  the `boot` partition, Android in `recovery`, the root filesystem in A/B
  slots carved out of `userdata`. The initramfs picks the slot, counts trial
  boots and falls back (`initramfs/frameboot`).
- **Kernel trials without a serial cable**: `tools/kernel-trial.sh` boots a
  new kernel once from the recovery partition and promotes it only when you
  say so; a kernel that doesn't come up costs a power cycle.
- **Display**: the panel as U-Boot lit it, taken over by a small DRM driver
  (`fbcon`, a Wayland compositor on top); PWM backlight 0-255.
- **GPU**: Mali-400 MP2 through mainline lima.
- **Touch**: GSL1680 through mainline `silead`, fitted to the glass.
- **Wi-Fi, native**: `wlan0` from our WCN driver (`kernel/wcn-sc7731g.c` +
  `kernel/sprdwl-sc7731g.c`, cfg80211 full-MAC): scan, WPA2-PSK through
  wpa_supplicant, DHCP, a few MB/s, an eight-hour soak without leaving the AP.
- **Bluetooth Classic**: `hci0` from `kernel/bt-sc7731g.c`, on the same
  co-processor as the Wi-Fi; BlueZ powers it, inquiry, paging and remote-name
  work.
- **Sound**: the speaker as ALSA card `VT10101` (VBC + SC2713S codec + the
  AP's DMA), with a volume control.
- **USB networking**: the micro-USB port as a CDC-ECM gadget (`usb0`, the
  frame at `192.168.7.2`), the way in when Wi-Fi is down. Host mode for a USB
  dongle works too.
- **Watchdog, restart, crash logs**: the PMIC watchdog as `/dev/watchdog`,
  restart and `reboot recovery` through the PMIC, and pstore/ramoops keeping
  the last kernel log across a panic.
- **SMP**: all four cores.

## What doesn't

- **BLE, on the built-in radio.** The WCN firmware is a Bluetooth 2.1
  controller with no LE in it at all (docs/bluetooth.md, bt1 and bt3). We
  made its BR receiver hear BLE adverts with a patched firmware copy (bt4-bt6),
  but connections, and anything needing LE transmit, are out of reach. The
  plan for BLE is an ESP32-C3 on the UART pads as an HCI controller
  (`c3-hci/`, docs/ble-c3.md): firmware written, not yet fitted.
- **Wi-Fi / Bluetooth coexistence.** While Wi-Fi is associated, cp2 gives
  Bluetooth airtime only when it has ACL data queued: about 1 in 4
  inquiries completes, paging is unreliable, and Bluetooth traffic costs
  the Wi-Fi up to 30% packet loss. Idle Bluetooth leaves Wi-Fi clean. A
  firmware patch is the fix; it isn't written yet (docs/bluetooth.md,
  "Coexistence").
- **No DSI host driver**: the display depends on U-Boot having lit the
  panel. Fine for a frame; no panel power-off or mode changes.
- **`nohlt`**: with the cores idling in WFI, the eMMC controller stalls, so
  the kernel runs with `nohlt` (more idle power) until the clock gate behind
  it is found.
- No 5 GHz (the radio is 2.4 GHz only), no Wi-Fi tx bitrate report, no
  suspend, no microphone (none fitted), no thermal sensor driver.
- The kernel has no modules: everything is built in, and the boot partition
  is 16 MiB (about 15 MiB used).

## Building

Everything builds in Docker; nothing is installed on your machine. Sources
and object trees live in a Docker volume (`sc7731g-frame-linux-cache`),
outputs in `out/`.

```sh
./build.sh firmware backup/   # vendor firmware from YOUR frame (below)
./build.sh kernel             # out/zImage, out/uImage, out/sc7731g-vtimes-vt10101.dtb
./build.sh bootimg            # out/boot-linux.img, what goes in the boot partition
```

`./build.sh` with no argument lists the steps. The kernel is Linux 6.18 LTS
(`build/versions.env`); `build/kernel.sh` copies our drivers into the
unpacked tree, hooks them into Kconfig, applies `kernel/patches/`, and
builds the initramfs (static busybox from Alpine, `initramfs/`) into the
kernel.

### The firmware

The frame needs vendor firmware that only the vendor ships: the WCN
co-processor's image, the Wi-Fi calibration, the touch controller's
firmware. **None of it is in this repository** (see NOTICE). The `firmware`
step cuts it out of dumps of **your own frame's** partitions, in `backup/`:
`boot.img`, `wcnmodem.img` and `system.img`, checks every input and output
against `firmware/SHA256SUMS`, and applies any patches in `firmware/patches/`.
The Bluetooth PSKEY is built on the frame at each boot from its own Android
files. Details, and how to dump the partitions: `docs/firmware.md`.

## Installing

Read `docs/install-plan.md` first: it is how this was done on our unit, step
by step, with what each step changes and how to undo it. In outline:

1. **Get root and back up everything.** UART pads on the back, `su`, adb
   (`docs/bring-up.md`). Back up every partition and both GPT copies. The
   recovery image you back up is your way back to Android.
2. **Try the kernel from recovery.** Write `out/boot-linux.img` to the
   `recovery` partition from Android (`dd` as root), `adb reboot recovery`.
   Android's normal boot is untouched; a power cycle goes back to it.
3. **Swap.** Our image into `boot`, Android's original `boot.img` into
   `recovery`. Power-on is now Linux; a `reboot(2)` with RESTART2
   "recovery" (`initramfs/reboot-to.c`) boots Android once.
4. **Partition.** `tools/gpt-split.py` rewrites the GPT's tail only:
   `userdata` shrinks to 1 GiB (Android in recovery still has a /data), and
   `fl-boot` (64 MiB FAT, label `FRAMEBOOT`), `fl-a` and `fl-b` (2 GiB root
   slots) and `fl-data` follow. p1-p22 don't move.
5. **A root filesystem** in `fl-a`, and `update/slot.env` containing
   `slot=a` on FRAMEBOOT. Any armv7 root with `/sbin/init` boots; see
   "The userland".
6. **Wi-Fi**: `wpa_supplicant.conf` on FRAMEBOOT, as on the A33 frames.

Kernel updates afterwards: `tools/kernel-trial.sh root@<frame> try
out/boot-linux.img`, check the frame, then `... promote`.
`tools/bt-kernel/green.sh` is the checklist we run before promoting.

Until it is installed, `tools/uboot-boot.py` boots a kernel without writing
anything: it stages it in Android's cache partition over adb, catches the
U-Boot prompt on the UART and `bootm`s it.

## The userland

The frame's root filesystem is not in this repository. We run the armv7
Alpine userland built for the A33 frames; the build for those
([vphoto-frame-linux](https://github.com/whybutter/vphoto-frame-linux)) is
the public starting point. What this board adds on top is in `rootfs/`:

- `etc/init.d/sc7731g-bt`, `usr/lib/sc7731g-bt/pskey.sh`: builds the
  Bluetooth PSKEY from the frame's own Android at boot and hands it to the
  kernel (needs python3 and `tools/bt-pskey.py` as
  `/usr/lib/sc7731g-bt/bt-pskey.py`);
- `etc/init.d/sc7731g-bt-ext`: the ESP32-C3 BLE controller, once fitted;
- `etc/conf.d/sc7731g`: their settings;
- `etc/asound/`: the ALSA default for the mono speaker and its mixer state.

What the root needs from the boot: FRAMEBOOT is mounted by `LABEL=FRAMEBOOT`
at `/boot`; the slot is in `/etc/frame-slot.env` and on the command line
(`frame.slot=`, `frame.trial=`), and the userland commits a trial by
deleting `update/trial.env` and writing `update/slot.env`.
`FRAMEBOOT/frame-fix.sh`, if present, runs inside the new root before its
init. Board facts for the userland: backlight
`/sys/class/backlight/backlight` (0-255), output rotated 270° for the
landscape hold (touch comes in already landscape), the accelerometer's X
axis reads +g with the frame turned clockwise.

## Getting in

- `ssh root@192.168.7.2` over the USB cable (or its IPv6 link-local
  address), or over Wi-Fi, as your userland allows.
- The initramfs has a debug path: `FRAMEBOOT/noframeboot` stops the boot in
  the initramfs; `usbnet` on the command line gives a shell on usb0
  (`nc 192.168.7.2 23`).
- The UART console is off in the installed kernel (the pads are reserved for
  the C3); a debug boot through `tools/uboot-cmd.py` brings it back.

## Repository layout

```
build.sh, build/     the Docker build: kernel.sh, the toolchain, pinned versions
kernel/              our drivers, the SoC dtsi and board dts, config fragment,
                     patches to mainline (sprd-adi, sprd-dma)
initramfs/           /init, frameboot (A/B slots), pmic-wdt, probe, the cp2
                     and bt user-space tools, small static helpers
firmware/            extract.sh, SHA256SUMS, patches/ -- the blobs stay out
rootfs/              the board's services for the userland
tools/               boot images (mkbootimg-sprd, unbootimg), GPT split,
                     kernel-trial, U-Boot helpers, firmware extractors,
                     the PSKEY and NVM builders, audio-poke, clocks2dtsi;
                     bt-kernel/ (checks), bt-raw/ (the raw-radio LE research)
c3-hci/              ESP32-C3 firmware: an HCI-over-UART BLE controller
docs/                bring-up, mainline log, install, firmware, Wi-Fi, WCN
                     protocol, Bluetooth, BLE via C3, audio
```

## Other models

Spreadtrum's SC7731G/SC8830 reference boards are in many frames and cheap
tablets. If you have one, an issue with these helps tell whether this port
fits it:

- brand, model and the Android build (`getprop ro.product.model`,
  `ro.product.board`, `ro.build.fingerprint`);
- the vendor DT (the boot image's `dt` section: `tools/unbootimg.py`) and
  the kernel command line (`lcd_id=` names the panel);
- `firmware/extract.sh`'s checksums for your dumps.

The panel is the easy part here (U-Boot lights it); the board's regulators,
touch wiring and the WCN's calibration are where frames differ.

## Licensing

- `kernel/` drivers and patches: GPL-2.0 (each file's SPDX header). The
  device trees: GPL-2.0+ OR MIT.
- `initramfs/*.c`: GPL-2.0-only.
- `c3-hci/`: Apache-2.0 (`c3-hci/LICENSE`).
- Everything else written for this project: MIT (`LICENSE`).
- No vendor firmware, ever: `NOTICE`.

Built by contributors with one frame on the bench and a lot of patience.
