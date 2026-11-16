#!/bin/bash
# Flash a raw HFS+ root image to bonito's userdata partition (this ERASES the phone's Android data).
#   bonito/tools/flash-userdata.sh IMAGE [--yes]
# Checks before writing: the device is a bonito in the bootloader (not fastbootd), unlocked, the image is a valid HFS+ volume
# (signature 'H+') and, if IMAGE.sha256 exists, matches it. fastboot splits big raw images into sparse chunks by itself.
set -euo pipefail
IMG=${1:?usage: $0 IMAGE [--yes]}
YES=${2:-}
[ -f "$IMG" ] || { echo "no such image: $IMG"; exit 1; }

P=$(timeout 10 fastboot getvar product 2>&1 | head -1)
case "$P" in *bonito*) ;; *) echo "device is not a bonito in fastboot ($P): stopping"; exit 1;; esac
timeout 10 fastboot getvar is-userspace 2>&1 | grep -q "no" || { echo "phone is in fastbootd; run 'fastboot reboot bootloader' first"; exit 1; }
timeout 10 fastboot getvar unlocked 2>&1 | grep -q "yes" || { echo "bootloader is locked: stopping"; exit 1; }

sig=$(dd if="$IMG" bs=1 skip=1024 count=2 2>/dev/null | od -An -c | tr -d ' ')
[ "$sig" = "H+" ] || [ "$sig" = "HX" ] || { echo "$IMG has no HFS+ volume header (got '$sig')"; exit 1; }
if [ -f "$IMG.sha256" ]; then
	(cd "$(dirname "$IMG")" && sha256sum -c "$(basename "$IMG").sha256") || { echo "checksum mismatch: stopping"; exit 1; }
fi
SZ=$(stat -c %s "$IMG")
echo "image: $IMG ($((SZ / 1048576)) MiB) -> bonito userdata (this erases Android's data)"
if [ "$YES" != "--yes" ]; then
	read -r -p "type 'yes' to flash: " a; [ "$a" = "yes" ] || { echo "cancelled"; exit 1; }
fi
time fastboot flash userdata "$IMG"
echo "flashed. Boot XNU with rd=ufsroot0 to mount it."
