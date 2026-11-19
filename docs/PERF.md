# bonito performance findings (2026-11-18/19)

Measured with the unattended loop (logs/bench1.txt, deny1.txt, gpusvc2.txt):

- **CPU clocks are fine.** `SWEET CPUFREQ` (patch 0130) puts the 6 silver cores at 1708 MHz and the 2 gold cores at 1996 MHz;
  `sweetcpubench=1` (patch 0151) measures 1995 M dependent adds/s on a gold core (1 per cycle) and 853 M/s on a silver core
  (the loop runs at 2 cycles per iteration there): nothing to gain.
- **Memory is fine.** 16 MiB buffers, single thread: memcpy 2.0 GB/s, memset 6.8 GB/s, read 2.3 GB/s (normal for this SoC).
- **Storage is the bottleneck.** The eMMC driver (patch 0126 plus later ones in bsd/dev/sweet_ufs.c) runs high-speed 8-bit at 50 MHz,
  polled, one request at a time under one mutex with a bounce buffer. `SWEET EMMC stats` (patch 0153, every 5 s): during the iOS boot
  about 115-126 MB are read per 5 s (about 24 MB/s average, 40-130 KiB per request, 20-30 requests queued behind the lock) for about
  50 s, about 1.5 GB in total. The silent stretches in the boot log (launchd's `xpcroleaccountd` boot task takes 36-100 s) are this.
- **Crash loops without a GPU** (touch5.txt): AegirPoster 34 per boot, the speech synthesizers (SiriTTSSynthesizerAU, MacinTalkAUSP,
  KonaSynthesizer) 12 each. `sweetdeny` (bonito/tools/mkios.sh) cuts them to 2 and 1 (deny1.txt).

Plan: eMMC HS200 (200 MHz, 8-bit SDR, about 4-6x the throughput). The controller is "qcom,sdhci-msm-v5" at 0x7c4000 with HS400, HS200
and DDR listed in the DT. Qualcomm's own tuning is in sdhci-msm.c (work/ref/sdhci-msm.c from the LineageOS msm-4.9 tree): vendor
registers at the hc base (SDCC v5 offsets): CORE_DLL_CONFIG 0x200, CORE_DLL_STATUS 0x208, CORE_VENDOR_SPEC 0x20c, CORE_DLL_CONFIG_2
0x254, CORE_DDR_CONFIG 0x258, CORE_DLL_USR_CTL 0x388; DLL init (reset, power down, freq select by clock, release, enable, wait for
lock), then for each of 16 grey-coded phases send CMD21 (128-byte block for an 8-bit bus) and compare with the JEDEC pattern, pick the
middle of the longest run of good phases. Safety plan: read a 16 MiB region at 50 MHz first, hash it, switch to HS200, tune, read it again and
compare several times; on any mismatch redo the whole controller init without HS200. Writes only ever go to the partition, and the first
HS200 session verifies a scratch write/read in the unused part of userdata.
