#!/bin/bash
# Run on the Pi HOST as root. Put the userland GPU stack (SweetMetal2.bundle from the CI artifact "userland-gpu") into the host copy of the iOS root
# image; flash the image afterwards with bonito/tools/flash-userdata.sh (about 20 min, ERASES the iOS data: the whole root is rewritten).
#   sudo bash bonito/tools/install-userland.sh SweetMetal2.bundle.tar.gz [IMAGE] [--extra DIR]
#   IMAGE defaults to /usb/build/ios16-root-20H392.img. --extra DIR: also copy DIR's files (vktest, m2vtest, ...) to /private/var/local/bin/.
# Needs the hfsplus kernel module. The image is mounted read-write (it has no journal until iOS first mounts it), checked with fsck.hfsplus
# afterwards. Never touches the phone.
set -euo pipefail
[ "$(id -u)" = 0 ] || exec sudo bash "$0" "$@"
TAR=${1:?usage: sudo bash $0 SweetMetal2.bundle.tar.gz [IMAGE] [--extra DIR]}
shift
IMG=/usb/build/ios16-root-20H392.img
EXTRA=
while [ $# -gt 0 ]; do
	case "$1" in
	--extra) EXTRA=$2; shift 2;;
	*) IMG=$1; shift;;
	esac
done
[ -f "$TAR" ] || { echo "no such file: $TAR"; exit 1; }
[ -f "$IMG" ] || { echo "no such image: $IMG"; exit 1; }
MP=$(mktemp -d /tmp/iosroot.XXXXXX)
LOOP=
cleanup() { mountpoint -q "$MP" && umount "$MP"; [ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null; rmdir "$MP" 2>/dev/null; true; }
trap cleanup EXIT
modprobe loop hfsplus 2>/dev/null || true
LOOP=$(losetup --find --show "$IMG")
mount -t hfsplus -o rw,force "$LOOP" "$MP"
mount | grep " $MP " | grep -q "[(,]rw[,)]" || { echo "the image did not mount read-write (journal?)"; exit 1; }
EXT=$MP/System/Library/Extensions
[ -d "$EXT" ] || { echo "not an iOS root: no $EXT"; exit 1; }
echo "== installing SweetMetal2.bundle into $EXT"
rm -rf "$EXT/SweetMetal2.bundle"
tar -xzf "$TAR" -C "$EXT"
chown -R 0:0 "$EXT/SweetMetal2.bundle"
find "$EXT/SweetMetal2.bundle" -type d -exec chmod 755 {} +
find "$EXT/SweetMetal2.bundle" -type f -exec chmod 644 {} +
chmod 755 "$EXT/SweetMetal2.bundle/SweetMetal2" 2>/dev/null || true
ls -la "$EXT/SweetMetal2.bundle"
echo "== /private/var/local (logs, pipeline cache, per-process options of the plugin)"
L=$MP/private/var/local
mkdir -p "$L/m2v-cache" "$L/bin"
chown 0:0 "$L" "$L/bin"; chmod 755 "$L" "$L/bin"
chmod 777 "$L/m2v-cache"
: >> "$L/mtl2.log"; chmod 666 "$L/mtl2.log"
if [ -n "$EXTRA" ]; then
	for f in "$EXTRA"/*; do [ -f "$f" ] && install -m 755 -o 0 -g 0 "$f" "$L/bin/$(basename "$f")"; done
	ls -la "$L/bin"
fi
sync
umount "$MP"
losetup -d "$LOOP"; LOOP=
echo "== fsck"
LOOP=$(losetup --find --show "$IMG")
fsck.hfsplus -f "$LOOP" 2>&1 | tail -4
losetup -d "$LOOP"; LOOP=
echo "done: flash with bonito/tools/flash-userdata.sh $IMG"
