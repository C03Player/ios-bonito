# bonito (Pixel 3a XL, SDM670) hardware notes

Status key: **[LIVE]** read on the phone by the probe shim (photo `logs/BONITOSHIM.jpg`, 2026-11-16, bootloader
`b4s4-0.4-8048689`), **[DTB]** from the stock SDM670 DTB / B4 DTBO in the SP2A.220505.008 factory image,
**[SRC]** from Lineage msm-4.9 source, **[ASSUMED]** not verified.

## Device / boot
- fastboot: product `bonito`, variant `SDM eMMC`, hw MP1.0, 2 slots, bootloader unlocked. [LIVE]
- **Bootloader `b4s4-0.1-5653622` hangs after "boot.img missing cmdline or OS version" on any header-v2 image,
  even Google's stock boot.img. `b4s4-0.4-8048689` (factory image) accepts `fastboot boot` of our shim.** [LIVE]
- Unlocked ABL shows an orange warning for ~5 s before the image runs. [LIVE, user]
- Image rules (as flame): header v2, base 0, kernel_off 0x8000, ramdisk_off 0x1000000, tags 0x100, dtb_off 0x1f00000,
  page 4096, LZ4 kernel, Image header text_offset 0x80000, AVB hash footer, partition size 64 MiB. [LIVE: accepted]
- ABL hands over at EL1, MMU off, x0 = DTB at 0x84500000, ramdisk at 0x84700000. [LIVE]
- **Dev loop works unattended.** `fastboot boot` -> ~5 s orange warning -> shim runs -> after its timer it writes PM660 PON
  0x88f (reason 2 << 1) and issues PSCI SYSTEM_RESET -> phone returns to fastboot with no buttons. [LIVE]
- The shim runs >= 30 s with no watchdog handling: the APSS WDT is not a problem for a probe. [LIVE, bisect 2026-11-16]
- **Do not touch the APSS WDT at 0x17980000 (DTB address).** Builds that wrote it (SHIM_WDT_QUIET) died early, never reached
  the display and made the phone return to fastboot ~13 s after start. Cause of the death not analysed (access fault/hang?). [LIVE]
- The display works after both cold and shim-caused resets (earlier "cold start only" theory was wrong). [LIVE]
- PON 0x88f reads 0x10 at shim start in every run, whatever was written before: the bootloader rewrites it. It is NOT evidence
  of a watchdog reset. [LIVE]

## SoC
- CPU: 8 cores, MIDR 0x517f803c (Kryo 360 Gold) MPIDR 0x81000000 (affinity in Aff1: cpu n = 0x81000n00 pattern, GICR typer
  affinity 0x000,0x100..0x700). 2 big + 6 little per DTB efficiency fields. [LIVE/DTB]
- Counter 19.2 MHz. PSCI 1.1, SMC conduit; CPU_ON and SYSTEM_RESET supported, RESET2/SYSTEM_SUSPEND not, SMCCC_VERSION not. [LIVE]
- GIC-500 (IIDR 0x200243b), GICD 0x17a00000, GICR 0x17a60000 stride 0x20000 x8, all awake. [LIVE]
- RAM (from ABL-filled DTB): 0x80000000 +0x80000000, 0x100000000 +0x7e2c0000 (about 4 GiB). [LIVE]
- Splash/reserved fb region 0x9c000000 +0x2400000. [DTB, LIVE]
- APSS WDT 0x17980000, bark 11 s, pet 9.36 s. [DTB]
- PMIC: PM660 (SID 0, subtype 0x1b), SPMI arbiter v5 @0xc440000 (same map as flame), 512 APIDs. PON soft_rb_spare 0x88f. [LIVE]
- IMEM base 0x146bf000; restart_reason@0x65c read 0x7c3edfc8 (not the expected cookie; do not rely on it). [LIVE]
- ramoops 0xa1810000 (2 MiB), alt 0xa1a10000, meta 0xa1c10000 (B4 DTBO). [DTB]

## Storage: **eMMC, not UFS**
- SDHCI controller 0x7c4000 (+CQ 0x7c5000), qcom,sdhci-msm-v5, 8-bit, HS400/HS200/DDR 1.8 V. Live: SDHCI spec v3 (0x6c02),
  caps 742dc8b2:808f, CQHCI 5.1 (0x510). [LIVE/DTB] -> the shim/XNU need a new SDHCI driver (flame's ufs.c is useless).

## Display
- SDE hw 4.0.1 (reg 0x40010000), MDP 0xae00000. Pipe vig0 -> LM0 -> PP0 -> DSC0 -> INTF1 (DSI). [LIVE]
- vig0 scans out 1080x2160 from 0x9c000000, stride 4320 (0x10e0), tear check on, autorefresh already armed by ABL. [LIVE]
- Panels (B4 = bonito, active in DTBO): SOFEF00 (Samsung) or NT37700F (Tianma), both 1080x2160 DSI command mode;
  the 1080x2220 s6e3fa7 is sargo's. Which one this unit has is not yet determined. [DTB]
- SDE register offsets differ from flame (SDE 5): tear/autorefresh live in PINGPONG (PP+0x30), not INTF. [SRC, LIVE]

## Touch
- Synaptics DSX over I2C addr 0x20, irq GPIO125, reset GPIO99, switch GPIO135 (flame: ST FTS over SPI). [DTB] Not probed yet.

## Not yet examined
GPU (Adreno 615), audio (cs35l36 + WCD9xxx), Wi-Fi/BT (WCN3990), USB (DWC3 + QUSB2), cpufreq, battery, regulators/clocks, SMMU.
