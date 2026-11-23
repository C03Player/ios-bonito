#!/bin/bash
# Boot iOS from the phone itself (slot B), without the PC: see docs/STANDALONE.md.
#
#   bonito/tools/standalone.sh flash [IMAGE]   write the iOS boot image to boot_b (default /usb/work/standalone/ios-standalone3.img)
#   bonito/tools/standalone.sh arm [--boot]    make slot B the active slot (3 boots), optionally reboot into it now
#   bonito/tools/standalone.sh disarm          make slot A (the Linux recovery the tools use) the active slot again
#
# The phone must be in the bootloader (`adb reboot bootloader` from the recovery, or hold Power + Volume Down).
# SAFETY: this script only ever runs `fastboot flash boot_b`, `fastboot set_active` and `fastboot reboot`. It never writes a firmware
# partition (xbl, abl, tz, hyp, aop, ...), never touches slot A, and never edits the partition table: nothing here can take fastboot away.
set -euo pipefail
CMD=${1:?usage: standalone.sh flash [IMAGE] | arm [--boot] | disarm}
P=$(timeout 15 fastboot getvar product 2>&1 | head -1)
case "$P" in *bonito*) ;; *) echo "standalone: not a bonito ($P); stopping"; exit 2;; esac
timeout 15 fastboot getvar unlocked 2>&1 | grep -q yes || { echo "standalone: bootloader is locked; stopping"; exit 2; }
case "$CMD" in
flash)
	IMG=${2:-/usb/work/standalone/ios-standalone3.img}
	[ "$(stat -c %s "$IMG")" = 67108864 ] || { echo "standalone: $IMG is not a 64 MiB boot image"; exit 2; }
	timeout 300 fastboot flash boot_b "$IMG"
	;;
arm)
	timeout 20 fastboot set_active b
	echo "slot B active: retry count $(timeout 10 fastboot getvar slot-retry-count:b 2>&1 | head -n 1 | sed 's/.*: //') boots left"
	[ "${2:-}" = "--boot" ] && timeout 20 fastboot reboot
	;;
disarm)
	timeout 20 fastboot set_active a
	;;
*)
	echo "usage: standalone.sh flash [IMAGE] | arm [--boot] | disarm"; exit 2;;
esac
