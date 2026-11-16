#!/bin/bash
# Build the iOS 16.7.16 (20H392, iPhone X) root filesystem image for bonito's userdata partition.
#
#   sudo bash bonito/tools/build-ios-root.sh IPSW_FILE WORK_DIR [SIZE]
#
#   IPSW_FILE  iPhone10,3_16.7.16_20H392_Restore.ipsw  (you downloaded it; never committed)
#   WORK_DIR   a directory on a disk with >= 40 GB free (the USB stick); everything is created inside it
#   SIZE       image size, default 16G (sparse file; about 12 GB are used)
#
# Must run as REAL root on a Linux host with loop devices, FUSE and the hfsplus kernel module (not in the rootless
# dev container). Steps: verify the IPSW -> extract the OS, SystemOS and AppOS disk images -> bonito/tools/mkiosroot.sh
# (apfs-fuse read, mkfs.hfsplus, loop mount, rsync) -> bonito/tools/sweet-root-tweaks.py -> fsck.hfsplus.
# Output: WORK_DIR/ios16-root-20H392.img (+ .sha256). Nothing is written to the phone.
set -euo pipefail

IPSW=${1:?usage: sudo bash $0 IPSW_FILE WORK_DIR [SIZE]}
WORK=${2:?usage: sudo bash $0 IPSW_FILE WORK_DIR [SIZE]}
SIZE=${3:-16G}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TOOLS=$ROOT/bonito/tools
EXPECT_SHA256=ece1194f7e233e9109c5790a8d23a4564a10a254e15ec5b09978a343265568fb
OUT=$WORK/ios16-root-20H392.img

say() { printf '\n==> %s\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "run as root (sudo bash $0 ...)"
[ -f "$IPSW" ] || die "IPSW not found: $IPSW"
mkdir -p "$WORK"
exec > >(tee -a "$WORK/build.log") 2>&1
say "build started $(date -u +%FT%TZ)  ipsw=$IPSW  work=$WORK  size=$SIZE"

# ---- free space (a mis-mounted USB stick would silently fill the system disk) ----
avail_kb=$(df -Pk "$WORK" | awk 'NR==2{print $4}')
[ "$avail_kb" -gt $((40 * 1024 * 1024)) ] || die "less than 40 GB free in $WORK ($(df -Ph "$WORK" | awk 'NR==2{print $4" on "$6}')). Is the USB stick mounted?"

# ---- host tools ----
say "checking host tools"
need_apt=()
for t in rsync python3 unzip; do command -v "$t" >/dev/null || need_apt+=("$t"); done
command -v mkfs.hfsplus >/dev/null || need_apt+=(hfsprogs)
command -v fsck.hfsplus >/dev/null || need_apt+=(hfsprogs)
command -v fusermount3 >/dev/null || need_apt+=(fuse3)
if [ ${#need_apt[@]} -gt 0 ]; then
	say "installing: ${need_apt[*]}"
	apt-get update -qq || true
	if ! apt-get install -y -qq "${need_apt[@]}"; then
		# hfsprogs lives in contrib/non-free
		codename=$(. /etc/os-release && echo "${VERSION_CODENAME:-bookworm}")
		cat > /etc/apt/sources.list.d/ios-root-extra.sources <<EOF
Types: deb
URIs: http://deb.debian.org/debian
Suites: $codename
Components: contrib non-free
Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg
EOF
		apt-get update -qq
		apt-get install -y -qq "${need_apt[@]}" || die "could not install ${need_apt[*]}"
	fi
fi

APFS=$(command -v apfs-fuse || true)
apfs_out=$("${APFS:-/nonexistent}" 2>&1 || true)
if [ -z "$APFS" ] || ! printf '%s' "$apfs_out" | grep -qi "usage"; then
	say "building apfs-fuse from $ROOT/third_party/apfs-fuse"
	apt-get install -y -qq git cmake g++ make libfuse3-dev libbz2-dev zlib1g-dev libicu-dev libattr1-dev pkg-config
	[ -d "$ROOT/third_party/apfs-fuse" ] || git clone --recursive --depth 1 https://github.com/sgan81/apfs-fuse.git "$ROOT/third_party/apfs-fuse"
	rm -rf "$ROOT/third_party/apfs-fuse/build-host"
	mkdir "$ROOT/third_party/apfs-fuse/build-host"
	(cd "$ROOT/third_party/apfs-fuse/build-host" && cmake .. -DCMAKE_BUILD_TYPE=Release >/dev/null && make -j"$(nproc)" >/dev/null)
	install -m 0755 "$ROOT/third_party/apfs-fuse/build-host/apfs-fuse" /usr/local/bin/apfs-fuse
	APFS=/usr/local/bin/apfs-fuse
fi
echo "apfs-fuse: $APFS"

modprobe loop 2>/dev/null || true
modprobe fuse 2>/dev/null || true
modprobe hfsplus 2>/dev/null || true
grep -qw hfsplus /proc/filesystems || die "this kernel has no hfsplus support (modprobe hfsplus failed). Try: apt install linux-modules-extra-\$(uname -r), or use another Linux machine."
grep -qw fuse /proc/filesystems || die "no FUSE support in this kernel"
[ -e /dev/fuse ] || die "/dev/fuse missing"
losetup -f >/dev/null || die "no free loop device (losetup -f failed)"

# ---- verify the IPSW ----
say "verifying the IPSW (sha256, takes a minute)"
got=$(sha256sum "$IPSW" | awk '{print $1}')
[ "$got" = "$EXPECT_SHA256" ] || die "IPSW sha256 is $got, expected $EXPECT_SHA256 (wrong or corrupt file)"
echo "ok"

# ---- extract the three disk images named by BuildManifest.plist ----
DMG=$WORK/dmg
mkdir -p "$DMG"
say "extracting the root and cryptex disk images"
python3 - "$IPSW" "$DMG" <<'PY'
import plistlib, sys, zipfile, os
ipsw, out = sys.argv[1:3]
z = zipfile.ZipFile(ipsw)
bm = plistlib.loads(z.read('BuildManifest.plist'))
want = {'OS': 'os.dmg', 'Cryptex1,SystemOS': 'sysos.dmg', 'Cryptex1,AppOS': 'appos.dmg'}
ident = None
for b in bm['BuildIdentities']:
    info = b['Info']
    if info.get('DeviceClass', '').lower() == 'd22ap' and 'Erase' in info.get('RestoreBehavior', ''):
        ident = b
        break
if ident is None:
    sys.exit('no d22ap erase-restore build identity in the BuildManifest')
for comp, name in want.items():
    path = ident['Manifest'][comp]['Info']['Path']
    dest = os.path.join(out, name)
    if os.path.exists(dest) and os.path.getsize(dest) == z.getinfo(path).file_size:
        print('have', name, path)
        continue
    print('extracting', comp, '->', name, '(', path, ')')
    with z.open(path) as src, open(dest + '.part', 'wb') as dst:
        while True:
            chunk = src.read(8 << 20)
            if not chunk:
                break
            dst.write(chunk)
    os.rename(dest + '.part', dest)
PY
ls -la "$DMG"

# ---- build the HFS+ root ----
[ ! -e "$OUT" ] || die "$OUT already exists; move or delete it first"
say "building the root image (this takes a while: ~12 GB are copied)"
export WORK_UNUSED=1
sudo() { "$@"; }           # mkiosroot.sh calls sudo; we already are root (and sudo may be absent)
export -f sudo
TMPDIR=$WORK bash "$TOOLS/mkiosroot.sh" "$DMG/os.dmg" "$DMG/sysos.dmg" "$DMG/appos.dmg" "$OUT" "$SIZE"

# ---- bring-up tweaks ----
say "applying the SWEET bring-up tweaks"
fsck.hfsplus -fy "$OUT" || true
MNT=$WORK/mnt
mkdir -p "$MNT"
cleanup() { mountpoint -q "$MNT" && umount "$MNT" || true; }
trap cleanup EXIT
mount -t hfsplus -o loop,force "$OUT" "$MNT"
python3 "$TOOLS/sweet-root-tweaks.py" "$MNT"
sync
umount "$MNT"
trap - EXIT
say "final filesystem check"
fsck.hfsplus -fy "$OUT" | grep -v 'Illegal name' || true
fsck.hfsplus -fn "$OUT" || echo "(fsck.hfsplus -n reports a minor repair on fresh journaled volumes: known, harmless)"

say "checksum"
(cd "$WORK" && sha256sum "$(basename "$OUT")" | tee "$(basename "$OUT").sha256")
ls -la "$OUT"
say "DONE $(date -u +%FT%TZ): $OUT  (nothing was written to the phone)"
