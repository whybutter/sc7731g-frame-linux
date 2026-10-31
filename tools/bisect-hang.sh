#!/bin/sh
# bisect-hang.sh LOGDIR -- the early-hang bisect (docs/mainline.md, "The early
# hang") as one unattended sitting: wait for Android, probe-boot out-3 with
# one set of kernel arguments, and if the frame comes back to Android by
# itself (the probe's reboot, or the PMIC watchdog armed from U-Boot), go on
# to the next set. Stops at the first boot it can't come back from.
#
# Order matters. Every boot that has touched the eMMC so far left the boot
# ROM unable to read it after a warm reset (`MCI ERF`, then nothing until a
# power cycle), so the boots without the eMMC driver go first and the one
# with it goes last.
#
# Run from the repository root with the frame on USB (adb) and the UART adapter on
# the pads, after a power cycle.
set -u
L=${1:?usage: bisect-hang.sh LOGDIR}
mkdir -p "$L"
BASE="console=ttyS1,115200n8 earlycon ignore_loglevel panic=5 printk.devkmsg=on wdt probe_reboot"
NOMMC="initcall_blacklist=sc7731g_sdhci_driver_init"
NODISP="initcall_blacklist=sc7731g_sdhci_driver_init,sc7731g_dispc_driver_init,lima_platform_driver_init"

# name|extra args. "nommc" without the sysctl the out-3 boots carried
# (sysctl.kernel.panic_on_rcu_stall=1, handled by do_sysctl_args() right
# where they hung; with it, the eMMC-less boot of 2026-10-27 hung at the same
# spot, so that step is done); "nodisp" also drops the
# display and GPU; "mmc" is the eMMC driver alone with the rest -- last.
STEPS="nommc|$NOMMC
nodisp|$NODISP clk_ignore_unused
mmc|"

wait_android() {
  timeout "$1" adb wait-for-device && sleep 45
}

echo "$STEPS" | while IFS='|' read -r name extra; do
  echo "=== $name: waiting for Android"; date
  if ! wait_android 300; then
    echo "=== $name: no Android -- the previous boot didn't come back; power-cycle the frame"
    exit 1
  fi
  python3 tools/uboot-boot.py --push out-3 --wdt 60 --seconds 150 \
    --until "probe: rebooting" --after 15 --args "$BASE $extra" \
    "$L/$name.log" > "$L/$name.out" 2>&1
  rc=$?
  if tr -d '\r\0' < "$L/$name.log" | grep -aq "probe: done"; then
    echo "=== $name: SURVIVED (probe done, uboot-boot rc $rc)"
  else
    echo "=== $name: HUNG or failed (uboot-boot rc $rc); last kernel line:"
    tr -d '\r\0' < "$L/$name.log" | grep -a '^\[' | tail -1
  fi
done
echo "=== bisect finished"; date
