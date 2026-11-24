#!/bin/bash
# Install the ssh shell userland (dropbear + dash + toybox, built by .github/workflows/shell.yml) on the phone's iOS volume.
#   bonito/tools/install-shell.sh KERNEL_RUN [BIN_DIR] [KEY_DIR]      defaults: BIN_DIR=/usb/work/shell/bin KEY_DIR=/usb/work/shell
# The phone must be in the bootloader. It does ONE journal-flush boot of the kernel (sweetnoinit=1: no launchd, no iOS userland; the same boot
# set-phone-file.sh does), then writes in the recovery with hfsx_write.py and checks every file byte for byte. It never starts iOS userland and
# only touches: /bin/sh, /private/var/local/bin/{dropbear,dropbearmulti,scp,dash,toybox}, /private/etc/dropbear, /private/var/root/{.ssh,.profile},
# and the launchd.plist (one job: org.iosonpixel.dropbear, via add-shell-job.py; the old plist is saved next to the undo files in logs/).
# Re-runnable: existing files are replaced. Remove the job again with: python3 bonito/tools/add-shell-job.py adb:/dev/block/mmcblk0p72 --remove
cd "$(dirname "$0")/../.." || exit 2
KR=${1:?usage: install-shell.sh KERNEL_RUN [BIN_DIR] [KEY_DIR]}
BIN=${2:-/usb/work/shell/bin}; KEYS=${3:-/usb/work/shell}
for f in dash dropbearmulti toybox; do [ -s "$BIN/$f" ] || { echo "install-shell: missing $BIN/$f"; exit 2; }; done
[ -s "$KEYS/id_ed25519.pub" ] || { echo "install-shell: missing $KEYS/id_ed25519.pub"; exit 2; }
V=/dev/block/mmcblk0p72
T=$(mktemp -d); L=/tmp/install-shell.log; echo "install-shell $(date)" > $L
say() { echo "install-shell [$(date +%H:%M:%S)]: $*" | tee -a $L; }
fail() { say "FAILED: $*"; exit 1; }

# the shell profile: toybox applets as shell functions (no symlinks can be created by hfsx_write)
APPLETS="ls cat cp mv rm mkdir rmdir ln chmod chown chgrp kill grep sed head tail wc find xargs sort uniq cut tr du uname date env id whoami hexdump od dd sleep touch stat which tee printf echo true false test basename dirname readlink realpath hostname netcat sync md5sum sha1sum sha256sum tar gzip zcat mknod truncate file strings xxd seq yes cmp diff"
cat > $T/profile <<PROF
export PATH=/private/var/local/bin:/usr/bin:/bin:/usr/sbin:/sbin
export HOME=/var/root
for a in $APPLETS; do eval "\$a() { /private/var/local/bin/toybox \$a \"\\\$@\"; }"; done
PS1='ios# '
PROF
cp "$KEYS/id_ed25519.pub" $T/authorized_keys

# 1. journal flush boot (as set-phone-file.sh); SKIP_FLUSH=1 when the phone is already in the recovery with a flushed journal
if [ -z "$SKIP_FLUSH" ]; then
OK=0
for try in 1 2 3; do
	bonito/tools/ci-cycle.sh "$KR" fl_shell "sweetnoinit=1 sweetreboot=150" 400 "sweetfb_sync" > /tmp/cycle-fl_shell.log 2>&1
	grep -a -q "VFS_SYNC(root) = 0" logs/fl_shell.txt && { OK=1; break; }
done
[ $OK = 1 ] || fail "journal flush boot did not complete"
rm -f logs/fl_shell.txt logs/fl_shell.png bonito/shim/out/fl_shell.img work/fl_shell.payload
[ "$(fastboot getvar product 2>&1 | grep -a product | head -1 | awk '{print $2}')" = bonito ] || fail "not a bonito"
fastboot set_active a >/dev/null 2>&1; fastboot reboot >/dev/null 2>&1
for i in $(seq 1 40); do adb devices | grep -q recovery && break; sleep 5; done
adb devices | grep -q recovery || fail "no recovery"
adb shell 'setprop ctl.stop sweetreboot; setprop ctl.stop sweetdiag' >/dev/null 2>&1
fi
adb devices | grep -q recovery || fail "the phone is not in the recovery"
adb shell 'setprop ctl.stop sweetreboot; setprop ctl.stop sweetdiag' >/dev/null 2>&1

W="python3 -I bonito/tools/hfsx_write.py adb:$V"
exists() { python3 -I bonito/tools/hfsx.py --phone stat "$1" >/dev/null 2>&1 < /dev/null; }
verify() {   # verify LOCAL REMOTE: sync + retry (a read right after a write can return old data)
	for t in 1 2 3; do sleep 3; adb shell 'sync; echo 3 > /proc/sys/vm/drop_caches' < /dev/null
		python3 -I bonito/tools/hfsx.py --phone cat "$2" $T/rb >/dev/null 2>&1 < /dev/null
		cmp -s "$1" $T/rb && { say "ok   $2"; return 0; }; done
	return 1
}
putfile() {  # putfile LOCAL REMOTE MODE
	if exists "$2"; then $W replace "$1" "$2" < /dev/null >> $L 2>&1 || return 1
	else $W put "$1" "$2" "$3" < /dev/null >> $L 2>&1 || return 1; fi
	verify "$1" "$2"
}
mk() {
	exists "$1" && return 0
	$W mkdir "$1" "$2" < /dev/null >> $L 2>&1
	for t in 1 2 3; do sleep 3; adb shell 'sync; echo 3 > /proc/sys/vm/drop_caches' < /dev/null; exists "$1" && return 0; done
	fail "mkdir $1"
}

mk /private/etc/dropbear 700
mk /private/var/root/.ssh 700
for n in dropbear dropbearmulti scp; do putfile $BIN/dropbearmulti /private/var/local/bin/$n 755 || fail "$n"; done
putfile $BIN/dash /private/var/local/bin/dash 755 || fail dash
putfile $BIN/toybox /private/var/local/bin/toybox 755 || fail toybox
putfile $BIN/dash /bin/sh 755 || fail "/bin/sh"
putfile $T/profile /private/var/root/.profile 644 || fail ".profile"
putfile $T/authorized_keys /private/var/root/.ssh/authorized_keys 600 || fail authorized_keys

# 2. the launchd job, then read the plist back and parse it (a damaged tail once made iOS panic "No service cache")
mkdir -p logs/shell-undo
python3 -I bonito/tools/hfsx.py --phone cat /System/Library/xpc/launchd.plist logs/shell-undo/launchd.plist.before >/dev/null 2>&1 < /dev/null
python3 -I bonito/tools/add-shell-job.py adb:$V < /dev/null >> $L 2>&1 || fail "add-shell-job"
adb shell 'sync; echo 3 > /proc/sys/vm/drop_caches' < /dev/null
python3 -I bonito/tools/hfsx.py --phone cat /System/Library/xpc/launchd.plist $T/launchd.after >/dev/null 2>&1 < /dev/null
python3 -I - $T/launchd.after >> $L 2>&1 <<'PY' || fail "launchd.plist does not parse after the write: restore logs/shell-undo/launchd.plist.before before booting iOS"
import plistlib, sys
p = plistlib.load(open(sys.argv[1], 'rb'))
assert '/System/Library/LaunchDaemons/org.iosonpixel.dropbear.plist' in p['LaunchDaemons'], 'job missing'
print('launchd.plist parses,', len(p['LaunchDaemons']), 'jobs, dropbear job present')
PY
say "launchd.plist ok"
adb shell sync < /dev/null; adb reboot bootloader >/dev/null 2>&1
say "DONE: shell userland installed; phone is in the bootloader (iOS has not been started)"
