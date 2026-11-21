#!/bin/bash
# Replace the contents of an EXISTING file on the phone's iOS volume (no mounting; needs a clean HFS journal, so a sweetnoinit=1 boot comes first).
#   bonito/tools/set-phone-file.sh KERNEL_RUN LOCALFILE /abs/path/on/the/phone
# The phone must be in the bootloader; it is left in the bootloader. Exit 0 only if the file was read back identical.
cd "$(dirname "$0")/../.." || exit 2
KR=$1; LOCAL=$2; REMOTE=$3
TAG=spf$$
bonito/tools/ci-cycle.sh "$KR" "fl_$TAG" "sweetnoinit=1 sweetreboot=150" 400 "sweetfb_sync" > "/tmp/cycle-fl_$TAG.log" 2>&1
grep -a -q "VFS_SYNC(root) = 0" "logs/fl_$TAG.txt" || { echo "journal flush failed"; exit 2; }
rm -f "logs/fl_$TAG.txt" "logs/fl_$TAG.png" "bonito/shim/out/fl_$TAG.img" "work/fl_$TAG.payload"
[ "$(fastboot getvar product 2>&1 | grep -a product | head -1 | awk '{print $2}')" = bonito ] || { echo "not bonito"; exit 3; }
fastboot set_active a >/dev/null 2>&1; fastboot reboot >/dev/null 2>&1
for i in $(seq 1 40); do adb devices | grep -q recovery && break; sleep 5; done
adb devices | grep -q recovery || { echo "no recovery"; exit 4; }
adb shell 'echo 3 > /proc/sys/vm/drop_caches'
python3 -I bonito/tools/hfsx_write.py adb:/dev/block/mmcblk0p72 replace "$LOCAL" "$REMOTE" | tail -1
adb shell 'echo 3 > /proc/sys/vm/drop_caches'
python3 -I bonito/tools/hfsx.py --phone cat "$REMOTE" "/tmp/readback-$TAG" > /dev/null 2>&1
cmp "$LOCAL" "/tmp/readback-$TAG" && R=0 || R=5
rm -f "/tmp/readback-$TAG"
adb reboot bootloader
for i in $(seq 1 20); do fastboot devices | grep -q fastboot && break; sleep 3; done
exit $R
