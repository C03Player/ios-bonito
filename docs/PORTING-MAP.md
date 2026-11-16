# bonito port map (from ios-flame @ 745c434 / ios-sweet @ 45263be)

**Confirmed** = observed on the phone or in the stock DTB/DTBO (see BONITO-HARDWARE.md). **Assumed** = not verified.
Nothing below has booted XNU on bonito. The XNU build needs macOS + Xcode (blacktop/darwin-xnu-build); this Linux/arm64 host can
only build and test the shim.

## Boot flow (confirmed up to the shim)
ABL b4s4-0.4 (unlocked) -> `fastboot boot shim-boot.img` -> ~5 s orange warning -> shim at EL1, MMU off, x0 = DTB 0x84500000, ramdisk 0x84700000.
Same image rules as flame (header v2, LZ4 kernel, text_offset 0x80000, AVB footer). Next step (not done): shim loads an XNU payload from the
ramdisk and jumps with an Apple-style device tree (flame: tools/mkadt.py + tools/mkpayload.py). Kernel load address of the shim itself: not recorded yet.

## Area by area
| Area | flame/sweet | bonito | Status |
|---|---|---|---|
| XNU base, build fixes (sweet 0001-0081, flame 0082) | generic | reuse as-is; rename machine config; Xcode 16 needs 0082 | Assumed |
| Early console / framebuffer (flame 0083/0084) | fb 0x9c400000, 1080x2280 | fb 0x9c000000, 1080x2160, stride 4320, a8r8g8b8 order ok | **Confirmed** values |
| CPU, timer, GICv3, PSCI | 4xA55+4xA76, GIC-600 | 6xA55-class + 2xKryo 360 Gold (A75), GIC-500, same GICD/GICR addresses, PSCI 1.1 (no RESET2, no SMCCC) | **Confirmed** |
| cpufreq (flame 0086) | SM8150 OSM | SDM670 OSM/CPUCC registers not examined | Unknown |
| Storage (UFS in flame) | UFS 3.0 driver | **new eMMC driver**: SDCC5 0x7c4000, ADMA2 only (PIO broken), GCC SDCC1 clock, power-IRQ handshake. Shim version works (read-only, 1-bit/400 kHz). XNU IOKit driver still to write; needs HS200/HS400 8-bit for usable speed | Shim **confirmed** |
| iOS root location | userdata sda15 HFS+ image | `userdata` LBA 17360896, 51163 MiB (fastboot-flashing of big images fails on flame; use a RAM-loaded writer/TWRP path) | **Confirmed** layout |
| PMIC / SPMI | PM8150 | PM660 (+PM660L) on SPMI arbiter v5, same arbiter addresses; PON at SID0 0x800 | **Confirmed** |
| Reboot reason | PM8150 SOFT_RB_SPARE | PM660 PON 0x88f reason<<1 (bootloader=2) works; ABL rewrites it at boot | **Confirmed** |
| Watchdog | APSS WDT 0x17c10000, kept alive by patch | APSS WDT 0x17980000; **writing it killed the shim**; a shim runs 30 s+ without petting. Needs analysis before XNU runs long | Unresolved |
| Buttons (flame 0083) | PM8150 PON + GPIO6 | PM660 PON: KPDPWR (power), RESIN (vol-); vol+ = PM660L GPIO7 (gpio-keys, active low) | DTB only |
| RTC (flame 0099) | PM8150 RTC | PM660 `qcom,qpnp-rtc` (SID0) | DTB only |
| Display (flame sde.c, SDE5) | NT37280, INTF1 autorefresh | SDE 4.0.1; vig0 -> LM0 -> PP0 -> DSC0 -> INTF1; autorefresh in PINGPONG; panel SOFEF00 or NT37700F (1080x2160, cmd mode; which one: unknown). ABL already runs it; shim draws a text console | **Confirmed** scanout |
| Touch (flame ST FTS, SPI) | GENI SPI | Synaptics DSX over I2C: QUPv3 se9 (i2c@a84000), addr 0x20, irq GPIO125, reset GPIO99, switch GPIO135 | DTB only |
| USB (flame 0088-0091) | DWC3 0xa600000 + femto/QMP PHY | DWC3 0xa600000, QUSB2 PHY 0x88e2000 (init seq in DTB), QMP SS PHY 0x88e8000; SMMU context differs | DTB only |
| GPU (flame A640 + Turnip) | Adreno 640 | Adreno 615 (sdm670-gpu.dtsi); GMU/zap firmware must come from the user's own factory image `vendor.img` | Unexamined |
| Wi-Fi (wland, WCN3998) | WCN3998 userland driver | WCN3990 (bdwlan for bonito from vendor image) | Unexamined |
| RPMh/BCM votes (flame 0102/0120/0122) | SM8150 IDs | SDM670 RPMh/BCM IDs differ | Unexamined |
| Audio, camera, sensors, BT, cellular | not working on flame | not attempted | - |
| iOS userland | iPhone X 16.7.16 IPSW root | user-supplied; keep out of Git | Not started |

## Blockers, in order
1. Shim -> XNU handoff: payload format, boot-args, Apple-style ADT for SDM670 (memory map 0x80000000+2 GiB, 0x100000000+0x7e2c0000).
2. A Mac with Xcode to build XNU with a bonito machine config (framebuffer consts, eMMC driver, PM660, GIC, I2C touch).
3. Understand/handle the APSS watchdog (and the PMIC/TZ side) so XNU can run past ~30 s.
4. eMMC performance (HS200/HS400, 8-bit) and an XNU block driver.
5. The iOS root filesystem built by the user from an IPSW, and a safe way to write it to `userdata` (wipe accepted).
6. GPU/Metal on Adreno 615; Wi-Fi; audio etc. after the UI boots.
