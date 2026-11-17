# Recovery for bonito (Pixel 3a XL)

Verified 2026-11-16. Official image: `bonito-sp2a.220505.008-factory-db19d2aa.zip`,
sha256 `db19d2aaff36f29b7f58f11acad856f9382dafa1ae9160a692782e27cd7b0002` (Google, kept in `images/`, not in git).
Bootloader `b4s4-0.4-8048689`, radio `g670-00145-220106-B-8048689`.

## What works
**Android Flash Tool** (flash.android.com, Chrome/WebUSB on another computer): Pixel 3a XL, build SP2A.220505.008,
Wipe + Force flash all partitions, **Lock bootloader OFF**. Restored a half-flashed phone on 2026-11-16.

## What did not
`images/.../flash-all.sh` from this Raspberry Pi: bootloader and radio flashed fine, then `fastboot -w update` failed in
fastbootd with `Resizing 'product_a' FAILED (remote: 'Invalid partition size')`. Cause not found.

## Getting to fastboot
Power off (hold Power ~30 s), then hold Power + Volume Down. If the bootloader hangs after a `fastboot boot`
(commands time out, USB still enumerates), that is the only way out.

## Lessons
- The original bootloader `b4s4-0.1-5653622` cannot `fastboot boot` any header-v2 image; 0.4 can.
- The unlocked bootloader shows an orange warning for ~5 s before the booted image starts.
- The probe screen output works after any reset; earlier failures were caused by watchdog writes in the shim.

## Log-reading recovery in `boot_a` (planned 2026-11-17)
Why: no camera-free log channel from iOS (the XNU USB gadget never enumerates). `fastboot boot` of a Linux recovery does
not work on this bootloader (it takes the normal-boot path and the ramdisk we send is ignored; verified by the USB
config string staying `adb` instead of our `adb+ums`). So the Lineage recovery is put in `boot_a`: Android fails to mount
`/data` (it is the iOS root), falls back to recovery, and our recovery exposes the eMMC read-only over USB mass storage.
- Image: LineageOS 22.2 `boot.img` for bonito (20261114, sha256 `f1c5227e14025943d051c2559588d40cbdc4c3bfc03f1a814e5b4b48ab7acd91`,
  from download.lineageos.org), repacked by `bonito/tools/mk-linux-recovery.py` (adds a read-only mass-storage LUN over
  `/dev/block/mmcblk0`, a 240 s timer that runs `reboot bootloader`, and AVB hash footer). Only `boot_a` is written.
- **Restore stock boot:** `fastboot flash boot_a stock/boot.img` (local copy, sha256
  `71559139c0e4c117ec397afd7922d6ab42ea2545525cf89c7f2da01e6cf995eb`; identical to `boot.img` inside the factory image zip).
  Or Android Flash Tool as above. The bootloader and radio are not touched.
