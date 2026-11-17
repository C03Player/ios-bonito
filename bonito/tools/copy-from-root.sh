#!/bin/bash
# Run on the Pi HOST as root. Copy a few paths out of the iOS root image (mounted READ-ONLY) into <repo>/work/rootcopy/ so they can be inspected.
#   sudo bash bonito/tools/copy-from-root.sh PATH_IN_IMAGE [PATH_IN_IMAGE ...]    (refuses more than 100 MB in total)
set -uo pipefail
[ "$(id -u)" = 0 ] || exec sudo bash "$0" "$@"
[ $# -ge 1 ] || { echo "usage: sudo bash $0 /System/Library/... [...]"; exit 1; }
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
DEST=$ROOT/work/rootcopy
DISK=$(lsblk -dno NAME,MODEL | awk 'tolower($0) ~ /cruzer/ {print $1; exit}')
PART=$(lsblk -lno NAME,TYPE "/dev/$DISK" | awk '$2=="part"{print $1; exit}')
MP=$(lsblk -no MOUNTPOINT "/dev/$PART" | awk 'NF{print; exit}')
[ -n "$MP" ] || { MP=/mnt/usb; mkdir -p "$MP"; mount "/dev/$PART" "$MP"; }
IMG=$MP/build/ios16-root-20H392.img
modprobe loop hfsplus 2>/dev/null || true
MNT=$(mktemp -d /tmp/rootcopy.XXXXXX)
cleanup() { mountpoint -q "$MNT" && umount "$MNT"; rmdir "$MNT" 2>/dev/null; }
trap cleanup EXIT
mount -t hfsplus -o ro,loop,force "$IMG" "$MNT" || { echo "mount failed"; exit 1; }
mkdir -p "$DEST"
total=0
for p in "$@"; do
	[ -e "$MNT$p" ] || { echo "missing in image: $p"; continue; }
	sz=$(du -sb "$MNT$p" | cut -f1); total=$((total + sz))
	[ "$total" -le $((100 * 1024 * 1024)) ] || { echo "over 100 MB: stopping at $p"; break; }
	mkdir -p "$DEST$(dirname "$p")"
	cp -a "$MNT$p" "$DEST$(dirname "$p")/"
	echo "copied $p ($sz bytes)"
done
( cd "$DEST" && find . -type f -exec sha256sum {} + > "$ROOT/logs/rootcopy.sha256" 2>/dev/null )
echo "copies are in $DEST ; checksums in logs/rootcopy.sha256"
