#!/bin/bash
# usage: tools_wait_boot.sh IMAGE  -- wait for a responsive bonito in fastboot, boot IMAGE, report USB behaviour
IMG=$1
for i in $(seq 1 300); do
  P=$(timeout 5 fastboot getvar product 2>&1 | head -1)
  case "$P" in *bonito*) break;; esac
  sleep 2
done
case "$P" in *bonito*) ;; *) echo "TIMEOUT: no responsive bonito"; exit 2;; esac
echo "device ok: $P"
timeout 40 fastboot boot "$IMG" 2>&1
echo "boot returned rc=$?"
for t in $(seq 1 40); do
  sleep 3
  if timeout 5 fastboot getvar product >/dev/null 2>&1; then echo "fastboot responsive again after ~$((t*3))s (device reset)"; exit 0; fi
done
echo "no response for 120s (shim hung or running)"
