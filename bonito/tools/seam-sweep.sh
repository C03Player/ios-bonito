#!/bin/bash
# Run N unattended gesture boots of one kernel with two snapshots each, then write the series report (logs/seam-sweep.txt).
#   bonito/tools/seam-sweep.sh KERNEL_RUN NAME N "extra boot-args" [FIRST]
# SHOTSECS (default 7) and SWIPE (default 75, seconds of gestures) can be set in the environment. The phone must be in the bootloader. Boots are NAME<FIRST>..; each lasts about 5 minutes. sweetshot=7 keeps the last two snapshots 7 s apart.
cd "$(dirname "$0")/../.." || exit 2
KR=$1; NAME=$2; N=$3; EXTRA=$4; FIRST=${5:-1}
for i in $(seq $FIRST $((FIRST + N - 1))); do
	rm -f "logs/$NAME$i.png" "logs/$NAME$i-prev.png"
	SHOT=1 CPUS=8 SAMPLE=- DENY=fairplayd WAIT_PHONE=180 bonito/tools/ci-cycle.sh "$KR" "$NAME$i" "sweetgpu=6 sweetmtl=2 sweetgpudelay=5 sweetshot=${SHOTSECS:-7} sweethidtest=30 sweetswipe=${SWIPE:-75} sweetreboot=$((195 + (i % 5) * 4)) $EXTRA" 500 "sweetfb_sync" > "/tmp/cycle-$NAME$i.log" 2>&1
	# keep the disk usable: the flashed image is regenerated every boot
	rm -f "bonito/shim/out/$NAME$i.img" "work/$NAME$i.payload"
done
python3 -I bonito/tools/sweep-report.py "$NAME" "$N" "$FIRST"
