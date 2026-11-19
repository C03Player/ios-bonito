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


## eMMC HS200 (patch 0154, boot-arg `sweeths200=6`, default off)

Verified on the phone (2026-11-19): SDCC version 0x1000006c (major 1, minor 0x6c = low byte, so the updated DLL reset is used),
DLL locks at 200 MHz, CMD21 tuning finds good phases 0xdfff / 0xffff and picks phase 8, 16 MiB at LBA 0x400000 reads back identical
three times. Reads reach 35-41 MB/s while busy (50 MHz high speed: ~22 MB/s). launchd boot tasks that took 20-50 s now take ~3 s
and SpringBoard starts about 45 s after the kernel (it used to be 100+ s), when the boot does not hit the process stall below.

How it was found (all in the patch): `sweeths200=N` runs only the first N steps (1 baseline read, 2 CMD6, 3 clock, 4 DLL, 5 tuning,
6 everything); 41-45 stop inside the DLL sequence; 8 and 9 are probes of the failure path. The stages restore 50 MHz in place and
succeed, because the failure path itself is broken: starting over with a full `ufs_take_over_host()` after a failed HS200 attempt
hangs the phone (probe 8; only the watchdog gets it back; cause not found). A data command that fails while HS200 runs now falls back to
50 MHz high speed in place (`emmc_hs200_recover`) and retries once.

Lessons: the SDCC minor version is `MCI_VERSION & 0xff`; a hang before the log thread starts leaves no log at all (the recovery then
shows the previous boot's ring: compare with `cmp` against an older log); the ring log mixes boots (find the last `SWEET EMMC: CID`).

## Process stall (not storage)

Independent of HS200 a boot sometimes stops for 15-175 s at one launchd boot task with the eMMC idle (`dirs_cleaner`,
`FinishRestoreFromBackup`, `usermanagerd` seen). `sweetsample=*` (patch 0152) shows the stalled process has ONE thread, always at
the same user pc 0x1e4ff7e0c (a syscall stub) with the same caller: it is blocked before main(). Patch 0155 adds the syscall number
and kernel wait state to the sample to find what it waits for.
