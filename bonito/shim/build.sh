#!/bin/bash
# Build the bonito probe shim and wrap it in a boot image v2 whose layout
# matches the stock bonito boot.img (base 0, kernel 0x8000, ramdisk 0x1000000,
# tags 0x100, dtb 0x1f00000, page 4096).
#
#   bonito/shim/build.sh [OUT]
#
# Host tools: clang, ld.lld, llvm-objcopy, llvm-nm, lz4, python3, cpio, and
# downloads/mkbootimg + downloads/avb (AOSP). The DTB is the stock boot.img's
# (stock/unpacked/dtb), which holds the SDM670 base DTB.
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
CC="${CC:-clang} --target=aarch64-none-elf"
CFLAGS="-O2 -ffreestanding -nostdlib -fno-builtin -fno-pic -fno-pie -mgeneral-regs-only -mstrict-align
	-fno-stack-protector -fno-asynchronous-unwind-tables -Wall -Wextra ${SHIM_CFLAGS:-}"
SRCS="start.S shim.c console.c log.c util.c fdt.c spmi.c sde.c emmc.c loader.c"
mkdir -p out
objs=
for f in $SRCS; do
	$CC $CFLAGS -c "$f" -o "out/${f%.*}.o"
	objs="$objs out/${f%.*}.o"
done
abs=$(llvm-readelf -r $(echo "$objs" | sed 's| out/start.o||') | grep -c R_AARCH64_ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
ld.lld -static -nostdlib -T shim.ld --build-id=none -N $objs -o out/shim.elf
llvm-objcopy -O binary out/shim.elf out/Image
size=$(llvm-nm out/shim.elf | awk '$3=="_end"{print $1}')
truncate -s $((16#$size)) out/Image
lz4 -q -9 -B7 -f out/Image out/Image.lz4
if [ -n "${PAYLOAD:-}" ]; then
	cp "$PAYLOAD" out/ramdisk.bin	# tools/mkpayload.py output: the shim boots it
else
	(cd out && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet 2>/dev/null | gzip -9 > ../ramdisk.bin)
fi
OUT=${1:-out/shim-boot.img}
if [ -f "$ROOT/downloads/mkbootimg/mkbootimg.py" ] && [ -f "$ROOT/stock/unpacked/dtb" ]; then
	PYTHONPATH="$ROOT/downloads/mkbootimg" python3 "$ROOT/downloads/mkbootimg/mkbootimg.py" \
		--header_version 2 --kernel out/Image.lz4 --ramdisk out/ramdisk.bin --dtb "$ROOT/stock/unpacked/dtb" \
		--base 0 --kernel_offset 0x8000 --ramdisk_offset 0x1000000 --tags_offset 0x100 \
		--dtb_offset 0x1f00000 --pagesize 4096 --os_version 12.0.0 --os_patch_level 2022-05 \
		--cmdline "${BOOT_CMDLINE:-console=ttyMSM0,115200n8 androidboot.console=ttyMSM0 printk.devkmsg=on msm_rtb.filter=0x237 ehci-hcd.park=3 service_locator.enable=1 firmware_class.path=/vendor/firmware cgroup.memory=nokmem lpm_levels.sleep_disabled=1 loop.max_part=7 androidboot.boot_devices=soc/7c4000.sdhci androidboot.super_partition=system buildvariant=user}" -o "$OUT"
	# Google's ABL (as on flame) wants an AVB hash footer on 'fastboot boot' images.
	if [ -f "$ROOT/downloads/avb/avbtool.py" ]; then
		python3 "$ROOT/downloads/avb/avbtool.py" add_hash_footer --image "$OUT" \
			--partition_size 67108864 --partition_name boot
	else
		echo "note: downloads/avb/avbtool.py missing; image has no AVB footer" >&2
	fi
	ls -l "$OUT"
fi
ls -l out/Image
