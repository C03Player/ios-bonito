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

### The stall is a kernel CPU spin (2026-11-19, patch 0155 sample)

`mount_hfs` (a child of launchd's mount-phase-1 task) at the 40 s sample: kernel state TH_RUN, no wait event, on CPU 0, CPU time user 3.8 s /
**sys 34.5 s**; at the next sample 120 s later user 15.7 s / sys 142.6 s, same user pc and sp both times. So the stalled process is not
waiting for anything: it burns CPU almost entirely in the kernel while its saved user state does not change. A livelock such as a
page fault (or syscall restart) that repeats without making progress fits, and a random duration (15-175 s) fits something that
eventually resolves (a lock, a page state, memory pressure). Patch 0155 now also prints the kernel call chain (cross call to the CPU,
frame pointer walk, unslid addresses; look them up with `nm` on the kernel from the CI artifact).

### The stall is a user page fault that repeats (kernel call chain, 2026-11-19)

Two samples of the spinning thread (`launchd_cache_loader`, 95 s and 215 s) with the unslid kernel addresses looked up in the CI kernel
(`llvm-nm -n --defined-only kernel.development.sweet`): the interrupted code is inside
`fleh_synchronous -> sleh_synchronous -> handle_user_abort -> vm_fault -> vm_fault_internal -> vm_fault_enter ->
vm_fault_pmap_enter_with_object_lock -> vm_fault_attempt_pmap_enter -> pmap_enter_options(_internal)`. So a user page fault is handled, the kernel
maps the page, the process faults again at the same instruction, and that repeats (99 % of its time is kernel time) until something changes after
15-175 s. Next: print far/esr of the last exception (patch 0155) to see the address and the fault kind (translation, access flag, permission,
instruction or data), then compare with how this port's pmap sets up PTEs (16K pages, execute permission, AF, shareability).

### What the repeated fault is (far/esr, TLB experiment)

`esr 0x92000047` = EC 0x24 (data abort from EL0), WnR=1 (a write), DFSC 0x07 (translation fault, level 3), at an address in the process's own
`__DATA` (e.g. far 0x104c38008), always at the same pc (a libsystem/dyld store). The pmap HAS a mapping for the address (`pmap_find_phys` returned a
valid page, e.g. pa 0x10cb9c000) but the hardware keeps reporting "no valid descriptor". `tlbi vmalle1is` on all CPUs does NOT help (system time
+0.90 s per second afterwards, same far and pc), so it is not a stale TLB entry. Remaining suspects: the table walker not seeing the PTE the kernel
wrote (non-coherent walk: the line sits dirty in a cache until it is evicted, which would explain the random 15-175 s), or a PTE the pmap writes in a
form the hardware treats as invalid (16K granule/level 3 handling). Experiment `sweetsampletlbi=2`: clean the data caches of every CPU
(CleanPoC_Dcache) and see whether the stuck thread moves on.

### The page tables are right and the hardware still faults (2026-11-19)

Hand walk of the stuck address (patch 0155, 16K granule, T0SZ 17, TTBR0 == pmap root and same ASID, TCR IPS 2 = 40 bit PA):
`L1[0]=0x10a298003 L2[129]=0x10a700003 L3[1064]=0x6000010a6fce43`: a valid page descriptor (type 3, AF 1, AP 1 = EL0 read/write, SH 2 = outer
shareable as XNU sets it, AttrIdx 0, PXN/UXN 1/1, nG 1, OA 0x10a6fc000). The CPU reports a level 3 translation fault for a store to it, repeatedly, for
15-175 s. Neither `tlbi vmalle1is` nor a clean of every CPU's data caches (set/way) helped. In all samples the root table, L3 table and the page are
above 4 GiB (0x1_0000_0000+): the stalls could be tied to pages from the upper half of DRAM (not proven: no sample of a healthy fault to compare).
Experiments `sweetsampletlbi=3` (dc civac of the descriptor's line) and `=4` (break-before-make rewrite) test whether the walker just needs the
entry rewritten.

Result of experiments 3 and 4 (2026-11-19): `dc civac` of the descriptor's line and a break-before-make rewrite, both executed from the SAMPLER's CPU, do not free
the thread (system time +0.90 s per second, same far/pc). They ran on the wrong CPU, and the earlier whole-cache clean (set/way) only writes back,
it does not invalidate. If the stuck CPU holds a stale cached copy of the L3 table line (the descriptor was written by another CPU and the line is not
snooped: every XNU PTE here is Outer Shareable, SH=2, and the SoC may only keep Inner Shareable coherent), only an invalidate executed on THAT CPU
helps, and the entry becomes visible when the stale line is evicted at some random later time. Experiments `sweetsampletlbi=5` (flush every CPU's data
cache with an xcall) and `=6` (`dc civac` of the descriptor on the stuck CPU) test exactly that. If it holds, the fix is inner shareable PTEs
(and TCR_EL1.SH0/SH1) in the arm64 pmap.

## HS200 is not stable over time (first hands-on boot, 2026-11-19 16:00)

A hands-on boot with `sweeths200=6` and no auto restart ran 14 minutes (78 s to 936 s), then the eMMC started failing every command with error -1
(the error interrupt 0x8000 is cleared before the message reads it, hence `int 0x0`): first a write of 8 blocks at 0x1f90168 (925.5 s), then everything.
Processes could not page in code, got SIGBUS ("Too many corpses being created"), launchd died with SIGBUS and the kernel panicked
(`initproc exited -- exit reason namespace 2 subcode 0xa`). 50 MHz high speed never did this in earlier long runs, so the single tuning at boot is not
enough once the card warms up (the sampling phase drifts), and the in-place fallback in `emmc_data_cmd` did NOT trigger: no "HS200 error: falling back"
line anywhere in the log although the kernel contains the string and `HS200 active` was printed at 77 s. Next: print `hs200_active` in the failure path,
and add a retune (re-run the 16-phase tuning) before falling back. HS200 stays off by default (`sweeths200=0`), the hands-on image is
`bonito/tools/boot-manual.sh RUN_ID sweeths200=0`.

### The stall needs more than one CPU (2026-11-19 19:00)

Same kernel, same root, boot-arg `cpus=1`: no stall at all. launchd boot tasks 125.7 -> 131.9 s (SpringBoard exec 6 s after launchd, backboardd 141 s); the same image
with `cpus=8` sat 134 s and 150 s in two stalls. So the page fault livelock (valid PTE, hardware keeps reporting a level 3 translation fault, TLB flushes and
cache flushes on the stuck CPU do nothing) only happens with several cores. Candidate: every PTE and the TCR walk attributes are Outer Shareable (SH=2) as XNU sets
them for Apple's fabric; on the Kryo 360 (A55/A75, DynamIQ) only Inner Shareable memory is kept coherent between cores, so one core's PTE write stays in its cache while
another core's table walker reads stale memory until the line is evicted (random delay). Patch 0158 makes SH_OUTER_MEMORY = 3 and TCR SH0/SH1 inner (what Linux uses).
`CPUS=N` in mkios.sh selects the core count.
