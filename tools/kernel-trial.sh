#!/bin/bash
# kernel-trial.sh SSH-TARGET try BOOT.IMG | promote | status
#
# A new boot image for the VT10101 without the UART as the safety net
# (most people won't keep one attached). The root filesystem has A/B
# slots; the kernel doesn't -- cboot boots the one boot partition (p16) --
# but it has a second bootable partition: recovery (p21), which cboot boots
# for exactly one boot when the PMIC reset reason says recovery (0x20, what
# our restart handler writes for `reboot recovery`; docs/install-plan.md).
# The next reset is a normal one again, back to p16.
#
#   try      write BOOT.IMG to recovery, verify it, reboot into it once.
#            If it never comes up, a power cycle (or the PMIC watchdog, if
#            the userland's watchdog had it armed) lands on p16, the known kernel.
#   promote  run on a frame that came up on the trial kernel: copy recovery
#            to boot (p16), verify. From then on it's the kernel.
#   status   which kernel runs, and what the trial file says.
#
# State lives in FRAMEBOOT/kernel-trial.env (the FAT, read by both slots):
# the image's size and md5, and `uname -v` of the kernel it was tried from,
# which is how promote knows the trial kernel is the one running. Stock
# Android's recovery.img was in p21; keep your own backup of it
# (docs/install-plan.md).
set -euo pipefail
target=${1:?usage: kernel-trial.sh SSH-TARGET try BOOT.IMG | promote | status}
cmd=${2:?usage: kernel-trial.sh SSH-TARGET try BOOT.IMG | promote | status}
sshf() { ssh -o BatchMode=yes -o ConnectTimeout=10 "$target" "$@"; }
# p NAME on the frame: the block device of a GPT partition, by name
# (true at the end: the loop's last grep fails, and set -e would stop on it)
part='for b in /sys/class/block/mmcblk0p*; do grep -qx "PARTNAME=$1" $b/uevent && echo /dev/${b##*/}; done; true'

case $cmd in
try)
  img=${3:?try needs a boot image}
  size=$(stat -f %z "$img"); md5=$(md5 -q "$img")
  [ "$size" -le $((16 * 1024 * 1024)) ] || { echo "$img: $size bytes, the partitions are 16 MiB"; exit 1; }
  sshf 'cat > /tmp/kernel-trial.img' < "$img"
  sshf "set -e
    p() { $part; }
    rec=\$(p recovery); [ -b \"\$rec\" ] || { echo 'no recovery partition'; exit 1; }
    [ \"\$(md5sum < /tmp/kernel-trial.img | cut -d' ' -f1)\" = $md5 ] || { echo 'copy corrupt'; exit 1; }
    dd if=/tmp/kernel-trial.img of=\$rec bs=1M conv=fsync 2>/dev/null; sync
    echo 3 > /proc/sys/vm/drop_caches
    [ \"\$(head -c $size \$rec | md5sum | cut -d' ' -f1)\" = $md5 ] || { echo \"\$rec readback differs\"; exit 1; }
    # from = p16's kernel, the one promote must not be running. A try run
    # from a trial kernel (a pending trial whose from isn't this kernel)
    # keeps the old from: recording the trial kernel there made promote
    # refuse once the next trial booted (seen going from trial #4 to #5)
    from=\$(uname -v)
    if [ -f /boot/kernel-trial.env ]; then
      old=\$(. /boot/kernel-trial.env; echo \"\$from\")
      [ -z \"\$old\" ] || [ \"\$old\" = \"\$from\" ] || { from=\$old; echo \"running a trial kernel: p16 is still '\$from'\"; }
    fi
    printf \"size=%s\\nmd5=%s\\nfrom='%s'\\n\" $size $md5 \"\$from\" > /boot/kernel-trial.env; sync
    rm -f /tmp/kernel-trial.img
    echo \"written to \$rec and verified; rebooting into it once\"
    # reboot(2) RESTART2 \"recovery\": the PMIC reason cboot reads. Synced, not
    # a clean shutdown -- busybox reboot takes no argument
    setsid python3 -c 'import ctypes,os,time; time.sleep(1); os.sync(); ctypes.CDLL(None,use_errno=True).syscall(88,ctypes.c_uint(0xfee1dead),ctypes.c_uint(672274793),ctypes.c_uint(0xa1b2c3d4),b\"recovery\")' </dev/null >/dev/null 2>&1 &"
  ;;
promote)
  sshf "set -e
    p() { $part; }
    . /boot/kernel-trial.env
    [ \"\$(uname -v)\" != \"\$from\" ] || { echo 'still on the old kernel: the trial did not boot (or was the same build)'; exit 1; }
    rec=\$(p recovery); boot=\$(p boot)
    [ \"\$(head -c \$size \$rec | md5sum | cut -d' ' -f1)\" = \$md5 ] || { echo 'recovery no longer holds the trial image'; exit 1; }
    head -c \$size \$rec > /tmp/kernel-trial.img
    dd if=/tmp/kernel-trial.img of=\$boot bs=1M conv=fsync 2>/dev/null; sync
    echo 3 > /proc/sys/vm/drop_caches
    [ \"\$(head -c \$size \$boot | md5sum | cut -d' ' -f1)\" = \$md5 ] || { echo \"\$boot readback differs\"; exit 1; }
    rm -f /tmp/kernel-trial.img /boot/kernel-trial.env; sync
    echo \"promoted: \$boot now holds \$md5 (\$(uname -v))\""
  ;;
status)
  sshf 'echo "running: $(uname -v)"; cat /boot/kernel-trial.env 2>/dev/null || echo "no trial pending"'
  ;;
*) echo "unknown command $cmd"; exit 1 ;;
esac
