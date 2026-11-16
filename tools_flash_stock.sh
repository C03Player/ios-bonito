#!/bin/bash
# Official factory flash for bonito (same steps as the image's flash-all.sh), with a product check first.
set -u
D=/workspace/IOSOnPixel/images/bonito-sp2a.220505.008/bonito-sp2a.220505.008
cd "$D" || exit 1
( cd /workspace/IOSOnPixel/images && sha256sum -c SHA256SUMS ) || { echo "checksum FAIL"; exit 1; }
for i in $(seq 1 300); do
  P=$(timeout 5 fastboot getvar product 2>&1 | head -1)
  case "$P" in *bonito*) break;; esac; sleep 2
done
case "$P" in *bonito*) ;; *) echo "TIMEOUT: no responsive bonito, nothing written"; exit 2;; esac
echo "device ok: $P; unlocked: $(timeout 5 fastboot getvar unlocked 2>&1 | head -1)"
set -e
fastboot flash bootloader bootloader-bonito-b4s4-0.4-8048689.img
fastboot reboot-bootloader; sleep 5
fastboot getvar version-bootloader
fastboot flash radio radio-bonito-g670-00145-220106-b-8048689.img
fastboot reboot-bootloader; sleep 5
fastboot getvar version-baseband
fastboot -w update image-bonito-sp2a.220505.008.zip
echo FLASH-DONE
