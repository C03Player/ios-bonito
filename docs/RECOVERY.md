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
- The probe shim's screen output only appeared after a cold fastboot entry.
