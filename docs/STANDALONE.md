# Booting iOS from the phone itself (slot B)

Normally every test boots through `fastboot boot` from the PC. Slot B can boot the iOS shim image by itself instead.

## How it is set up

- Slot A keeps the Linux recovery (`boot_a`): all the tools (`adb`, `hfsx_write.py`, `ci-cycle.sh`, ...) use it. It is never changed.
- Slot B (`boot_b`) holds the iOS boot image built for use by hand (`AUTORESET= bonito/tools/mkios.sh KERNEL NAME "sweeths200=6 sweetpmem=1"`: no timed reset,
  no panic reset, no watchdog). The finished image is `/usb/work/standalone/ios-standalone.img` (kernel run 45029003673).
- `dtbo_b` and `vbmeta_b` are copies of slot A's (`/usb/work/standalone/dtbo_a.img`, `vbmeta_a.img`); the originals of everything that was in slot B are in
  `/usb/work/standalone/backup-b/`.
- Slot B's firmware (`xbl`, `xbl_config`, `abl`, `aop`, `tz`, `hyp`, `keymaster`, `devcfg`, `qupfw`, `msadp`, `apdp`, `cmnlib`, `cmnlib64`, `bluetooth`) is
  bit-for-bit identical to slot A's (checked with sha256 from the recovery), so slot B runs exactly the bootloader that already works. Only `storsec` (TrustZone
  secure-storage data, not code) differs; it is left alone.

## Use

    bonito/tools/standalone.sh arm --boot     # slot B active, reboot: iOS comes up by itself
    bonito/tools/standalone.sh disarm         # slot A active again (what the test scripts do on their own anyway)
    bonito/tools/standalone.sh flash [IMAGE]  # new iOS image into boot_b

## The limit: three boots per `arm`

Nothing in iOS tells the bootloader that a boot worked, so slot B's A/B retry counter counts down by one per boot (3 after `arm`). After the third boot the
bootloader marks slot B unbootable and stays in fastboot (verified: the phone sat in fastboot, slot B `unbootable: yes`). `arm` again (hold Power + Volume Down
for fastboot) renews the three boots. The test scripts all call `fastboot set_active a` themselves, so they keep working while slot B is armed or not.

Making the counter permanent means setting the "boot successful" flag, which Qualcomm keeps in the GPT partition attributes. That is deliberately not done:
a wrong write to the partition table is the one thing that can take fastboot away. (Android's own boot-control HAL does it on every Android boot; it could be run once
from the recovery if this limit ever matters more than that risk.)

## Rules this was done under

Never write firmware partitions, never edit the GPT, never touch slot A. Only `fastboot flash` of `boot_b`/`dtbo_b`/`vbmeta_b` and `fastboot set_active`.

## Verified

A self-rebooting variant (`sweetreboot=60`) booted from slot B three times with no PC involved (about 200 s), the iOS log (`logs/standalone-b.txt`) shows the shim,
launchd's boot tasks and the 60 s timed reset, and the phone ended in fastboot with slot B out of retries.
