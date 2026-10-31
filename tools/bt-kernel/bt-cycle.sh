#!/bin/sh
# bt-cycle.sh [ROUNDS] -- on the VT10101, detached (setsid, log in
# /tmp/bt-cycle.log): does Bluetooth (hci0, bt-sc7731g) survive the Wi-Fi
# going down and up on the same cp2, and the reverse? wlan0 is the frame's
# only network, so whatever happens this ends with wlan0 reaching the
# gateway, or with a reboot -- a plain one lands on p16, the known kernel.
#
# Per round:
#   A. wlan0 down, hci0 must answer at +2 and +30 s (cp2 kept up by BT
#      alone: the deaf trap of wifi.md wifi7 #5 measured with DEV_OPEN only)
#   B. wlan0 up (DEV_OPEN on a cp2 that never stopped), the gateway must
#      answer within 90 s and hci0 still answer
#   C. hci0 down and up with wlan0 up: the gateway must still answer
rounds=${1:-3}
log=/tmp/bt-cycle.log
gw=$(ip route | sed -n 's/^default via \([0-9.]*\).*/\1/p' | head -1)
here=$(dirname "$0")
say() { echo "$(date +%T) $*" >> $log; }
hci() { r=$(python3 $here/hciping.py 0 2>&1); say "  $1: $r"; case $r in *" ok "*) return 0;; esac; return 1; }
net_back() {
	ip link set wlan0 up 2>>$log
	for i in $(seq 1 45); do
		ping -c1 -W2 $gw >/dev/null 2>&1 && { say "  gateway answers after ~$((i * 2)) s"; return 0; }
		sleep 2
	done
	return 1
}
rescue() {
	say "RESCUE: $*"
	net_back && return 0
	say "RESCUE: no gateway with wlan0 up; rebooting (p16)"
	sync; dmesg | tail -60 >> $log; sync
	reboot -f
}
: > $log
say "start: gateway $gw, $rounds rounds, $(uname -v)"
bluetoothctl scan off >/dev/null 2>&1
hci "before" || say "  (hci0 not answering before the test)"
fail=0
for n in $(seq 1 $rounds); do
	say "round $n"
	ip link set wlan0 down; say "  A wlan0 down"
	sleep 2; hci "A +2s" || fail=$((fail + 1))
	sleep 28; hci "A +30s" || fail=$((fail + 1))
	say "  A inquiry: $(python3 $here/inq.py 4 2>&1 | tail -1)"
	say "  B wlan0 up"
	net_back || { rescue "round $n: gateway gone after wlan0 up"; fail=$((fail + 1)); }
	hci "B" || fail=$((fail + 1))
	say "  B inquiry: $(python3 $here/inq.py 4 2>&1 | tail -1)"
	say "  C hci0 down/up"
	# bluetoothctl, not btmgmt: btmgmt 5.82 without a terminal can sit in
	# epoll forever waiting for a reply it has already had (#4, 2026-10-30)
	timeout 15 bluetoothctl power off >/dev/null 2>&1
	sleep 3
	timeout 15 bluetoothctl power on >/dev/null 2>&1
	sleep 3
	hci "C" || fail=$((fail + 1))
	ping -c3 -W2 $gw >/dev/null 2>&1 && say "  C gateway ok" ||
		{ rescue "round $n: gateway gone after hci0 down/up"; fail=$((fail + 1)); }
done
say "done: $fail failures"
dmesg | grep -iE 'bt-sc7731g|sprdwl|wcn|bluetooth' | tail -40 >> $log
