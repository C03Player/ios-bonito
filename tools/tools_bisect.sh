#!/bin/bash
# For each stage image: boot it, time how long until fastboot answers again. Idle shim => ~ wait+reboot; early death => ~+20s.
cd /workspace/IOSOnPixel/bonito/shim
for n in "$@"; do
  for i in $(seq 1 60); do timeout 5 fastboot getvar product 2>&1 | grep -q bonito && break; sleep 2; done
  P=$(timeout 10 fastboot getvar product 2>&1 | head -1); case "$P" in *bonito*) ;; *) echo "stage $n: no bonito, stop"; exit 1;; esac
  T0=$(date +%s); timeout 60 fastboot boot out/s$n.img >/dev/null 2>&1
  gone=0; back=-1
  for i in $(seq 1 200); do
    if lsusb | grep -q 18d1; then s=up; else s=down; fi
    [ $s = down ] && gone=1
    if [ $s = up ] && [ $gone = 1 ]; then back=$(( $(date +%s)-T0 )); break; fi
    sleep 1
  done
  echo "stage $n: USB back at +${back}s"
  sleep 5
done
