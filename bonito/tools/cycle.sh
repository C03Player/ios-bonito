#!/bin/bash
# One unattended test cycle on the bonito (Pixel 3a XL): boot an image, wait for it to finish and restart itself, pull the
# persistent console log, put the phone back in the bootloader for the next cycle.
#
#   bonito/tools/cycle.sh IMAGE LOGNAME [WAIT_SECONDS]      log -> logs/LOGNAME.txt (and logs/LOGNAME.png with SHOT=1)
#
# The image must restart the phone by itself: boot-args sweetreboot=SECS (timed), sweetpanicreset=1 (after a panic) and
# sweetwdt=SECS (watchdog, for hangs); see xnu patch 0150. WAIT_SECONDS is how long to wait for the recovery to appear after
# `fastboot boot` (default 420).
#
# Exit status: 0 ok; 1 the phone never came back (hang that no reset caught: it needs a Power hold); 2 could not reach
# the bootloader to start; 3 the log could not be read.
#
# WAIT_PHONE=SECS (default 240): how long to wait for a phone that is not visible (a hung phone needs a Power hold; a patient
# cycle then starts by itself when it comes back).
#
# Safety: checks `fastboot getvar product` says bonito before anything is written, resets the A/B retry counter
# (`fastboot set_active a`) every cycle, and only ever runs `fastboot boot`/`set_active`/`reboot` (no flashing).
cd "$(dirname "$0")/../.." || exit 2
IMG=${1:?usage: cycle.sh IMAGE LOGNAME [WAIT_SECONDS]}
NAME=${2:?usage: cycle.sh IMAGE LOGNAME [WAIT_SECONDS]}
WAIT=${3:-420}
[ -f "$IMG" ] || { echo "cycle: no image $IMG"; exit 2; }
mkdir -p logs

state() {
	lsusb 2>/dev/null | grep -q "18d1:4ee0" && { echo fastboot; return; }
	lsusb 2>/dev/null | grep -q "18d1:d0b1" && { echo recovery; return; }
	echo none
}

wait_for() {    # wait_for STATE SECONDS
	local i
	for i in $(seq 1 $(($2 / 2))); do
		[ "$(state)" = "$1" ] && return 0
		sleep 2
	done
	return 1
}

say() { echo "cycle [$(date +%H:%M:%S)]: $*"; }

to_bootloader() {
	local s
	s=$(state)
	if [ "$s" = none ]; then
		say "phone not visible; waiting up to ${WAIT_PHONE:-240} s (it may be booting; WAIT_PHONE=SECS makes the cycle patient)"
		for _ in $(seq 1 $((${WAIT_PHONE:-240} / 2))); do s=$(state); [ "$s" != none ] && break; sleep 2; done
	fi
	if [ "$s" = recovery ]; then
		say "in recovery: adb reboot bootloader"
		timeout 20 adb reboot bootloader >/dev/null 2>&1
	fi
	wait_for fastboot 120
}

to_bootloader || { say "cannot reach the bootloader (state $(state))"; exit 2; }
sleep 2
P=$(timeout 15 fastboot getvar product 2>&1 | head -1)
case "$P" in *bonito*) ;; *) say "device is not bonito ($P); stopping"; exit 2;; esac
timeout 20 fastboot set_active a >/dev/null 2>&1
say "booting $IMG"
timeout 120 fastboot boot "$IMG" 2>&1 | tail -2

sleep 25
gotback=0
fb_resets=0
for _ in $(seq 1 $((WAIT / 3))); do
	s=$(state)
	if [ "$s" = recovery ]; then gotback=1; break; fi
	if [ "$s" = fastboot ] && [ $fb_resets -lt 4 ]; then
		# the recovery's own 4-minute timer fired before we read the log, or the boot fell back: go round again
		say "back in fastboot; rebooting into the recovery to read the log"
		timeout 20 fastboot set_active a >/dev/null 2>&1
		timeout 20 fastboot reboot >/dev/null 2>&1
		fb_resets=$((fb_resets + 1))
		sleep 20
	fi
	sleep 3
done
if [ $gotback = 0 ]; then
	say "the phone did not come back within ${WAIT}s: it is hung and no reset caught it"
	exit 1
fi
say "recovery is up"
sleep 4
ok=0
rm -f "logs/$NAME.txt"
for try in 1 2 3; do
	if python3 -I bonito/tools/sweet-log-pull.py "logs/$NAME.txt" 2>&1 | tail -1; then
		[ -s "logs/$NAME.txt" ] && { ok=1; break; }
	fi
	sleep 5
done
if [ "${SHOT:-0}" = 1 ]; then
	python3 -I bonito/tools/sweet-shot-pull.py "logs/$NAME.png" 2>&1 | tail -1
fi
[ $ok = 1 ] || { say "could not read the log"; exit 3; }
say "log: logs/$NAME.txt ($(wc -c < "logs/$NAME.txt") bytes)"
timeout 20 adb reboot bootloader >/dev/null 2>&1
wait_for fastboot 120 && say "phone is in the bootloader, ready for the next cycle" || say "warning: not in fastboot yet"
exit 0
