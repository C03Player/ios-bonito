#!/bin/bash
# Boot a CI kernel for a person to use by hand: no timed reset, no panic reset, no watchdog (the phone stays up until it is
# turned off; if it panics or hangs it stays that way, hold Power to restart).
#
#   bonito/tools/boot-manual.sh RUN_ID [boot-args]      default boot-args: sweeths200=6 (the fast eMMC mode)
#
# Checks product == bonito, then only `fastboot set_active a` + `fastboot boot` (nothing is flashed). The phone must be in the bootloader
# (hold Power + Volume Down, or `adb reboot bootloader` from the recovery).
cd "$(dirname "$0")/../.." || exit 2
RUN=${1:?usage: boot-manual.sh RUN_ID [boot-args]}
ARGS=${2:-sweeths200=6}
D=work/ci-$RUN
if [ ! -f "$D/kernel.development.sweet" ]; then
	rm -rf "$D"
	bonito/tools/fetch-artifact.sh C03Player/ios-bonito "$RUN" xnu-kernel "$D" >/dev/null 2>&1 || { echo "no artifact for run $RUN"; exit 5; }
	(cd "$D" && unzip -qo xnu-kernel.zip kernel.development.sweet) || { echo "cannot unzip"; exit 5; }
fi
AUTORESET= bonito/tools/mkios.sh "$D/kernel.development.sweet" "manual-$RUN" "$ARGS" | tail -1
P=$(timeout 15 fastboot getvar product 2>&1 | head -1)
case "$P" in *bonito*) ;; *) echo "device is not bonito ($P); stopping"; exit 2;; esac
timeout 20 fastboot set_active a >/dev/null 2>&1
timeout 120 fastboot boot "bonito/shim/out/manual-$RUN.img" 2>&1 | tail -2
