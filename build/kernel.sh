#!/bin/sh
# Mainline kernel for the VT10101: out/zImage (for the boot image cboot
# loads), out/uImage (for a debug boot through the vendor U-Boot's bootm,
# which has no bootz) and out/sc7731g-vtimes-vt10101.dtb. The initramfs is
# built into the kernel.
#
# Like the A33 frame's kernel step, our files are copied into the unpacked tree
# and hooked into Kconfig/Makefiles idempotently -- no patches to rebase.
set -eu
. /versions.env
dl=/cache/dl
src=/cache/src/linux-$LINUX_VERSION-vt10101${OBJ:+-$OBJ}  # our drivers are copied in, so per-OBJ too
# OBJ=<name>: a separate object tree and initramfs staging dir, so two builds
# can run at once (two parallel builds once shared these and overwrote init)
obj=/cache/obj/linux-vt10101${OBJ:+-$OBJ}
board=sc7731g-vtimes-vt10101
out=/work/${OUT:-out}
mkdir -p $dl $obj $out

if [ ! -f $src/Makefile ]; then
  t=$dl/linux-$LINUX_VERSION.tar.xz
  if [ ! -f $t ]; then wget -q -O $t.part "$LINUX_URL" && mv $t.part $t; fi
  rm -rf $src.part && mkdir -p $src.part
  tar -C $src.part --strip-components=1 -xf $t
  mv $src.part $src
fi

# arch/arm has no ARCH_SPRD (mainline's Spreadtrum support is arm64); the
# drivers we reuse (sprd_serial, ...) depend on it, so the 32-bit platform
# gets the same symbol.
mkdir -p $src/arch/arm/mach-sprd
cat > $src/arch/arm/mach-sprd/Kconfig <<'K'
# SPDX-License-Identifier: GPL-2.0-only
menuconfig ARCH_SPRD
	bool "Spreadtrum (Unisoc) SC7731G / SC8830 family"
	depends on ARCH_MULTI_V7
	select ARM_GIC
	select MFD_SYSCON
	select SC7731G_TIMER
	help
	  32-bit Spreadtrum SoCs of the SC8830 generation (SC7731G).
K
cp /work/kernel/mach-sprd/*.c $src/arch/arm/mach-sprd/
echo 'obj-$(CONFIG_SMP) += platsmp.o' > $src/arch/arm/mach-sprd/Makefile
grep -q 'ARCH_SPRD' $src/arch/arm/Makefile ||
  sed -i 's|^machine-$(CONFIG_ARCH_SUNXI).*|&\nmachine-$(CONFIG_ARCH_SPRD)\t\t+= sprd|' $src/arch/arm/Makefile
grep -q 'ARCH_SPRD' $src/arch/arm/Makefile || { echo "arch/arm/Makefile hook failed"; exit 1; }
grep -q mach-sprd/Kconfig $src/arch/arm/Kconfig ||
  sed -i 's|^source "arch/arm/mach-sunxi/Kconfig"|&\nsource "arch/arm/mach-sprd/Kconfig"|' $src/arch/arm/Kconfig

cp /work/kernel/timer-sprd-sc7731g.c $src/drivers/clocksource/
grep -q SC7731G_TIMER $src/drivers/clocksource/Kconfig || cat >> $src/drivers/clocksource/Kconfig <<'K'

config SC7731G_TIMER
	bool "Spreadtrum SC7731G timer" if COMPILE_TEST
	depends on HAS_IOMEM && GENERIC_CLOCKEVENTS
	select TIMER_OF
	select MFD_SYSCON
	help
	  The 32 kHz timer blocks of the SC7731G (SC8830 family).
K
grep -q timer-sprd-sc7731g $src/drivers/clocksource/Makefile ||
  echo 'obj-$(CONFIG_SC7731G_TIMER) += timer-sprd-sc7731g.o' >> $src/drivers/clocksource/Makefile
grep -q CPUHP_AP_SC7731G_TIMER_STARTING $src/include/linux/cpuhotplug.h ||
  sed -i 's|^\(\s*\)CPUHP_AP_ARM_ARCH_TIMER_STARTING,|\1CPUHP_AP_SC7731G_TIMER_STARTING,\n&|' $src/include/linux/cpuhotplug.h
grep -q CPUHP_AP_SC7731G_TIMER_STARTING $src/include/linux/cpuhotplug.h || { echo "cpuhotplug.h hook failed"; exit 1; }

cp /work/kernel/clk-sprd-sc7731g.c $src/drivers/clk/
grep -q clk-sprd-sc7731g $src/drivers/clk/Makefile ||
  echo 'obj-$(CONFIG_ARCH_SPRD) += clk-sprd-sc7731g.o' >> $src/drivers/clk/Makefile

cp /work/kernel/i2c-sprd-sc7731g.c $src/drivers/i2c/busses/
grep -q I2C_SC7731G $src/drivers/i2c/busses/Kconfig || cat >> $src/drivers/i2c/busses/Kconfig <<'K'

config I2C_SC7731G
	tristate "Spreadtrum SC7731G I2C controller"
	depends on ARCH_SPRD || COMPILE_TEST
	help
	  The byte-at-a-time I2C controller of the SC7731G (SC8830 family).
K
grep -q i2c-sprd-sc7731g $src/drivers/i2c/busses/Makefile ||
  echo 'obj-$(CONFIG_I2C_SC7731G) += i2c-sprd-sc7731g.o' >> $src/drivers/i2c/busses/Makefile

cp /work/kernel/pwm-sprd-sc7731g.c $src/drivers/pwm/
grep -q PWM_SC7731G $src/drivers/pwm/Kconfig || sed -i 's|^config PWM_SPRD$|config PWM_SC7731G\n\ttristate "Spreadtrum SC7731G PWM"\n\tdepends on ARCH_SPRD \|\| COMPILE_TEST\n\thelp\n\t  The four-channel PWM of the SC7731G (SC8830 family).\n\n&|' $src/drivers/pwm/Kconfig
grep -q PWM_SC7731G $src/drivers/pwm/Kconfig || { echo "pwm Kconfig hook failed"; exit 1; }
grep -q pwm-sprd-sc7731g $src/drivers/pwm/Makefile ||
  echo 'obj-$(CONFIG_PWM_SC7731G) += pwm-sprd-sc7731g.o' >> $src/drivers/pwm/Makefile

cp /work/kernel/phy-sprd-sc7731g-usb.c $src/drivers/phy/
grep -q PHY_SC7731G_USB $src/drivers/phy/Kconfig || sed -i 's|^source "drivers/phy/allwinner/Kconfig"|config PHY_SC7731G_USB\n\ttristate "Spreadtrum SC7731G USB PHY"\n\tdepends on ARCH_SPRD \|\| COMPILE_TEST\n\tselect GENERIC_PHY\n\tselect MFD_SYSCON\n\thelp\n\t  The USB 2.0 PHY switches of the SC7731G.\n\n&|' $src/drivers/phy/Kconfig
grep -q PHY_SC7731G_USB $src/drivers/phy/Kconfig || { echo "phy Kconfig hook failed"; exit 1; }
grep -q phy-sprd-sc7731g-usb $src/drivers/phy/Makefile ||
  echo 'obj-$(CONFIG_PHY_SC7731G_USB) += phy-sprd-sc7731g-usb.o' >> $src/drivers/phy/Makefile

cp /work/kernel/sdhci-sprd-sc7731g.c $src/drivers/mmc/host/
grep -q MMC_SDHCI_SC7731G $src/drivers/mmc/host/Kconfig || cat >> $src/drivers/mmc/host/Kconfig <<'K'

config MMC_SDHCI_SC7731G
	tristate "Spreadtrum SC7731G SD/eMMC host"
	depends on ARCH_SPRD || COMPILE_TEST
	depends on MMC_SDHCI_PLTFM
	help
	  The SDHCI 3.0 host of the SC7731G (SC8830 family).
K
grep -q sdhci-sprd-sc7731g $src/drivers/mmc/host/Makefile ||
  echo 'obj-$(CONFIG_MMC_SDHCI_SC7731G) += sdhci-sprd-sc7731g.o' >> $src/drivers/mmc/host/Makefile

cp /work/kernel/regulator-sc2713s.c $src/drivers/regulator/
grep -q REGULATOR_SC2713S $src/drivers/regulator/Kconfig || sed -i 's|^config REGULATOR_SC2731$|config REGULATOR_SC2713S\n\ttristate "Spreadtrum SC2713S PMIC LDOs"\n\tdepends on SPI_SPRD_ADI \|\| COMPILE_TEST\n\tselect REGMAP\n\thelp\n\t  The LDOs of the SC7731G'"'"'s PMIC, on the ADI bus.\n\n&|' $src/drivers/regulator/Kconfig
grep -q REGULATOR_SC2713S $src/drivers/regulator/Kconfig || { echo "regulator Kconfig hook failed"; exit 1; }
grep -q regulator-sc2713s $src/drivers/regulator/Makefile ||
  echo 'obj-$(CONFIG_REGULATOR_SC2713S) += regulator-sc2713s.o' >> $src/drivers/regulator/Makefile
# audio (docs/audio.md): the VBC DAI beside mainline's sprd-pcm-dma, whose
# header it includes, and the SC2713S codec; both ride CONFIG_SND_SOC_SPRD
cp /work/kernel/sc7731g-vbc.c /work/kernel/sc2713s-codec.c $src/sound/soc/sprd/
grep -q sc7731g-vbc $src/sound/soc/sprd/Makefile ||
  echo 'obj-$(CONFIG_SND_SOC_SPRD) += sc7731g-vbc.o sc2713s-codec.o' >> $src/sound/soc/sprd/Makefile

# WCN co-processor and its SIPC transport, under drivers/soc/sprd/ (where
# the vendor's sibling mainline drivers for the SP9860 live upstream).
mkdir -p $src/drivers/soc/sprd
cp /work/kernel/wcn-sc7731g.c $src/drivers/soc/sprd/
cp /work/kernel/wcn-sc7731g.h $src/drivers/soc/sprd/
grep -q WCN_SC7731G $src/drivers/soc/sprd/Kconfig 2>/dev/null || cat >> $src/drivers/soc/sprd/Kconfig <<'K'

config WCN_SC7731G
	tristate "Spreadtrum SC7731G WCN co-processor and SIPC transport"
	depends on ARCH_SPRD || COMPILE_TEST
	depends on OF
	select MFD_SYSCON
	help
	  The SC7731G's integrated connectivity subsystem is an ARM core
	  (cp2) that runs Spreadtrum's firmware for Wi-Fi, Bluetooth and FM
	  and talks to the AP over shared DRAM. This driver loads cp2's
	  firmware and offers the sbuf and sblock transport to the radios on
	  top (sprdwl-sc7731g for Wi-Fi).
K
grep -q 'drivers/soc/sprd/Kconfig' $src/drivers/soc/Kconfig ||
  echo 'source "drivers/soc/sprd/Kconfig"' >> $src/drivers/soc/Kconfig
grep -q wcn-sc7731g $src/drivers/soc/sprd/Makefile 2>/dev/null ||
  echo 'obj-$(CONFIG_WCN_SC7731G) += wcn-sc7731g.o' >> $src/drivers/soc/sprd/Makefile
grep -q 'drivers/soc/sprd/' $src/drivers/soc/Makefile ||
  echo 'obj-y += sprd/' >> $src/drivers/soc/Makefile

# sprdwl: the cfg80211 full-MAC Wi-Fi driver on top of WCN. The core
# header lives next to wcn-sc7731g.c under drivers/soc/sprd/, so the
# driver source goes there too; Kconfig and Makefile hooks mirror the core
cp /work/kernel/sprdwl-sc7731g.c $src/drivers/soc/sprd/
grep -q SPRDWL_SC7731G $src/drivers/soc/sprd/Kconfig 2>/dev/null || cat >> $src/drivers/soc/sprd/Kconfig <<'K'

config SPRDWL_SC7731G
	tristate "Spreadtrum SC7731G Wi-Fi (sprdwl over SIPC)"
	depends on WCN_SC7731G
	depends on CFG80211
	help
	  cfg80211 full-MAC STA driver for the SC7731G's WCN Wi-Fi. Needs
	  a WCN co-processor parent in the DT. cp2 does scanning, auth and
	  association; the host runs the WPA 4-way handshake through
	  wpa_supplicant on the netdev.
K
grep -q sprdwl-sc7731g $src/drivers/soc/sprd/Makefile 2>/dev/null ||
  echo 'obj-$(CONFIG_SPRDWL_SC7731G) += sprdwl-sc7731g.o' >> $src/drivers/soc/sprd/Makefile

# bt-sc7731g: the WCN's Bluetooth as an HCI device, on the same core
cp /work/kernel/bt-sc7731g.c $src/drivers/soc/sprd/
grep -q BT_SC7731G $src/drivers/soc/sprd/Kconfig 2>/dev/null || cat >> $src/drivers/soc/sprd/Kconfig <<'K'

config BT_SC7731G
	tristate "Spreadtrum SC7731G Bluetooth (HCI over SIPC)"
	depends on WCN_SC7731G
	depends on BT
	help
	  The SC7731G WCN's BR/EDR controller as an HCI device: H4 over
	  SIPC sbuf channel 4 ring 10. The hci device registers once user
	  space writes the controller's PSKEY block to the device's pskey
	  attribute.
K
grep -q bt-sc7731g $src/drivers/soc/sprd/Makefile 2>/dev/null ||
  echo 'obj-$(CONFIG_BT_SC7731G) += bt-sc7731g.o' >> $src/drivers/soc/sprd/Makefile

cp /work/kernel/sprd-sc7731g-dispc.c $src/drivers/gpu/drm/tiny/
grep -q DRM_SC7731G_DISPC $src/drivers/gpu/drm/tiny/Kconfig || cat >> $src/drivers/gpu/drm/tiny/Kconfig <<'K'

config DRM_SC7731G_DISPC
	tristate "Spreadtrum SC7731G display controller (U-Boot takeover)"
	depends on DRM && OF && MMU
	depends on ARCH_SPRD || COMPILE_TEST
	select APERTURE_HELPERS
	select DRM_CLIENT_SELECTION
	select DRM_GEM_DMA_HELPER
	select DRM_KMS_HELPER
	help
	  Scans out from the SC7731G's DISPC OSD layer on the display the
	  bootloader left lit.
K
grep -q sprd-sc7731g-dispc $src/drivers/gpu/drm/tiny/Makefile ||
  echo 'obj-$(CONFIG_DRM_SC7731G_DISPC) += sprd-sc7731g-dispc.o' >> $src/drivers/gpu/drm/tiny/Makefile

# kernel/patches/*.patch: changes to mainline code, each with its why in the
# patch itself. Applied once (a patch that reverses cleanly is already in).
# $src/.vt10101-patches lists what's in: a reverse dry run alone stopped
# working once 0002 changed lines 0001 had added (0001 no longer reverses on
# its own), so a patch found in either way is recorded and skipped from then on.
stamp=$src/.vt10101-patches
touch $stamp
for p in /work/kernel/patches/*.patch; do
  [ -e "$p" ] || continue
  n=$(basename "$p")
  grep -qx "$n" $stamp && continue
  if ! patch -d $src -p1 -R -s -f --dry-run < "$p" >/dev/null 2>&1; then
    patch -d $src -p1 -s -N < "$p" || { echo "patch failed: $p"; exit 1; }
    echo "applied $n"
  fi
  echo "$n" >> $stamp
done

cp /work/kernel/sc7731g.dtsi /work/kernel/sc7731g-clocks.dtsi /work/kernel/$board.dts $src/arch/arm/boot/dts/unisoc/
grep -q $board $src/arch/arm/boot/dts/unisoc/Makefile ||
  printf 'dtb-$(CONFIG_ARCH_SPRD) += %s.dtb\n' $board >> $src/arch/arm/boot/dts/unisoc/Makefile

# initramfs: static busybox from Alpine (same branch as the rootfs)
ird=/cache/obj/vt10101-initramfs${OBJ:+-$OBJ}
if [ ! -x $ird/bin/busybox ]; then
  rm -rf $ird && mkdir -p $ird/bin $ird/proc $ird/sys $ird/dev
  repo=$ALPINE_MIRROR/$ALPINE_BRANCH/main/armv7
  wget -q -O /tmp/APKINDEX.tar.gz $repo/APKINDEX.tar.gz
  ver=$(tar -xzOf /tmp/APKINDEX.tar.gz APKINDEX | awk '/^P:busybox-static$/{f=1} f&&/^V:/{print substr($0,3); exit}')
  wget -q -O /tmp/bb.apk $repo/busybox-static-$ver.apk
  mkdir -p /tmp/bb && tar -C /tmp/bb --ignore-zeros -xzf /tmp/bb.apk 2>/dev/null || true
  cp /tmp/bb/bin/busybox.static $ird/bin/busybox
fi
# vendor firmware tools/ extracted from the frame (gitignored): the gslX680's
rm -rf $ird/lib/firmware
# firmware/ is gitignored, so each checkout has its own, and a kernel built
# from one without this file boots without touch (silead_ts probes at 1 s and
# fails for good, the compositor finds no input): three kernels once shipped
# that way from trees that held only the Wi-Fi's blobs. So no touch firmware,
# no build.
[ -f /work/firmware/silead/gsl1680.fw ] || { echo "no firmware/silead/gsl1680.fw: this kernel would boot without touch (run ./build.sh firmware first, docs/firmware.md)"; exit 1; }
if [ -d /work/firmware ]; then mkdir -p $ird/lib && cp -r /work/firmware $ird/lib/firmware; fi
# wireless-regdb's signed regulatory.db: cfg80211 is built in and asks for
# it once, at its init, long before switch_root -- from the rootfs it comes
# too late ("failed to load regulatory.db" on boot-linux25, 2026-10-28) and
# the dongle runs on the world domain. 8 KB, from the pinned Alpine branch
regdb=$(tar -xzOf /tmp/APKINDEX.tar.gz APKINDEX 2>/dev/null | awk '/^P:wireless-regdb$/{f=1} f&&/^V:/{print substr($0,3); exit}') || true
if [ -z "$regdb" ]; then
  wget -q -O /tmp/APKINDEX.tar.gz $ALPINE_MIRROR/$ALPINE_BRANCH/main/armv7/APKINDEX.tar.gz
  regdb=$(tar -xzOf /tmp/APKINDEX.tar.gz APKINDEX | awk '/^P:wireless-regdb$/{f=1} f&&/^V:/{print substr($0,3); exit}')
fi
mkdir -p $dl/apk
[ -f $dl/apk/wireless-regdb-$regdb.apk ] ||
  wget -q -O $dl/apk/wireless-regdb-$regdb.apk $ALPINE_MIRROR/$ALPINE_BRANCH/main/armv7/wireless-regdb-$regdb.apk
mkdir -p $ird/lib/firmware
tar -xzf $dl/apk/wireless-regdb-$regdb.apk -C $ird --warning=no-unknown-keyword \
  lib/firmware/regulatory.db lib/firmware/regulatory.db.p7s 2>/dev/null
[ -f $ird/lib/firmware/regulatory.db.p7s ] || { echo "no regulatory.db from wireless-regdb-$regdb"; exit 1; }
cp /work/initramfs/init $ird/init
cp /work/initramfs/probe $ird/bin/probe && chmod 755 $ird/bin/probe
cp /work/initramfs/pmic-wdt $ird/bin/pmic-wdt && chmod 755 $ird/bin/pmic-wdt
cp /work/initramfs/frameboot $ird/bin/frameboot && chmod 755 $ird/bin/frameboot
# /lib/modules/<release> for the userland (frameboot puts it in the new root):
# this kernel has no modules, and without the directory every modprobe in
# the userland's boot printed "can't change directory to '6.18.57'" on the
# glass and failed, which kept its touch daemon (modprobe uinput) from starting
# (2026-10-27). modules.builtin makes busybox modprobe succeed quietly for a
# built-in driver. It comes out of the kernel build, so it is staged from the
# last build here and the build below runs again if it changed.
rel_dir() { echo $ird/lib/modules/$(cat $obj/include/config/kernel.release 2>/dev/null || echo none); }
stage_builtin() {
  rm -rf $ird/lib/modules
  [ -f $obj/modules.builtin ] || return 0
  mkdir -p "$(rel_dir)"
  cp $obj/modules.builtin $obj/modules.builtin.modinfo "$(rel_dir)/"
  : > "$(rel_dir)/modules.dep"
}
stage_builtin
# busybox-static has no devmem applet, so /bin/devmem is ours (initramfs/devmem.c)
arm-linux-gnueabihf-gcc -static -nostdlib -ffreestanding -fno-stack-protector -Os -s \
  -o $ird/bin/devmem /work/initramfs/devmem.c
arm-linux-gnueabihf-gcc -static -nostdlib -ffreestanding -fno-stack-protector -Os -s \
  -o $ird/bin/i2cxfer /work/initramfs/i2cxfer.c
arm-linux-gnueabihf-gcc -static -nostdlib -ffreestanding -fno-stack-protector -Os -s \
  -o $ird/bin/reboot-to /work/initramfs/reboot-to.c
# the Wi-Fi co-processor from user space (docs/wifi.md)
arm-linux-gnueabihf-gcc -static -nostdlib -ffreestanding -fno-stack-protector -Os -s \
  -o $ird/bin/cp2 /work/initramfs/cp2.c
arm-linux-gnueabihf-gcc -static -nostdlib -ffreestanding -fno-stack-protector -Os -s \
  -o $ird/bin/bt /work/initramfs/bt.c
cp /work/initramfs/cp2test $ird/bin/cp2test && chmod 755 $ird/bin/cp2test
# the USB host-mode and dongle tests (docs/wifi.md)
for t in hosttest wifitest udhcpc-script; do
  rm -f $ird/bin/$t
  [ -f /work/initramfs/$t ] && cp /work/initramfs/$t $ird/bin/$t && chmod 755 $ird/bin/$t
done
# WIFI=1: the USB-dongle Wi-Fi test builds (docs/wifi.md): kernel/wifi.config
# (spare drivers) on top of vt10101.config, and wpa_supplicant, iw, their
# libraries and the dongles' firmware from Alpine (build/wifi-userland.py).
# No credentials: wifitest reads FRAMEBOOT/wpa_supplicant.conf off the eMMC.
# Without WIFI the staging dir is cleaned of all of it, since OBJ dirs are
# reused
rm -rf $ird/lib/ld-musl-armhf.so.1 $ird/lib/lib*.so* $ird/usr $ird/sbin $ird/etc/wifi
wificfg=
if [ -n "${WIFI:-}" ]; then
  ALPINE_MIRROR=$ALPINE_MIRROR ALPINE_BRANCH=$ALPINE_BRANCH python3 /work/build/wifi-userland.py $ird
  wificfg=/work/kernel/wifi.config
fi
write_list() {
  {
    printf 'dir /dev 0755 0 0\nnod /dev/console 0600 0 0 c 5 1\n'
    (cd $ird && find . -mindepth 1 | sort | while read -r p; do
       p=${p#./}
       if [ -d "$p" ]; then echo "dir /$p 0755 0 0"
       else echo "file /$p $ird/$p $(stat -c %a "$p") 0 0"; fi
     done)
    # after /bin: the unpacker makes entries in list order
    echo 'slink /bin/sh busybox 0777 0 0'
  } > $obj/initramfs.list
}
write_list

make -s -C $src O=$obj multi_v7_defconfig
printf 'CONFIG_INITRAMFS_SOURCE="%s"\n' $obj/initramfs.list > $obj/initramfs.config
# The Wi-Fi's firmware inside the kernel: the kernel lives in the boot
# partition both root slots share, so it can't be lost to an A/B update the
# way files pushed to one slot's /lib/firmware are, and native Wi-Fi is the
# only way this frame reaches the network. They're vendor blobs, gitignored,
# so a tree without them still builds, with a kernel that wants /lib/firmware.
: > $obj/firmware.config
if [ -f /work/firmware/wcnmodem.bin ] && [ -f /work/firmware/wifi-nvm.bin ]; then
  rm -rf $obj/fw && mkdir -p $obj/fw/sprd
  cp /work/firmware/wcnmodem.bin /work/firmware/wifi-nvm.bin $obj/fw/sprd/
  printf 'CONFIG_EXTRA_FIRMWARE="sprd/wcnmodem.bin sprd/wifi-nvm.bin"\nCONFIG_EXTRA_FIRMWARE_DIR="%s"\n' $obj/fw > $obj/firmware.config
else
  echo "no firmware/wcnmodem.bin + wifi-nvm.bin: this kernel's Wi-Fi needs them in /lib/firmware/sprd"
fi
$src/scripts/kconfig/merge_config.sh -m -O $obj $obj/.config /work/kernel/vt10101.config $wificfg $obj/initramfs.config $obj/firmware.config
make -s -C $src O=$obj olddefconfig
sed -n 's/^# \(CONFIG_[A-Z0-9_]*\) is not set$/\1=n/p; /^CONFIG_/p' /work/kernel/vt10101.config $wificfg |
while IFS= read -r want; do
  case "$want" in
    *=n) key=${want%=n}; ! grep -q "^$key=" $obj/.config || { echo "still set: $key"; exit 1; } ;;
    *) grep -qx "$want" $obj/.config || { echo "not applied: $want"; exit 1; } ;;
  esac
done

make -s -C $src O=$obj -j"$(nproc)" zImage unisoc/$board.dtb
# a second pass when the built-in list the initramfs carries is stale (a
# config change, or the first build of a tree): only the initramfs relinks
if ! cmp -s $obj/modules.builtin "$(rel_dir)/modules.builtin"; then
  stage_builtin
  echo "modules.builtin changed: restaging the initramfs"
  write_list
  make -s -C $src O=$obj -j"$(nproc)" zImage
fi
mkimage -q -A arm -O linux -T kernel -C none -a 0x80008000 -e 0x80008000 \
  -n "vt10101 $(cat $obj/include/config/kernel.release)" -d $obj/arch/arm/boot/zImage $out/uImage
cp $obj/arch/arm/boot/dts/unisoc/$board.dtb $out/
cp $obj/arch/arm/boot/zImage $out/   # what the boot image carries (cboot boots a zImage)
cat $obj/include/config/kernel.release > $out/kernel.release
ls -l $out/zImage $out/uImage $out/$board.dtb
