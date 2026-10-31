# The VT10101: the hardware, and getting in

What the frame is, how root was had on a stock unit, and the boot chain
Linux has to live with. The kernel bring-up log, driver by driver, is
`docs/mainline.md`; the install, `docs/install-plan.md`.

## The hardware (one unit, 2026-10)

| | |
|---|---|
| Frame | VTimes VT10101, 10.1" 1280x800 touch, Android 6.0 stock (firmware `VTimes_VT10101_V01_20250617`, security patch 2017-04-05) |
| Platform | `si006asb` / `sp7731g_1h10`: a Spreadtrum reference board sold under several brands (the "SI006" Uhale frame and the VTimes VT01001 look like the same board) |
| SoC | Spreadtrum (Unisoc) **SC7731G**, SC8830 family: 4x Cortex-A7 at 1.2 GHz, Mali-400 MP2 |
| RAM | 2 GiB in two chip-selects (the vendor U-Boot sizes only the first and says 512 MiB; cboot hands Linux both) |
| eMMC | ~16 GB, GPT with 23 partitions, plus two 4 MiB boot areas |
| PMIC | SC2713S (audio codec, LDOs, watchdog), reached over the SoC's ADI bus |
| Panel | Fitipower **JD9365** MIPI-DSI, 800x1280 native (portrait scan, mounted landscape); vendor `lcd_id=ID936500` |
| Touch | Silead GSL1680 (gslX680 family) on i2c1 0x40 |
| Accelerometer | MiraMEMS DA213 on i2c2 0x27 (mainline `da280` binds it) |
| Wi-Fi / BT | the SoC's own **WCN** co-processor ("cp2") with an SR2351 RF transceiver: 802.11 b/g/n 2.4 GHz and Bluetooth 2.1 + EDR (no LE) |
| Sound | one speaker, through the codec's headphone-right output and an external amplifier (enable on GPIO 85) |
| USB | one micro-USB port, DWC2 OTG (gadget by default; host mode with the board's 5 V boost on GPIO 69) |
| Power | a barrel jack; no battery fitted |
| Not fitted | the presence sensor (an empty IR-IN / GND / V-IR footprint next to the UART pads; GPIO 217) and a microphone |

Android's `build.prop` and the vendor kernel's `/proc` claim a Realtek
RTL8723 for Wi-Fi; that is wrong. The `sprdwl` module is Spreadtrum's
driver for the integrated WCN (docs/wifi.md).

## Getting in

Every software route we know of failed on this unit: the adb RSA dialog is
suppressed, no keyboard shortcut opens an app (keyboard injection over USB
AOA works but reaches nothing), the hidden taps in About do nothing, the
Wi-Fi setup is the kiosk's own, an SD card raises no file browser, and the
kiosk's own picker refuses anything but images and video.

**The way in is the UART.** On the back of the board, bottom left on the
solder side, three silkscreen-labelled pads: `GND`, `TX`, `RX`. 3.3 V TTL,
115200 8N1; adapter RX to the board's TX, adapter TX to the board's RX,
grounds together, and **no VCC**: the frame has its own supply. IC-grabber
clips hold through a whole boot; nothing needs soldering.

- The vendor U-Boot (2014.01) prints there and stops autoboot on any key
  (bootdelay is 0, so a key held while powering on).
- Android ends its boot in a console shell, `shell@sp7731g_1h10:/ $`
  (uid 2000), and `su -c id` answers root at once, without a prompt.
- For adb as root: append your computer's adb public key to
  `/system/etc/security/adb_keys` (keep the factory key in it), restoring
  the file's owner and SELinux label (`u:object_r:system_file:s0`) after a
  `mount -o remount,rw /system`. The image's `daemonsu` remounts /system
  every few seconds; `stop daemonsu` first. init copies the file to
  `/data/misc/adb/adb_keys` at every boot, so the key persists, and adbd
  runs as root.

From opening the case to root adb took about half an hour.

Before anything else, **back up every partition** (docs/firmware.md, step 1)
and both GPT copies. The vendor's Adups update agents are enabled on stock
Android and talk plain HTTP; keep Android off Wi-Fi.

## The boot chain

- **Boot ROM**: reads SPL from the eMMC boot area. On a full power
  disconnect it also offers a USB download mode (`1782:4d00`, Spreadtrum's
  protocol) as a last resort; nothing here uses it.
- **SPL and U-Boot 2014.01**, vendor-built and left untouched. U-Boot has
  `ext4load`, `fatload`, `bootm` and `fdt`, no `bootz`, no SD driver.
- **`bootcmd=cboot`**: Spreadtrum's loader. It reads the `boot` partition
  (p16) as an Android boot image (page 2048) whose header's spare word gives
  the size of a Spreadtrum DT table (`SPRD`, one entry: platform 0x227e,
  variant 1, rev 0x20000). It rewrites `/memory` (both banks), appends its
  own arguments to `/chosen/bootargs` (`lcd_id`, `lcd_base`, `mem_cs`,
  `androidboot.serialno`, ...) and **ignores the header's cmdline**, so our
  command line lives in the DT.
- **Recovery**: cboot boots the `recovery` partition (p21) instead when the
  `misc` partition says `boot-recovery` or when the PMIC's reset reason says
  recovery (0x20). Our restart handler writes that reason for
  `reboot(2) RESTART2 "recovery"`, and the reason lasts one boot.

That last point is the whole install design (docs/install-plan.md): our
kernel goes into `boot`, stock Android's boot image moves to `recovery` as
the way back, and the same mechanism gives a one-shot **trial slot** for
new kernels (`tools/kernel-trial.sh`): write the new kernel to recovery,
reboot into it once; if it doesn't come up, a power cycle lands on the
known kernel in `boot`.

U-Boot also lights the panel and leaves the display controller scanning a
16 bpp splash buffer at 0x9fdfc000. Our display driver takes that scanout
over (docs/mainline.md, "Display"); there is no DSI host driver yet.
