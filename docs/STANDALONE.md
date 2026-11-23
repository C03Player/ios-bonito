# Booting iOS from the phone itself (slot B)

Normally every test boots through `fastboot boot` from the PC. Slot B can boot the iOS shim image by itself instead.

## How it is set up

- Slot A keeps the Linux recovery (`boot_a`): all the tools (`adb`, `hfsx_write.py`, `ci-cycle.sh`, ...) use it. It is never changed.
- Slot B (`boot_b`) holds the iOS boot image for standalone use: `DENY=fairplayd bonito/tools/mkios.sh KERNEL ios-standalone4 "sweetgpu=6 sweetmtl=2 sweetgpudelay=5 sweetddr=0"`, i.e. the
  GPU on (see below), the default eMMC mode, the panic reset (`sweetpanicreset=1`) and the watchdog (`sweetwdt=25`) on, and no timed reset. The finished image is
  `/usb/work/standalone/ios-standalone4.img` (kernel run 45029003673). `DENY=fairplayd` is needed to fit the 591-character command line.
- **The GPU needs boot-args**: the kernel's `sweetgpu` defaults to 0 (off), so without `sweetgpu=6 sweetmtl=2 sweetgpudelay=5` backboardd logs `IOAcceleratorES -> 0 found`
  forever and nothing renders through the GPU (the first standalone images had none: "the GPU is not working"). With them: `SWEET GPU615` starts, `SWEET MTL: IOAcceleratorES
  published (MetalPluginName SweetMetal2)`, backboardd/SpringBoard match it. `sweetpmem=1` is only for Wi-Fi and is not in the standalone image. **`sweetddr=0` is also required**: with the default DDR/bus vote (patch 0204) the screen shows rectangles of other parts of the frame while things move (see docs/GPU.md, 2026-11-23). **Do not add `sweeths200=6`** (the "fast eMMC mode" that `boot-manual.sh` defaults to): on this phone HS200 throws read errors, a failed read of a
  code page kills launchd (SIGBUS) and the kernel panics ("initproc exited -- exit reason namespace 2 subcode 0xa", about 150 `SWEET EMMC ... failed` lines before it);
  the first standalone image had it and panicked within minutes.
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

The corrected image booted from slot B with no PC involved and ran for at least 137 s with no eMMC error and no panic (log read from the raw ring, see below). Slot B
then showed "no valid slot to boot" because its 3 boot attempts were used up (every power cycle by hand counts as one). The panics of the HS200 image are kept in
`logs/recovered-user-panic-1.txt` / `-2.txt` and `logs/standalone-b.txt`.

## Reading the iOS log without destroying it

The persistent log is the last 4 MiB (+1 block) of the userdata partition, rewritten from offset 0 by every iOS boot: boot nothing else before pulling it.
`bonito/tools/sweet-log-pull.py` reads the newest boot; the whole ring (older boots' tails survive beyond the newest boot's length) can be copied with
`ums.UMS` / `ums.gpt` from the recovery (see the script) and searched for `panic(cpu`.
