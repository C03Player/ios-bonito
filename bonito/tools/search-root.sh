#!/bin/bash
# Run on the Pi HOST as root. Mount the iOS root image READ-ONLY and search it for a name, to debug what the phone's iOS cannot find.
#   sudo bash bonito/tools/search-root.sh PATTERN [IMAGE]
# Finds the USB stick ("Cruzer Blade") and the image if IMAGE is not given. Results go to <repo>/logs/search-root.txt.
set -uo pipefail
[ "$(id -u)" = 0 ] || exec sudo bash "$0" "$@"
PATTERN=${1:?usage: sudo bash $0 PATTERN [IMAGE]}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$ROOT/logs/search-root.txt
IMG=${2:-}
if [ -z "$IMG" ]; then
	DISK=$(lsblk -dno NAME,MODEL | awk 'tolower($0) ~ /cruzer/ {print $1; exit}')
	PART=$(lsblk -lno NAME,TYPE "/dev/$DISK" | awk '$2=="part"{print $1; exit}')
	MP=$(lsblk -no MOUNTPOINT "/dev/$PART" | awk 'NF{print; exit}')
	[ -n "$MP" ] || { MP=/mnt/usb; mkdir -p "$MP"; mount "/dev/$PART" "$MP"; }
	IMG=$MP/build/ios16-root-20H392.img
fi
[ -f "$IMG" ] || { echo "image not found: $IMG"; exit 1; }
modprobe loop hfsplus 2>/dev/null || true
MNT=$(mktemp -d /tmp/rootsearch.XXXXXX)
cleanup() { mountpoint -q "$MNT" && umount "$MNT"; rmdir "$MNT" 2>/dev/null; }
trap cleanup EXIT
mount -t hfsplus -o ro,loop,force "$IMG" "$MNT" || { echo "mount failed"; exit 1; }
mkdir -p "$ROOT/logs"
{
	echo "=== search for '$PATTERN' in $IMG at $(date -u +%FT%TZ)"
	echo "--- file/dir names:"
	timeout 900 find "$MNT" -iname "*$PATTERN*" 2>/dev/null | sed "s#^$MNT##" | head -100
	echo "--- files containing it (System/Library, usr, private/preboot):"
	for d in System/Library usr private/preboot; do
		[ -d "$MNT/$d" ] && timeout 1500 grep -rIl --binary-files=text -a "$PATTERN" "$MNT/$d" 2>/dev/null | sed "s#^$MNT##" | head -60
	done
	echo "=== done $(date -u +%FT%TZ)"
} 2>&1 | tee "$OUT"
