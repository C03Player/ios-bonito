#!/bin/bash
# One test boot of the iOS USB Ethernet gadget (boot-arg sweetusb=2) with the host watching the USB bus for it.
#   bonito/tools/usb-test.sh [KERNEL_RUN] [NAME] [extra boot-args]      defaults: 45029003673 usbt ""
# The phone must be in the bootloader. This BOOTS iOS (fastboot boot): run it only when the owner has agreed.
# Output: logs/NAME.txt (the iOS console log with the "SWEET USB:" lines) and logs/NAME-lsusb.txt (when 1209:0001 was or was not seen).
# If the gadget enumerates, usbbridge.py ping runs at once and its result is appended to logs/NAME-lsusb.txt.
cd "$(dirname "$0")/../.." || exit 2
KR=${1:-45029003673}; NAME=${2:-usbt}; EXTRA=${3:-}
W=logs/$NAME-lsusb.txt
: > "$W"
( seen=0; start=$(date +%s)
  while true; do
	if lsusb 2>/dev/null | grep -q "1209:0001"; then
		if [ $seen = 0 ]; then
			seen=1; echo "$(( $(date +%s) - start )) s: 1209:0001 appeared" >> "$W"; lsusb | grep "1209:0001" >> "$W"
			for i in 1 2 3; do timeout 40 python3 -I bonito/tools/usbbridge.py ping 3 >> "$W" 2>&1 && break; sleep 2; done
		fi
	elif [ $seen = 1 ]; then seen=0; echo "$(( $(date +%s) - start )) s: 1209:0001 went away" >> "$W"; fi
	sleep 0.5
  done ) &
WATCH=$!
trap 'kill $WATCH 2>/dev/null' EXIT
USB=2 bonito/tools/ci-cycle.sh "$KR" "$NAME" "sweetreboot=200 $EXTRA" 420 "sweet_usb"
RC=$?
kill $WATCH 2>/dev/null
echo "--- usb watch:"; cat "$W"; [ -s "$W" ] || echo "1209:0001 never appeared on the host's USB bus"
echo "--- SWEET USB lines of the iOS log:"; grep -a "SWEET USB" "logs/$NAME.txt" 2>/dev/null | head -60 | cut -c1-200
exit $RC
