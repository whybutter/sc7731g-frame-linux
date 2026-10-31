#!/bin/sh
# green.sh -- on the VT10101 after a trial boot of a Bluetooth kernel: the
# checks a kernel must pass before promote (2026-10-30: not just BT and
# Wi-Fi -- #57-#59 were promoted with no touch). One line per check,
# PASS/FAIL, and `green` or `NOT green` at the end. Writes the PSKEY by hand
# when sc7731g-bt didn't, so hci0 exists to test.
#
# Optional: WLAN_IP and WLAN_MAC, what wlan0 should have; UI_CHECK, a
# command that says whether your UI is healthy (default: cage is running).
here=$(dirname "$0")
bad=0
ok() { echo "PASS $1"; }
no() { echo "FAIL $1"; bad=$((bad + 1)); }
chk() { d=$1; shift; if "$@" >/dev/null 2>&1; then ok "$d"; else no "$d"; fi; }

echo "kernel: $(uname -v)"
chk "silead_ts: no failure in dmesg" sh -c '! dmesg | grep -i silead | grep -qiE "fail|error"'
chk "silead_ts: probed" sh -c 'dmesg | grep -qi silead'
chk "/dev/input/event* exists" sh -c 'ls /dev/input/event*'
chk "UI healthy (${UI_CHECK:-pidof cage})" sh -c "${UI_CHECK:-pidof cage}"
chk "no /lib/firmware/sprd needed (image firmware)" sh -c '[ ! -e /lib/firmware/sprd/wcnmodem.bin ] || dmesg | grep -q "loaded .* of sprd/wcnmodem.bin"'
chk "wlan0 has an address${WLAN_IP:+ ($WLAN_IP)}" sh -c "ip addr show wlan0 | grep -q \"inet ${WLAN_IP:-}\""
[ -z "${WLAN_MAC:-}" ] || chk "wlan0 is $WLAN_MAC" grep -qi "$WLAN_MAC" /sys/class/net/wlan0/address
gw=$(ip route | sed -n 's/^default via \([0-9.]*\).*/\1/p' | head -1)
chk "gateway answers over wlan0" ping -c3 -W2 $gw
chk "a ~1 MB download over wlan0 (Alpine's APKINDEX)" wget -q -O /dev/null https://dl-cdn.alpinelinux.org/alpine/v3.22/main/armv7/APKINDEX.tar.gz
chk "sound card present" sh -c 'grep -q . /proc/asound/cards && [ -e /dev/snd/pcmC0D0p ]'
chk "sound plays (1 s of silence through plughw:0,0)" sh -c 'head -c 176400 /dev/zero | aplay -q -D plughw:0,0 -f cd -t raw'

# Bluetooth
p=$(ls -d /sys/bus/platform/drivers/bt-sc7731g/*/ 2>/dev/null | head -1)
chk "bt-sc7731g bound" test -n "$p"
if [ -n "$p" ] && [ ! -e /sys/class/bluetooth/hci0 ]; then
	echo "     (no hci0: writing the PSKEY by hand)"
	/usr/lib/sc7731g-bt/pskey.sh /run/sc7731g-bt/pskey.bin && cat /run/sc7731g-bt/pskey.bin > ${p}pskey
	sleep 3
fi
chk "hci0 registered" test -e /sys/class/bluetooth/hci0
timeout 15 bluetoothctl power on >/dev/null 2>&1; sleep 2
r=$(python3 $here/hciping.py 0); echo "     $r"
case $r in *" ok "*) ok "hci0 answers";; *) no "hci0 answers";; esac
# an inquiry must complete (one that never did, with wlan0 associated, is
# what kept #4 from promotion); results depend on what's discoverable nearby
r=$(python3 $here/inq.py 4 2>&1 | tail -1); echo "     $r"
case $r in "inquiry ok"*) ok "an inquiry completes";; *) no "an inquiry completes";; esac
dmesg | grep -iE 'bt-sc7731g|PSKEY' | tail -5 | sed 's/^/     /'
[ $bad = 0 ] && echo green || echo "NOT green: $bad"
