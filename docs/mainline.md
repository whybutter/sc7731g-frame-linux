# VT10101: Linux on mainline — bring-up log

Goal (2026-10-24): the VT10101 runs our frame Linux (the armv7 Alpine
userland of the A33 frames), not Android. Wiping
Android is fine; new drivers are fine. This file is the running log: what is
known, how it was learned, what was tried and failed.

## Why mainline and not the vendor kernel

Our userspace does run on the vendor 3.10.65 kernel. Proven 2026-10-24 from
a chroot under Android:

- Alpine 3.22 armv7 runs.
- cage runs on wlroots' headless backend with pixman, and libinput sees the
  gslX680. libseat needs `LIBSEAT_BACKEND=noop` because Android kernels have
  no VTs.
- A small client (`fbmirror`) copies cage's output to `sprdfb`.
- 3.10 has no `memfd_create`, which foot died on, so an LD_PRELOAD shim backs
  it with `/dev/shm`.

It would have been a working frame within days. What it can't give us is the
GPU. A web UI on the frame composites on the Mali-400 through Mesa's lima, which
needs a mainline DRM kernel. The vendor's Mali is an Android-only blob, so the
page would be painted by the CPU: 2 fps instead of 10 on a stress page. Mainline also gets security fixes, which
a 2015 vendor kernel with a 2017 patch level will never have.

## The SoC, as the vendor's own files describe it

- **Vendor device tree.** The kernel is device-tree based, but `/proc` doesn't
  expose the tree. The DTB is in the boot image's `dt` section, a Spreadtrum
  `dt.img` ("SPRD" magic, platform 8830) with the FDT at offset 0x800. That
  is where the addresses below come from.
- **Vendor source.** Samsung's GPL source for the SM-G531H (Galaxy Grand Prime
  VE, also SC7731G, also 3.10.65) is the reference for register layouts:
  `drivers/platform/sprd/`, `include/soc/sprd/chip_x30g/`.
- **Names.** The vendor calls the clock tree `scx30g` and the board
  `sp8835eb`.

| Block | Where | Mainline |
|---|---|---|
| GIC-400 | 0x12001000 / 0x12002000 | yes |
| UART0-3 | 0x70000000.. (uart1 = console pads) | `sprd,sc9836-uart`, to be confirmed on the glass |
| Timers | 0x40050000, 0x40220000, 0x40330000, 0x40340000, 32 kHz | **new driver**: `kernel/timer-sprd-sc7731g.c` (mainline's is the SC9860's 64-bit timer) |
| Sys counter | 0x40230000, 1 kHz | not needed |
| Clocks | AON_APB 0x402e0000, AP_AHB 0x20d00000, PMU_APB 0x402b0000, AP clk 0x71200000 | **new driver** `kernel/clk-sprd-sc7731g.c`, the vendor's DT binding redone for mainline; tree in `kernel/sc7731g-clocks.dtsi` |
| PMIC | SC2713S (adie id 0x2713ca01) over ADI 0x40030000 | ADI maybe (`sprd,sc9860-adi`); SC2713 regulators: new |
| eMMC / SD / SDIO | 0x20600000 / 0x20300000 / 0x20400000, "sdhost-3.0" | mainline `sdhci-sprd` is r11 (SC9860); to check |
| Display | DISPC 0x20800000 (SPI 46), DSI 0x21800000 (SPI 48/49) | DSI host is Synopsys DW MIPI-DSI **1.21** (version register `1.21*`). DISPC is older than mainline's sharkl3 DPU: **new DRM driver** `kernel/sprd-sc7731g-dispc.c` (takeover of U-Boot's display, no DSI yet). Panel controller is a Fitipower **JD9365**, see "Display" below |
| GPU | Mali-400 MP2 (GP + 2 PP) at 0x60000000, one shared IRQ (SPI 39) | lima; power domain + clk_gpu through the clock driver |
| GPIO | 0x40280000, 256 in banks of 16 (SPI 35) | mainline `gpio-sprd`: the vendor's "d-gpio" has the same register layout |
| I2C | 0x70500000.. x4 (SPI 11-14) | **new driver** `kernel/i2c-sprd-sc7731g.c` (mainline's i2c-sprd is the SC9860's, a different design) |
| Touch | gslX680 on i2c1 0x40, GPIO 81 reset, 82 interrupt | mainline `silead`; firmware extracted from the Android kernel (`tools/extract-gsl-fw.py`) |
| Presence radar | GPIO 217 (`radar_input`, "zed-radar") | plain GPIO through sysfs, as frame-shell reads the A33's PIR. Its `PIR_GPIO = 204` is the A33's number; here it is the gpio-sprd chip's base + 217 |
| Backlight | PWM3 of the AP PWM block 0x40260000 (the vendor's `sprd_backlight` in PWM mode) | **new driver** `kernel/pwm-sprd-sc7731g.c` + mainline pwm-backlight |
| USB | DWC2 HS OTG at 0x20200000 (SPI 55); PHY = switches in AON_APB/AP_AHB/AP_APB plus the PMIC's vddusb LDO | mainline dwc2 as a gadget (CDC ECM: the network until there is Wi-Fi) + **new PHY glue** `kernel/phy-sprd-sc7731g-usb.c` |
| Wi-Fi/BT | integrated WCN: `sprdwl` over SIPC to a co-processor (`scproc` at 0x8a800000) running `wcnmodem.img`, RF chip 2351 | **not Realtek** as the vendor's build.prop says; nothing in mainline |
| RAM | 2 GiB (2 chip-selects); U-Boot sizes only cs0 and says 512 MiB | |

## Booting our own kernel

The vendor U-Boot 2014.01 stops on a key held on the UART even with bootdelay=0.
It has `ext4load`, `fatload`, `bootm`, `fdt` and `md`/`mw`. It has no `bootz`
and no SD driver (only `mmc 0`, the eMMC). So the dev loop is
`tools/uboot-boot.py`:

1. uImage + dtb go into the `cache` partition over adb.
2. Reboot, catch the prompt, `ext4load mmc 0:14` (partition numbers are hex),
   then `bootm`.

Nothing is flashed, and a plain power cycle boots the eMMC as before.

U-Boot's `bootm` rewrites `/memory` with what it sized (512 MiB), so the second
GiB waits until we own the boot chain.

U-Boot also lights the panel itself and leaves DISPC scanning its 16 bpp splash
buffer (layer 0 at 0x9fdfc000, 800x1280). The dts describes that buffer as a
`simple-framebuffer`, so simpledrm can draw before a display driver exists.

## SMP (written, not yet seen on the hardware)

`kernel/mach-sprd/platsmp.c`, from the vendor's `platsmp.c`. Bringing up core N
(1-3):

1. Write its entry address to `0x20d00018 + 4*N` (CPU_JUMP).
2. Set bit N in `0x20d00014` (HOLDING_PEN). The boot ROM spins on that.
3. Power the core on through `PMU_APB 0x402b0000 + PD_CA7_Cn_CFG` (0x8 for C1).
   Set auto-shutdown, clear force-shutdown, and hold the core's reset in
   `AP_AHB + 0x8` while that happens.

The vendor jumped into a second pen in the kernel (`sci_secondary_startup` plus
`pen_release`). Mainline dropped that pattern, so we jump straight to
`secondary_startup_arm` as sunxi does. No hotplug yet.

The first boots ran `nosmp`. The next boot tries all four cores. A core that
doesn't come up costs one second and a "failed to come online" line, not the
boot.

## eMMC (written, not yet seen on the hardware)

The controller is the vendor's `sprd,sdhost-3.0`, an SDHCI 3.0 register file.
The vendor drove it with its own driver (`drivers/mmc/host/sdhost.c`), not the
sdhci core. Mainline's `sdhci-sprd` is for the next generation (r11: v4 mode,
PHY DLL, 64-bit ADMA) and doesn't fit. `kernel/sdhci-sprd-sc7731g.c` is an
sdhci-pltfm glue carrying the vendor's differences:

- **Clock divider:** off by one, base / (2(N+1)).
- **Reset bit 3:** must stay set. It is the eMMC hardware reset, active low.
- **Delay registers:** at 0x80/0x84/0x88, rewritten after every full reset.
- **Speed mode:** no HISPD bit, and high speed is run as SDR12, as the vendor
  does.

The base clock is a fixed 384 MHz in the dts until there is a clock driver.
The vendor DT's `clk_emmc` mux (0x71200054, 2 bits) selects 26M / 256M / 312M
/ 384M. If U-Boot left it lower, the bus only runs slower than computed. That
is the safe direction.

Still unknown: whether the host's CAPABILITIES register (0x40) reports
voltages and ADMA honestly. `initramfs/probe` reads it. `sdhci-caps` and
`sdhci-caps-mask` in the dts, or `sdhci.debug_quirks=` on the command line,
are the fixes if not.

## Probe boots

Put `probe` on the command line and the initramfs runs `/bin/probe`. It dumps
the register state the next drivers need: SMP power domains, clock muxes, the
eMMC host's capabilities, DISPC and DSI as U-Boot left them. Every line is
prefixed `probe:`. With `probe_reboot` the initramfs then reboots, which also
tests the ADI/PMIC-watchdog restart. If that works, a probe boot needs nobody
at the frame.

## Built, waiting for the hardware (out-next/)

`out/` is what a push helper (not kept) sends on the next power cycle: SMP, eMMC, the
DISPC takeover, the probe. `OUT=out-next ./build.sh kernel` has the next step
on top of it. That step changes the boot more, so it waits until `out/` has
been seen working:

- **Clocks.** The vendor's clock tree (`kernel/sc7731g-clocks.dtsi`) is
  converted from the frame's own dt.img by `tools/clocks2dtsi.py`.
  `clk-sprd-sc7731g.c` reads the vendor binding: one node per clock, with raw
  `<register mask>` pairs. PLLs are read-only. Gates and muxes are
  read-modify-write, not the set/clear aliases. The camera/codec (MM domain)
  clocks are left out. The eMMC stays on a fixed 384 MHz until the probe's
  `clk_summary` matches what the muxes say.
- **lima.** clk_gpu's mux and divider are inside the GPU, so the clock driver
  powers the GPU domain first (`sprd,domain`: PD_GPU_TOP_CFG force-shutdown
  off, AON_APB EB0 bit 27 on) and leaves it on. lima gets clk_gpu_axi as
  "bus", and clk_gpu muxed to the 256 MHz tap (the vendor's default) as
  "core".
- **Touch.** GPIO comes from mainline gpio-sprd, I2C from the new
  controller driver, and the gslX680 from mainline silead. Its firmware
  (4719 register/value pairs, 143 pages) is cut from the Android kernel by
  `tools/extract-gsl-fw.py` into the gitignored `firmware/`, which the build
  copies into the initramfs. The vendor DT's TP_MAX is 1024x600, a leftover;
  the dts says 800x1280 until evtest says otherwise. vdd28 is left as U-Boot
  left it.
- **Backlight.** PWM3 runs at the vendor's 26 MHz / 256 with levels 0-255
  straight onto the duty, through pwm-backlight
  (`/sys/class/backlight/backlight`).

- **USB network.** dwc2 runs with the role switch pinned to peripheral,
  which overrides B-session-valid (the vendor sensed VBUS on a GPIO). The
  PHY glue replays the vendor's `usb_startup()`: PHY power, three soft
  resets, the tune word. A built-in CDC ECM gadget gives `usb0`, and
  `usbnet` on the command line makes the initramfs bring it up at
  192.168.7.2 with a telnetd. Still unknown: whether vddusb (PMIC LDO,
  `LDO_PD_CTRL` bit 9) is on after U-Boot. The probe reads it over the ADI.
  If it's off, a small SC2713S regulator driver comes next.

## Display: what the vendor's kernel says about the panel

- **Panel.** `lcd_id=ID936500` on the vendor command line is a Fitipower
  **JD9365**. The kernel's panel driver strings say `jd9365`.
- **Init sequence.** It is in the Android kernel as LCM_Init_Code entries
  (u32 tag + 56 bytes). `tools/extract-lcm-init.py` prints it: 197 commands.
  It starts with the JD9365 password (E0 00, E1 93, E2 65, E3 F8), writes
  pages 1, 2 and 4, then sleep-out, 120 ms, display-on. That is the shape
  mainline's `panel-jadard-jd9365da-h3` takes. A panel entry there plus a
  DW-DSI 1.21 host is the real display driver. Until then U-Boot lights the
  panel and `sprd-sc7731g-dispc.c` takes over its scanout.
- **Timings and lanes.** The probe dumps DISPC (DPI timing) and the DSI
  host's registers as U-Boot left them.

## Installing (later)

U-Boot's `bootcmd` is `cboot`, Spreadtrum's loader for the boot partition
(an Android boot image whose dt section is a Spreadtrum dt.img). Two ways to
make our kernel the default without touching U-Boot itself:

1. Write a boot.img with our zImage and our dtb (wrapped as dt.img) into
   `boot`. cboot loads it like Android's. Undo: write back
   `partitions/boot.img` from the backup.
2. `setenv bootcmd` to our ext4load/bootm and `saveenv`. This loses cboot's
   recovery and charge-mode logic.

Either way the root filesystem would be the A33 frame's: the A33 is a
Cortex-A7 too, so the armv7 Alpine rootfs carries over as is, and only the
kernel, dtb and modules differ. It goes on `userdata`.

## RAM

U-Boot's bootm sizes /memory to 512 MiB. The vendor command line says
`mem_cs=2, mem_cs0_sz=40000000`: two chip-selects of 1 GiB, 2 GiB in all.
`mem=2048M@0x80000000` on our command line should claim it all, once it is
confirmed that CS1 starts at 0xc0000000.

## The early hang (2026-10-24, two boots)

Both SMP boots hard-hung about 1.7-2.5 s after the kernel started, with no
console, no serial SysRq and no RCU stall report, so every core died at once:

- `out/` (SMP + eMMC + DISPC): the kernel got through all its probes, the
  eMMC read its 23 partitions, init ran, and the probe's output stopped
  mid-word about a second into userspace. That run also showed the ADI
  refusing to register (`-EINVAL`: sprd-adi takes `num_chipselect` from the
  number of child nodes, and the node had none; fixed with a `pmic@0` child),
  and busybox-static having no `devmem` (now initramfs/devmem.c).
- `out-3/` (the above + clocks, lima, I2C/touch, PWM, USB): it hung right
  after "Freeing unused kernel image", before /init printed anything, so
  pmic-wdt never armed.

Ruled out: MCU_PAUSE's sleep enables. platsmp.c logged `MCU_PAUSE was 0x10`
(SLEEP_FOLLOW_CA7 only, all four enables already off). The clearing stays in,
as the vendor does it, but it is not the cause.

The two hangs land at the same time since boot under very different loads,
which points at something timed rather than at a workload. Next boots, with
`tools/uboot-boot.py --wdt 60` so a hang resets the frame without anyone at
the plug, bisect it: `maxcpus=1` (SMP), `initcall_blacklist=` for the dispc
and sdhci drivers, `clk_ignore_unused`.

What out-3 showed besides the hang:
- lima probes: Mali-400 with GP and two PPs ("MP2"), 32K L2, bus 128 MHz,
  core 256 MHz (the clock driver's rates)
- i2c1 comes up (controller r0p0, 100 kHz off 26 MHz), but the gslX680
  doesn't answer at 0x40 (-ENXIO): the chip is unpowered (vdd28?) or held
  in reset
- dwc2: "Soft Reset timeout GRSTCTL_CSFTRST", so the PHY or its clock isn't
  running; check vddusb (LDO_PD_CTRL bit 9) once the probe works
- the eMMC hadn't found its card yet when the kernel died

### Third boot: single core, watchdog from U-Boot (2026-10-24)

`tools/uboot-boot.py --wdt 60` armed the PMIC watchdog before bootm and read
back CTRL 0xe (RUN|RST_EN|NEW), so the SC2713S watchdog offsets the restart
handler and pmic-wdt use are right. With `maxcpus=1` the kernel hung at the
same point ("Freeing unused kernel image"), so SMP is not the cause.

The watchdog reset the chip 60 s later, but the boot ROM printed `MCI ERF`
twice (a good boot prints `MCI ERS`), then `ROM` and nothing more. It was off
USB entirely, neither adb nor the 1782:4d00 download device, until a power
cycle. A reading: the eMMC was left mid-operation by our driver, and a warm
reset doesn't reset the card the way a power cycle does. **Until that is
understood, a warm reset after our kernel has touched the eMMC needs a
replug to recover**, the restart handler's included.

The eMMC driver is now the suspect for the hang too: in both out-3 boots
the hang came within ~0.2 s of "mmc0: SDHCI controller on 20600000.mmc",
before the card was detected; in the first boot the card was found and the
hang came about a second later.

**Next boot** (the push helper was not kept; re-create it
from this): out-3, all cores, `--wdt 60`, and
`initcall_blacklist=sc7731g_sdhci_driver_init` on the command line, after a
power cycle:

    python3 tools/uboot-boot.py --push out-3 --wdt 60 --seconds 300 \
      --until "probe: rebooting" --after 30 \
      --args "console=ttyS1,115200n8 earlycon ignore_loglevel panic=5 \
    printk.devkmsg=on sysctl.kernel.panic_on_rcu_stall=1 \
    initcall_blacklist=sc7731g_sdhci_driver_init wdt probe_reboot" boot6.log

If it survives, the hang is in sdhci-sprd-sc7731g.c (suspects: the clock
divider at 384 MHz base, the forced RST bit 3, the delay registers); the
probe then also runs (devmem, ADI, watchdog readback) and the reset should
return to Android. If it still hangs, the next candidates are the DISPC
takeover (`initcall_blacklist=sc7731g_dispc_driver_init`) and
`clk_ignore_unused`.

### Done offline while the frame was away (2026-10-25)

- **`tools/bisect-hang.sh LOGDIR`**: the bisect as one unattended sitting,
  starting from Android after a power cycle. It runs out-3 with the eMMC
  driver blacklisted and without the sysctl the hung boots carried; then
  with the sysctl back; then also without DISPC and lima, with
  `clk_ignore_unused`; and the eMMC driver alone last, because a boot that
  touches the eMMC can't warm-reset back to Android. Each boot is
  `--wdt 60`, so a hang resets back to Android and the next step starts.
- **Why the sysctl is suspect too**: both out-3 hangs stopped exactly after
  "Freeing unused kernel image", where kernel_init goes on to
  mark_readonly() and do_sysctl_args(), and
  `sysctl.kernel.panic_on_rcu_stall=1` is the one command-line difference
  from out/, whose init did run. Unlikely, but cheap to rule out.
- **DMA mode is probably not the eMMC hang**: the vendor's sdhost.c uses
  SDMA only, with one segment and a 512 KiB boundary (BLK_SIZE | 0x7000),
  and mainline ended up in the same mode ("using DMA", the bounce buffer).
  The vendor does nothing to the card before a reset either (its
  sdhost_shutdown is empty; restart is the bare PMIC watchdog).
- **`kernel/regulator-sc2713s.c`** (built into out-4/): the PMIC's LDOs over
  the ADI, with the vendor DT's register/mask description. Only vdd28
  (touch, always-on) and vddusb (USB PHY `vdd-supply`) are described, since
  the regulator core turns off what nobody claims. This targets out-3's
  touch -ENXIO and dwc2 soft-reset timeout. Untested on hardware.

## 2026-10-27: the hang found, the frame stays up

- **The early hang was clock auto-gating.** U-Boot leaves AP_AHB
  `AP_SYS_AUTO_SLEEP_CFG` at 0x3B (CA7 core/debug/EMC, AP EMC and AP AHB
  auto gates), the vendor's deep-sleep value. Once every core is in WFI the
  clocks gate, and the wake would have to come through Spreadtrum's INTC,
  which mainline doesn't drive. The idle matrix (initramfs `idletest`, 150
  heartbeats a second apart):
  plain WFI died at the first sleep, `sprd_noautopd` (CA7_TOP/C0
  auto-shutdown off) too, `nohlt` survived, and gates cleared survived
  under WFI. platsmp.c now clears them at boot. The earlier suspects (eMMC
  driver, the `sysctl.kernel.panic_on_rcu_stall=1` argument, MCU_PAUSE) were
  all timing coincidences: whichever boot went idle first died first.
- **Restart and watchdog both work on hardware.** The ADI restart handler
  resets the chip ~2 s after `reboot -f`. The PMIC watchdog armed from
  U-Boot (`uboot-boot.py --wdt`) and fed by `pmic-wdt` brings back any hang
  60 s later. A warm reset after real eMMC use boots fine when the card was
  idle.
- **USB networking works.** With vddusb from regulator-sc2713s.c, dwc2
  comes out of reset, g_ether enumerates on the Mac as a high-speed
  device, and `nc -6 fe80::…%enN 23` reaches a root shell. dwc2 still
  rejects some of the board's FIFO sizes (np-tx 256, tx[1..2] 256 "invalid")
  and falls back to its defaults for them; it works anyway.
- **Touch still doesn't answer.** vdd28 is on (as under Android, where
  `gslX680` binds at i2c1 0x40), and GPIO 81 drives high, but nothing at
  all answers on i2c1. The I2C driver itself works: i2c2's accelerometer
  answers at 0x27 (Android's DT says 0x26; its mir3da driver tries both).
  Next suspect: GPIO 81's pad isn't muxed to its GPIO function, so the chip
  stays in reset. Android has no /dev/mem or debugfs to compare against.
- **The shell works**: init's cttyhack (absent from busybox-static) is
  replaced by setsid opening the console itself.

### Touch works (2026-10-27)

- The gslX680's reset (GPIO 81) and interrupt (GPIO 82) are pads SIMCLK2 and
  SIMDA2, in the SIM2 pad group powered by `vddsim2`, which U-Boot leaves off
  (LDO_PD_CTRL bit 4). Found by mapping pads to GPIOs on the live frame:
  every input GPIO's read-back enabled, each GPIO-muxed pad's weak pulls
  flipped, all 16 banks read. Results: GPIO = pad offset / 4 - 3 from SPI0 to
  SD1, -7 over the CP1 RF control pads, -22 over LCD/NAND; radar GPIO 217 =
  IIS1CLK. U-Boot's board pin table (231 entries, offset/value pairs) is at
  0x5b4xx in mmcblk0boot1.img.
- With vddsim2 on, the chip reads ID 0xB4820000 and loads the extracted
  firmware, and 30 s of tapping gave 447 interrupts and 1457 evdev events.
- Raw coordinates are the firmware's landscape frame, not the panel's: X
  91..934 along the long side (a ~1024 range), Y 60..616 along the short
  side. Held landscape, our clockwise corner taps were TL (91,539),
  TR (918,526), BR (878,147), BL (103,138): X runs left to right, Y bottom
  to top. The dts still says 800x1280; the mapping to the panel's
  portrait-native scanout is the next fix, once we know which way up the
  panel's text reads in that hold.

### Display and touch, lined up (2026-10-27)

- **The screen works.** The DISPC driver had used
  drm_gem_fb_create_with_dirty; with a dirty hook drm_fbdev_dma draws into
  a vmalloc shadow and a damage worker blits it to the scanout buffer. On
  the frame the blit happened once and never again, so the glass kept
  whatever Android had left in that RAM (it survives a warm reset), which
  we saw as "nothing", then as a frozen Android screen. Why the worker
  stopped wasn't found: no oops, no stuck thread, oops_in_progress 0, and no
  kprobes in this build. Without the hook, fbdev and fbcon draw straight
  into the buffer the DISPC re-reads every frame. (Some of the debugging
  was muddied by busybox dd: `iflag=fullblock` makes it fail silently.)
- **Orientation:** held landscape with the panel's ribbon at the bottom,
  framebuffer row 0 is the left edge and each row runs bottom to top.
  `fbcon=rotate:1` makes the console upright, confirmed on the glass.
- **Touch** is a plain axis swap of the firmware's frame
  (`touchscreen-swapped-x-y`, 1024 x 640). Corner taps on squares 40 px in
  from each corner landed at (971,17) bottom-right, (38,12) bottom-left and
  (21,624) top-left, in left-to-right x bottom-to-top units, within ~20 of
  the squares. Reported coordinates follow the panel's native framebuffer
  axes, so a compositor rotating the output rotates touch with it.
  **Reversed the same day:** wlroots maps a touchscreen's range linearly
  onto the *transformed* output, so touch must come in already landscape.
  frame-shell's calibration page landed cross 1 on cross 2, as the swap
  predicts. The DT now has no swap, only `touchscreen-inverted-y`.

### The userland on the eMMC, day one (2026-10-27)

- **Touch is fitted, not nominal.** With 1024 x 640 our taps on
  frame-shell's five calibration crosses landed 20-25 px toward the middle
  at every corner. 20 taps (frame-shell logs them; frame-diag's 180s report
  on FRAMEBOOT/diag/ carried them off the frame) put the glass edges at raw
  X 22..1004, Y 6..614: `touchscreen-min-x = 22`, `size-x = 1005`,
  `size-y = 615`. Linux inverts Y as max - y, so Y can't also carry a
  minimum: the top edge is exact, the bottom ~8 px high (we saw
  exactly that on cross 3).
- **USB network: dwc2's ep1out wedges if the host talks first.** The userland
  opened usb0 ~30 s into boot (its network wait expects a wlan0 this board
  lacks). Traffic the Mac sent to the enumerated gadget before then left
  ep1out dead: the frame took 7 packets and no more, TX carried on, the
  USB interrupt count stood still, DOEPTSIZ read nonsense, and a soft
  reconnect logged `dwc2_hsotg_ep_stop_xfr: timeout DOEPCTL.EPDisable` --
  after which it worked. The Mac never got a lease, and multicast pings
  that "failed" in the debug shell misled the first look (unicast worked).
  frameboot now does `ifconfig usb0 up` before switch_root; the Mac leases
  192.168.7.1 and SSH answers at ~50 s. Reach it as
  `ssh root@fe80::fa:a3ff:fe00:802%enN` -- 192.168.7.2 by route is ambiguous
  when a LAN also covers 192.168.7.0/24 or an A33 frame is on USB too.
- **No modules, but the userland modprobes.** With no /lib/modules/6.18.57
  every modprobe printed "can't change directory" on the glass and failed,
  and the touch daemon (modprobe uinput) didn't start. The build stages the
  kernel's modules.builtin into the initramfs (a second pass when it
  changed) and frameboot mounts it over the root's /lib/modules. TUN was
  missing from the config too (for VPN clients).
- **The radar isn't fitted.** GPIO 217 read 1 throughout, empty room or
  not; the board's back shows the footprint next to the UART pads,
  silkscreened IR-IN / GND / V-IR, with its holes empty. A PIR soldered
  there would likely land on that GPIO.
- **Catching U-Boot from cold.** The black screen with uboot-cmd.py armed
  was U-Boot's prompt (the panel isn't lit yet), not a frame that couldn't
  boot: the script missed `=>` behind the BELs U-Boot answers an
  overflowing command line with. Fixed in the tool.
- **A UART shell inside the userland** (bring-up only): FRAMEBOOT/frame-fix.sh
  opens /dev/ttyS1 before switch_root and keeps a root shell on it. Its
  first version opened the tty after a sleep, by which time the userland's
  tmpfs over /tmp hid the node it had made.
- Debug boots (`tools/uboot-cmd.py` + `ext4load mmc 0:14`) now load out-23
  from /cache/vt10101; out-15 stays beside it as uImage.out15.

### Kernel updates without the UART (2026-10-28)

Our rule from here on: no serial cable. The rootfs has A/B
slots, the kernel had only p16, so `tools/kernel-trial.sh` makes recovery
(p21, where stock Android's recovery.img was; backed up) the kernel's trial
slot. `try` writes a boot image there, verifies it and reboots with
RESTART2 "recovery" (Python's ctypes, as busybox reboot takes no argument),
which our restart handler turns into PMIC reason 0x20: cboot boots p21 for
that one boot and p16 again after any other reset. `promote`, run on the
trial kernel (`uname -v` differs from the one recorded in
FRAMEBOOT/kernel-trial.env), copies it to p16. Proven both ways with
out-p16b (kernel #12): it booted from p21, a plain reboot came back
to #8 on p16, a second try was promoted. A trial kernel that hangs needs a
power cycle, no more -- unplug it, p16 boots.

## The last kernel log across a crash (pstore, 2026-10-29)

the native Wi-Fi scan seemed to take the frame down hard, and nothing
survived to say how: no console (uart1 is no longer one), and the next boot
was a fresh kernel. (It turned out the "crash" was cp2 state carried over
from a hung trial, not a crash; the insurance stays.) ramoops now sits in
the DT's reserved-memory at 0x9fc00000, 1 MiB under the splash buffer, with
PSTORE_RAM, CONSOLE and PMSG, and ecc-size 64 as on the A33 frame, whose
DRAM drops bits across the reset.

Proven with #50 (out-30): a sysrq-c panic, then the reboot onto p16. The first
boot of a kernel with ramoops prints "ramoops: error in header" seven times
(the strip never held a record); the boot after the panic printed none. So
this frame's DRAM keeps that strip across the reset, and ECC reported no
errors. Nothing appears in /sys/fs/pstore, because the userland's frame-diag
moves the records at boot to FRAMEBOOT/diag/crash-<stamp>-<id>/:
dmesg-ramoops-0 (the panic and backtrace), console-ramoops-0 (the whole log
up to it) and pmsg-ramoops-0 (whatever user space wrote to /dev/pmsg0, which
is handy as a "about to do X" marker). The stamp is taken before chrony syncs
(2025-07-23 on this frame), so sort by name, not by date. Only a kernel that
has the node reads the strip back; p16 does now.
