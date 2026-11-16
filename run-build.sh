#!/bin/bash
# TEMPORARY launcher for bonito/tools/build-ios-root.sh. Finds the USB stick (SanDisk "Cruzer Blade") by model,
# mounts it if it is not mounted, finds the IPSW on it, and starts the build. No arguments needed.
set -euo pipefail
cd "$(dirname "$0")"
[ "$(id -u)" = 0 ] || exec sudo bash "$0" "$@"

DISK=$(lsblk -dno NAME,MODEL | awk 'tolower($0) ~ /cruzer/ {print $1; exit}')
if [ -z "$DISK" ]; then
	echo "No 'Cruzer Blade' USB stick found. Block devices:"; lsblk -o NAME,SIZE,MODEL,MOUNTPOINT; exit 1
fi
SIZE_GB=$(( $(lsblk -bdno SIZE "/dev/$DISK") / 1000000000 ))
[ "$SIZE_GB" -ge 50 ] && [ "$SIZE_GB" -le 70 ] || { echo "/dev/$DISK is ${SIZE_GB} GB, not the 64 GB stick; stopping"; exit 1; }
PART=$(lsblk -lno NAME,TYPE "/dev/$DISK" | awk '$2=="part"{print $1; exit}')
[ -n "$PART" ] || { echo "no partition on /dev/$DISK"; exit 1; }
DEV=/dev/$PART
MP=$(lsblk -no MOUNTPOINT "$DEV" | awk 'NF{print; exit}')
if [ -z "$MP" ]; then
	MP=/mnt/usb
	mkdir -p "$MP"
	mount "$DEV" "$MP"
fi
echo "USB stick: $DEV (${SIZE_GB} GB) mounted at $MP"

IPSW=$(ls "$MP"/ipsw/*20H392*.ipsw 2>/dev/null | head -1 || true)
[ -n "$IPSW" ] || { echo "No 20H392 IPSW in $MP/ipsw/. Contents of $MP:"; ls -la "$MP"; exit 1; }
echo "IPSW: $IPSW"
exec bash bonito/tools/build-ios-root.sh "$IPSW" "$MP/build"
