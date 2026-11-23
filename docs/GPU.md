# bonito GPU (Adreno 615) for Metal: findings and plan

Status 2026-11-18: nothing GPU-related runs on the phone yet. This file records what is known and the staged plan.

## Why
iOS has no GPU service on this port: `backboardd` matches `AGXAccelerator` and finds none (boot log), so Metal has no
device. Wallpapers that render with Metal fail (AegirPoster: `-[NUNIAegirRenderer getOrCreateUniformBufferforFrameBufferIndex:]`
gets a nil buffer and throws; CollectionsPoster's saved snapshot is black), and many apps will too.

## Hardware facts (stock bonito DTB, `qcom,kgsl-3d0@5000000`)
- Adreno 615, chip id `0x06010500` (a6xx gen1, same family as A630; the Pixel 4's A640 is gen2).
- Registers 0x5000000 (0x40000); GMU 0x506a000 (0x31000) + GMU PDC 0xb200000; GPU CC (sdm670 gpucc/gfxcc) 0x5090000;
  GPU IOMMU `arm,smmu-kgsl@5040000`; irq 0x12c (GPU), 0x130/0x131 (GMU HFI/GMU).
- Quirks from the DT: `gpu-quirk-hfi-use-reg`, `limit-uche-gbif-rw`, `secvid-set-once`; UBWC mode 2, highest bank bit 14,
  min access length 32 (the A640 port uses bit 15).
- Speed bin fuse 0x41a0, SoC hw rev efuse 0x414c.

## Firmware (taken from the phone's own `vendor_a`, kept out of Git)
Read with `bonito/tools/ext4.py --base $((4492*4096)) vendor_a.img cat /firmware/NAME` (the partition holds a stale ext4
at block 0 and the live one at block 4492). Extracted to `/usb/work/gpufw` (SHA256SUMS next to the files):
`a630_sqe.fw` (command processor, 32136 B), `a630_gmu.bin` (GMU, 32768 B), `a615_zap.{mdt,b00,b01,b02}` (TrustZone zap shader).

## Downstream driver facts (kgsl, `drivers/gpu/msm/adreno-gpulist.h`, LineageOS msm-4.9 @ df5cccf)
A615: core 6 major 1 minor 5, GMEM 512 KB, 0x20 protected regs, features 64BIT | RPMH | PREEMPTION | GPMU | CONTENT_PROTECTION |
IFPC | IOCOHERENT; SQE `a630_sqe.fw`, GMU `a630_gmu.bin` (version 1.003), zap `a615_zap`. The kgsl sources used as the
bring-up reference (adreno_a6xx.c, kgsl_gmu.c, kgsl_hfi.c, a6xx_reg.h, ...) are fetched into `work/ref/kgsl` (not in Git).
Open point: the GMU register offsets for the 0x506a000 block (flame's probe uses offsets relative to the A640 GPU base).

## Stage A result (2026-11-18, patch 0149, log logs/gpu2.txt)
Ran without a hang. GCC cfg_ahb and ddrss_gpu_axi clocks come up; `gpu_memnoc_gfx` (0x17100c) and `gpu_snoc_dvm_gfx` (0x171018)
stay halted after being enabled (not needed for GMU access, needed later for GPU memory traffic: find what they wait for).
CX GDSC turns on (0xf8222000), GMU clock = XO, gpucc ahb/cxo/cx_gmu branches run. GMU ITCM and DTCM take writes
(readback matches), Adreno SMMU ID registers read (cr0 0x200626, idr0 0x48016605, idr1 0x20008005, idr2 0x7511).
Lesson: the kernel's I/O mapping space is small; map registers a page at a time (a 512 KiB map failed with KERN_NO_SPACE).
Next: stage B, GMU boot: SMMU context bank for the GMU stream, RSC/PDC setup, firmware into ITCM/DTCM and the cache
windows, HFI queue table, release the CM3 and poll for the init result (flame's gmu_boot in shim/gpuprobe.c as the template;
firmware reaches the kernel as /chosen properties via mkpayload, as in flame).

## Stage B result (2026-11-18, patch 0149 with sweetgpu=2, log logs/gpu3.txt): GMU BOOTED
RSC and PDC sequencer microcode writes caused no fault; the 32 KiB `a630_gmu.bin` loaded into the GMU ITCM read back
identical; after releasing CM3 from reset `FW_INIT_RESULT` read `0xbabeface` after 1 ms (GENERAL_7 = 1, rpmh_state 0xf,
cx_busy 0x71000000). The GMU clock was left at XO (19.2 MHz), no HFI queue memory was set up, power collapse features were
left off. Image build needs `mkpayload.py --gpufw /usb/work/gpufw` (this build packs the six firmware files into the ADT).

Next, stage C (GX rail and the Adreno core): the DT has `gpu-quirk-hfi-use-reg`, so kgsl powers the GX rail by register
(`a6xx_gfx_rail_on`: BOOT_SLUMBER_OPTION, GX/MX_VOTE_IDX, OOB set/check) instead of an HFI message. Then SPTPRAC, then load
the SQE microcode and zap shader, set up the GPU SMMU context, start the CP ringbuffer and run a fence-write packet.
HFI queues (GMU-side SMMU context) may be skippable at first; try the OOB path without them.

## Stage C plan and inputs (GX rail on, then the Adreno core)
Approach: port the Pixel 4 core driver (`sweet_a6xx.c/.h` + the environment of `sweet_gpu.c`, both inside ios-flame's xnu patch
0095; extracted to work/ref/flame) with renamed symbols (`b6_`/`B6_`, so they cannot clash with the Pixel 4 copy that is still
compiled into the kernel), and replace the A640-specific parts. Facts gathered for the A615:
- Bases: GPU 0x5000000, GPUCC 0x5090000, SMMU 0x5040000 (context banks at +0x8000), GMU = GPU + 0x6a000 (same layout as flame,
  so the RSCC block is at GPU + 0x8d000 and flame's RSCC register list applies unchanged).
- PDC: DT `kgsl_gmu_pdc_reg` 0xb200000; the GPU PDC block is at 0xb280000 (flame: 0xb290000), sequencer memory at 0xb480000
  (flame: 0xb490000). TCS command 2 address is 0x30080 (flame: 0x30090).
- GMU boot is TCM boot (BOOT_CONFIG = 2): the whole 32 KiB image goes to ITCM; no cache windows, no block parsing (done, stage B).
  Still needed for HFI: SMMU context bank for the GMU stream (DT: gmu_kernel iommus <smmu-kgsl 5>, gmu_user <.. 4>), page tables,
  HFI queue table and queues in GMU VA space, QTBL_ADDR (stage B wrote 0).
- cmd-db is read the same way as flame (register 0xc3f000c -> db, magic 0x0c0330db); levels "gfx.lvl", "mx.lvl", "cx.lvl".
- GPU levels (DT, speed bin 0): 430, 355, 267, 180 MHz and an off level (initial level index 3); ARC levels not in the DT (no OPP
  nodes), expected NOM 256, SVS_L1 192, SVS 128, LOW_SVS 64: check against what cmd-db offers. GMU levels 0 and 200 MHz.
- Zap shader: PAS id 13, memory region `gpu_region@0x95c15000` size 0x2000 (flame: 0x9a915000), files a615_zap.mdt + a615_zap.b02.
- CP bring-up values come from kgsl `a6xx_start()`/`a6xx_hwcg` for the A615 (GMEM 512 KB, highest bank bit 14, UBWC mode 2, min
  access length 32), not from flame's A640 table.
- Because of `gpu-quirk-hfi-use-reg` the boot GX rail request is a register OOB (BOOT_SLUMBER_OPTION, GX/MX_VOTE_IDX,
  HOST2GMU_INTR_SET) done before HFI starts, then HFI sends init, firmware version, perf table, bw table and a final test message.
Milestones: C1 = GX power domain on (gx_gdsc on, HFI acked); C2 = CP microcode loaded, zap shader accepted, ring runs ME_INIT;
C3 = a CP_MEM_WRITE fence lands in memory.

## Stage C1 result (2026-11-18, patch 0149 with sweetgpu=3, log logs/gpu4.txt): GX RAIL ON
cmd-db (read via the pointer register 0xc3f000c, magic 0x0c0330db) ARC levels: gfx.lvl 0 16 48 64 128 192 256 320 384 416;
mx.lvl 0 16 16 256 384; cx.lvl 0 16 48 64 128 192 256 384. Level 64 (LOW_SVS) -> gx index 3, mx index 3. The OOB handshake
(BOOT_SLUMBER_OPTION 0, GX_VOTE_IDX 3, MX_VOTE_IDX 3, HOST2GMU_INTR_SET bit 22, ack = bit 30 of GMU2HOST_INTR_INFO, cleared
through GMU2HOST_INTR_CLR) was acked and the GPU CC GX GDSC went from 0x222001 to 0xf8222000 (power on). No HFI was needed.
Next (C2): SPTPRAC power on (GMU_GX_SPTPRAC_POWER_CONTROL 0x1a881 = 0x778000), prove GPU core register access, then SMMU
context for the GPU, SQE microcode, zap shader (SCM PAS 13, region 0x95c15000), ring + ME_INIT, then the fence packet (C3).

## Stage C2a result (2026-11-18, patch 0149, log logs/gpu5.txt): THE GPU CORE ANSWERS
SPTPRAC power-on through GMU_GX_SPTPRAC_POWER_CONTROL = 0x778000: status 0x28 (bit 3 on). GPU core registers read:
rbbm_status 0x1, int_0_status 0, vbif_gx_reset_status 0, cp_rb_base 0; a write/readback of cp_rb_base (0x800) returned the
written value. Next (C2b): kernel pool (kmem_alloc_contig), SMMU context banks for GMU and GPU (SMR/CB counts from IDR0/IDR1:
5 and 5, not the 6 and 5 the Pixel 4 code assumes), GPU page tables, a615 hw_init register list from kgsl a6xx_start(), SQE
microcode, zap shader, ring + ME_INIT.

## Stage C2b/C3 result (2026-11-18, patch 0149 with sweetgpu=4, log logs/gpu7.txt): THE GPU RUNS COMMANDS
Boot-time 4 MiB contiguous pool (a 16 MiB runtime allocation failed on fragmented memory; claim at boot). SMMU: 5 stream groups,
5 context banks; GPU bank cb0 (T0SZ 25, SID 0). a615 register setup, SQE loaded (a630_sqe.fw minus its first dword), CP started:
ME_INIT idle (rptr 9). Zap shader through TrustZone PAS 13: init image, mem setup, auth and reset all rc 0. `SET_SECURE_MODE 0`
idle. CP_MEM_WRITE 0x6a615001 to GPU VA 0x10218000 landed in the pool scratch (read back 0x6a615001). No SMMU fault, hw_fault 0.
The kernel probe (bsd/dev/bonito_gpu.c, sweetgpu=4) is a bring-up harness: nothing keeps the GPU for iOS yet.

Next, in order: (1) turn the probe into a persistent kernel GPU service: bring-up once at boot, a lock, buffer allocation and
mapping into the GPU page tables beyond the 4 MiB pool, a submit/fence path with timeouts and fault recovery, power handling;
(2) publish an IOAccelerator service that Metal can find (see ios-flame patches 0096/0097 for the shape: IOAcceleratorES, user
client with info/alloc/free/import/submit/wait selectors); (3) the userland Metal device. The Pixel 4 SweetMetal path needs
pipelines compiled on a Mac, so the plan is the on-device path from its METAL-NEXT.md: Metal frontend -> metal2vulkan (AIR to
SPIR-V) -> Mesa Turnip built for iOS with a darwin kernel backend -> the kernel user client. That is the large part.

## Stage C4 result (2026-11-18, patch 0149 with sweetgpu=5 sweetgpufast=1, log logs/gpu8.txt): MAPPINGS AND IBs WORK
A 64 KiB kernel buffer (allocated contiguous at boot) mapped with 4 KiB pages at GPU VA 0x100000000 (L1 index 4: two new
tables from the arena) and an indirect buffer in the pool (GPU VA 0x10300000) with two CP_MEM_WRITEs: both markers landed in
the two 4 KiB pages (0xc0ffee11, 0xc0ffee22), the fence written after CP_INDIRECT_BUFFER + WAIT_FOR_IDLE landed, no fault.
The whole bring-up (GMU boot, GX, SMMU, SQE, zap, ring) takes about 1 s with `sweetgpufast=1`.
Next: replace the staged probe by a persistent kernel service (bring-up once at boot without pauses; lock; map/unmap of
arbitrary page lists; submit with a timeout and fault recovery), then the IOAccelerator service.

## Stage 6 result (2026-11-18 23:22, patch 0149 with sweetgpu=6, log logs/gpusvc1.txt): THE SERVICE LAYER HOLDS
bonito_gpu_ready / bonito_gpu_va_alloc / bonito_gpu_map_pages / bonito_gpu_unmap / bonito_gpu_submit (all under one mutex; a
translation fault is cleared and reported). Boot self test: 4 x 16 KiB pages mapped at GPU VA 0x200000000, then 2000 submits of
an IB with a CP_MEM_WRITE, each read back: 0 bad, 0 faults. 1161 us per submit and verify with a 1 ms polling sleep (since
changed to 2 us polling; stage 7 adds an unmap + fault + recovery test). The whole bring-up still ends well inside 30 s of the
probe start, and the watchdog never fired.

Userland constraint found while planning the Metal layer: the Pixel 4 port installs userland bundles over USB networking (ssh).
The bonito port has no network yet, so adding a bundle (for example /System/Library/Extensions/<Metal plugin>.bundle) means
rebuilding the iOS root image and reflashing it (flash-sparse.py, about 20 minutes), or a way to write files into the HFS+ root
from the recovery. A network path (USB gadget or Wi-Fi) would make the userland work much faster to iterate on.

## Reference work to reuse
`third_party/ios-flame` (Pixel 4, Adreno 640) already has the whole stack; see its `METAL-NEXT.md`:
kernel A6xx bring-up (`bsd/dev/sweet_a6xx.c`, patches 0095..0122 in xnu/patches), `SweetAccelerator` (IOAcceleratorES +
user client that Metal finds), a Metal plugin (`phone/metal`, `phone/metal2`) and Mesa Turnip with a darwin kernel
backend (`phone/turnip`). The recommended end state there is Metal -> metal2vulkan (AIR to SPIR-V) -> Turnip -> kernel
user client. Mesa has supported A615 in Turnip since 2020, so the Vulkan route applies to this GPU.

## Plan
0. Done: identify the GPU, collect DT facts and firmware.
1. Bring-up probe (shim, like flame's `gpuprobe.c`): clocks/GDSC via gpucc, GMU boot with `a630_gmu.bin`, HFI, SQE load,
   zap shader via SCM, CP ringbuffer, one packet with a fence write. Differences from the A640 code to resolve from the
   downstream kgsl a6xx sources: GMU/HFI sequence for 615, register init list, GBIF vs VBIF, UCHE/UBWC values, GMEM size.
2. Kernel driver and accelerator service (port 0095..0097 and the later fixes), publish `AGXAccelerator`/IOAcceleratorES.
3. Userland: first the flame SweetMetal path to get Core Animation drawing on the GPU, then metal2vulkan + Turnip.
4. Wallpapers (Astronomy/Collections) and apps as the acceptance test.


## IOKit access (patch 0156, 2026-11-19)

The kernel tree already had the Pixel 4's `SweetAccelerator.cpp`: it publishes `IOAcceleratorES` (the class Metal.framework iterates in
`MTLRegisterDevices`) plus a user client (info, buffer alloc/free, IOSurface import, submit, fences; v1 ABI for SweetMetal and the v2
"NativeAdreno" ABI that Mesa Turnip's darwin backend speaks) on top of a `sweet_gpu_*` kernel API. Patch 0156 backs that API with the Adreno 615
driver (`bsd/dev/bonito_gpu.c`) instead of the Pixel 4's `sweet_gpu.c`/`sweet_a6xx.c` (no longer built):

- boot-args: `sweetgpu=6` (GPU service), `sweetmtl=1` (SweetMetal ABI) or `sweetmtl=2` (Turnip ABI; plugin SweetMetal2), `sweetgpupool=MB` (default 96).
- userland buffers come from a physically contiguous pool claimed at boot (`kmem_alloc_contig`, 96 MiB, flushed once, then only WC/GPU access);
  per-client 4 GiB GPU VA windows from 64 GiB up; submits are synchronous (fence complete on return); a GPU fault is cleared and reported.
- the accelerator reports Adreno 615: chip id 0x06010500, GMEM 512 KB, UBWC 2, highest bank bit 14.
- the framebuffer keeps the CPU composition path (`sweet_gpu_fb_ready()` is false); the GPU scale-to-fit blit is Pixel 4 only.

Verified on the phone: with `sweetgpu=6 sweetmtl=1`, backboardd and SpringBoard match `IOAcceleratorES` -> `SweetAccelerator` (before: 0 found). The user pool
was claimed (`user pool 96 MiB`). NOT verified yet: any user client call (there is no userland client on the phone), the GPU being ready when one connects
(stage A only starts 60 s after boot), and Metal itself: Metal looks for `/System/Library/Extensions/SweetMetal(2).bundle`, which does not exist on the root image.
Limits: the page-table arena is inside the 4 MiB pool (about 380 L3 tables, roughly 760 MB of mapped GPU memory).


## Userland stack and how it gets onto the phone (2026-11-19)

**Build** (CI, `.github/workflows/userland.yml`, artifact `userland-gpu`, about 23 MB, builds in well under an hour on the first try): Mesa Turnip for iOS 16 with the
darwin kernel backend, the metal2vulkan Rust bridge, `SweetMetal2.bundle`, and the test programs `vktest` (Turnip: device, fill+copy, compute, a triangle) and
`m2vtest` (the bridge). Sources and pins: `bonito/userland/README.md` (copied from ios-flame, which draws the Pixel 4 home screen with the same stack and kernel ABI).

**Memory** (patch 0157): GPU buffers for userland come from RAM XNU does not manage (low RAM at 0xb0000000, up to 204 MB), not from managed pages: a write-combined
CPU view of a managed page is a mismatched alias of its cacheable one. XNU on bonito only manages the 1.94 GB above 4 GiB, so every page it hands out is >= 4 GiB.

**Install** (no mounting, the container has no loop devices): `bonito/tools/hfsx_write.py` inserts folders and files into the HFSX root image directly
(catalog B-tree inserts with node splits, allocation bitmap, volume header and its backup), with an undo log (`--undo`) and a log of every byte range written
(`--touched`). Verified on a synthetic image with 800 files (depth 3 tree, `fsck.hfsplus` clean, contents identical, undo restores the original, delta ranges
reproduce the edited image) and on the real image (fsck clean, files read back identical). Work on a COPY (`/usb/work/ios16-root-userland.img`; the original
under `/usb/build` belongs to an unmapped uid and stays pristine). `bonito/tools/add-gpu-test-jobs.py` adds boot-time test jobs to `/System/Library/xpc/launchd.plist`.

**Flash**: `flash-sparse.py IMAGE userdata` (about 20 min when the image is mostly zeros; 60+ min with this much data on this host). Right after a full flash, before
iOS has booted from it (iOS rewrites metadata every boot), `bonito/tools/flash-delta.py IMAGE userdata TOUCHED_FILE` sends only the ranges an edit touched: seconds.

**Boot**: `sweetgpu=6 sweetmtl=2 sweetgpudelay=5` (the GPU is ready before launchd starts backboardd); log lines `MTL2[pid name]:` from the plugin, the test jobs print `PASS`/`FAIL`
lines to the console.


## FIRST BOOT WITH THE USERLAND STACK (2026-11-19 19:00, cpus=1, sweetgpu=6 sweetmtl=2 sweetgpudelay=5)

The GPU is ready 13 s after boot (stage A..C4 and the self-test pass: 2000 submits, 0 faults, 36 us each; 192 MiB user pool in unmanaged low RAM at 0xb0000000).
`backboardd` loads `SweetMetal2.bundle` through the kernel's IOAcceleratorES: `MTL2[57 backboardd]: device Turnip Adreno (TM) 615 in 3815 ms`, the v2 user client methods 1-15
are used, and the pipeline journal on the phone (`/var/local/m2v-cache/backboardd/pipelines.log`) lists 14 Core Animation render pipelines compiled on the device
(AIR -> SPIR-V -> Turnip -> IR3, 90-375 ms each, ok=1). Read from the phone with `bonito/tools/hfsx.py --phone cat PATH OUT` in the log-reading recovery.
Not seen yet: GPU rendering confirmed on screen, `vktest`/`m2vtest` results (their output goes to /dev/console, not into the kernel log: next time use StandardOutPath files),
`generateMipmapsForTexture` is refused by metal2vulkan (MipmapGeneration not implemented).


## GPU hang with Turnip (2026-11-20, findings so far)

- The userland stack is up (device created, shaders compiled on the phone) but the first real Turnip submit hangs the GPU and nothing recovers it (no hang recovery in
  `bonito_gpu.c` yet): `ring (work): rptr == wptr, rbbm_status 0x800005 (busy), cp_status_1 0x81004f3e, TIMEOUT`, no SMMU fault, hw_fault 0, protect_status 0.
  Idle tests (15 s, 30 s, 45 s after init: a plain CP_MEM_WRITE IB) pass, so an idle GPU keeps working; every submit after the first hang fails.
- The CP consumes the whole IB (`IB1 rem 0` in sysmem mode, `rem 2844` of 3324 in tiled mode) and then the GPU never goes idle, so my tail (WAIT_FOR_IDLE, fence write) never runs.
  `TU_DEBUG=sysmem` (file `/var/local/mtl2-tu-debug`, first line = TU_DEBUG) does not avoid it: not a binning/GMEM problem.
- `vktest` creates its Vulkan device (`PASS device created`) but its first submit fails (device already lost after backboardd's hang); its IB starts with
  `EVENT_WRITE CACHE_INVALIDATE`.
- Next: `sweetgputest=N` (patch 0161) issues one packet type as the first idle test (1 CACHE_INVALIDATE, 2 CCU invalidate, 3 LRZ_FLUSH, 4/5 events with timestamp, 6 COND_WRITE5, 7 SET_MARKER, 8 WAIT_MEM_WRITES,
  9 register write, 10 NOP, 11 several events): the one after which the GPU does not idle wedges the A615.
- Pool policy: ordinary processes may only use the GPU pool beyond `sweetgpureserve` MB (default 160) with `sweetgpucap` MB each (256); with the 192 MB low-RAM pool that leaves nothing,
  so test programs got `VK_ERROR_OUT_OF_DEVICE_MEMORY`. Boot with `sweetgpureserve=16 sweetgpucap=128`.
- Live edits of the phone's volume: `bonito/tools/hfsx_write.py adb:/dev/block/mmcblk0p72 ...` in the recovery after a `sweetnoinit=1` boot (xnu 0159: replays and flushes the journal, unmounts, resets).
  Drop the recovery's caches (`adb shell 'echo 3 > /proc/sys/vm/drop_caches'`) before reading back through the mass-storage LUN.

## ROOT CAUSE FOUND: Turnip works on the Adreno 615 (2026-11-20, patches 0167 and 0168)

`vktest` (Vulkan through Turnip, run alone on the GPU, cpus=1) went from 3 FAIL to ALL PASS:
fill+copy 1 MB 0 wrong words, a compute shader compiled on the phone 0 wrong of 65536, a 256x256 draw 20808 red and 44728 blue pixels,
0 other, with no GPU hang and no SMMU fault in the kernel log (log: logs/vk168.txt, vktest.log).

- 0168 was the bug: SweetAccelerator hands `sweet_gpu_map_at` a list of **4K** pages (one physical address per 4K step, the VM page
  is 16K), and the bonito wrapper mapped each entry as a **16K run** (`PAGE_SIZE`). Four PTEs per entry then pointed at the next
  12K after each page, i.e. the wrong physical memory behind most of every buffer (74 % of the words wrong, the same number every run).
  The draw's `cdcdcdcd` was the CPU's own initial fill, never overwritten. Fix: map them as 4K pages.
- 0167 (Linux a6xx_submit wrapper: CCU invalidates in front, the fence written by a CACHE_FLUSH_TS event) is correct and stays, but was
  not the cause of the wrong data.
- The hangs seen in other boots were from other Metal clients (backboardd, SpringBoard, ...) and from `m2vtest` running the bridge
  at the same time, not from the Turnip command streams themselves; to be re-checked now that the mappings are right.
- So the earlier "GPU wedges on the first real Turnip IB" theory (hang dumps, blit tests 30/31) was chasing a symptom of the same
  wrong mapping: the IB itself was fetched from a mapping that pointed at the wrong pages. The blit tests are still valid as hardware checks (2D blit works from high and low RAM).

## UI boot with the GPU on, 8 CPUs (2026-11-20 01:44, kernel run 44647376840, log logs/ui168c.txt)

Boot-args `cpus=8 sweetgpu=6 sweetmtl=2 sweetgpudelay=5 sweetreboot=600`, only WirelessRadioManagerd, bluetoothd and fairplayd denied:

- SpringBoard exec at 86 s (233 s with cpus=1), backboardd, chronod, PosterBoard and the poster extensions follow; no stall, no panic, the
  full timed window ran; no GPU hang and no SMMU fault in the kernel log.
- backboardd holds 35 then 70 MB of the GPU pool and SpringBoard 32 MB, i.e. the compositor and SpringBoard really render through
  Metal -> metal2vulkan -> Turnip -> the Adreno 615.
- Open: AegirPoster (the Astronomy wallpaper) gets "extension check-in timeout" (ns 7 code 3) and is relaunched; with cpus=1 every poster
  extension timed out (too slow), so single-core runs are only for diagnostics. The earlier multi-core boot stall did not show here:
  the wrong 16K-run GPU page-table entries let the GPU write into arbitrary physical pages (BoImport buffers are managed RAM), which can
  corrupt kernel or user memory. To be confirmed over several boots before calling the stall fixed.
- The screenshot pulled from the recovery (SHOT=1) shows stale console text from an earlier boot, not the live UI; the display can only be judged by eye.

### Correction (2026-11-20 02:22, logs/ui168d.txt): the multi-core stall is NOT fixed

A second cpus=8 boot with the same kernel: SpringBoard only at 427 s (86 s in the first boot), the log nearly empty from about 100 s to 400 s
(the stall), and the log ended at 714 s without the timed reset line (the 25 s SoC watchdog reset the phone). So the stall is independent
of the GPU page-table bug; the "GPU writes into wrong pages" idea above was a hypothesis and did not hold. Treat the first boot's 86 s as the lucky case.

AegirPoster is refused its buffer by the pool policy ("the compositor's reserve": 96 MB of the 192 MB pool free, reserve default 160 MB),
which Metal reports as `OutOfMemory(Device)`; `sweetgpureserve=32 sweetgpucap=128` lets it through.

## Big pool, wallpapers on the GPU (2026-11-20 03:14, kernel run 44665296319 = patches through 0169, log logs/ui169.txt)

Boot-args `cpus=8 sweetgpu=6 sweetmtl=2 sweetgpudelay=5 sweetgpureserve=32 sweetgpucap=160 sweetreboot=900`; test jobs removed from launchd.plist and
`/private/var/local/mtl2-clients` = `SpringBoard chronod PosterBoard AegirPoster CollectionsPoster`.

- 0169: the user pool starts at 0xa2000000 and is up to 428 MB (default 384 MB). The recovery's /proc/iomem shows System RAM from
  0xa1c11000 on, persistent_ram only at 0xa1810000..0xa1a10000, and the stock DTB's fixed carve-outs end at 0x9f800000 (reserved-memory
  of stock/dtbs/0.dtb = SDM670). With 192 MB the pool ran out (backboardd 70 MB, SpringBoard 32 MB, AegirPoster 34..70 MB): `OutOfMemory(Device)`.
- With 384 MB: no refusals; backboardd 69 MB, SpringBoard 32 MB, AegirPoster (Astronomy) 70 MB held, AegirPoster stops crash-looping
  after its first launches (check-in timeouts while its Metal device is created, a few seconds each); no GPU hang, no SMMU fault.
- Not verifiable from here: what the display shows (the recovery screenshot is stale console text).

### The multi-core boot stall, measured (5 boots of cpus=8: SpringBoard at +8 s, +351 s, +8 s, +263 s, +11 s)

- Intermittent, about half of the boots; independent of the GPU (it predates it). During the stall the kernel is alive (the 15 s GPU idle tests
  and the other kernel timers keep printing) but nothing moves in userland, and the eMMC stats thread itself reports "193333 ms" since its last run;
  the stall lasts a fairly constant 190..270 s, then launchd goes on (first exec after it: prng_seedctl).
- Already ruled out: TLBI, `dc civac` of the descriptor's line on the stuck CPU, whole-cache flush (set/way and FlushPoC_Dcache) on all CPUs, rewriting the
  descriptor, Inner Shareable page-table walks (0158), the GPU page-table bug (0168), HS200 (off).
- The constant length smells like a timeout, not a race that resolves by chance: look at what userland waits on for 3..4 minutes (a launchd/XPC
  dependency, a keybag/AKS or other service the shim answers late, a lock held by the stuck vm_fault thread), e.g. with sweetsample=* output during the stall.

## WHY THE UI WAS SLOW: the GPU core ran on the 19.2 MHz crystal (2026-11-20, patches 0171-0174; logs/ft2.txt, ft4.txt, ft5.txt)

Symptoms: a Home press animated at about 5 fps (1 fps with the GPU on), every command buffer of backboardd took 0.1-2 s from commit to completion
(the bridge's per-buffer telemetry in /var/local/mtl2.log, `K` lines: 235 ms for one full-screen pass with 1-2 draws, 1.5-2 s for a transition).

How it was found:
- 0171 driver statistics (every 5 s): lock wait 0 us, GPU page mapping a few ms in 5 s, **submit run time 66-115 ms on average, up to 1.8 s**
  (an idle test submit: 80 us): the GPU itself is slow, not the driver or the bridge.
- 0172 slow submit snapshots: the command processor is not stalled, it marches through the IB at ~2.5 thousand dwords/s (one draw per ~25 ms) with
  every block busy.
- 0173 clock probe: RBBM_ALWAYS_COUNT (perf counter, counts at half the core clock) against the 19.2 MHz always-on counter: ~9.6 MHz of counter
  = core on the **19.2 MHz crystal**; `GPU_CC_GX_GFX3D_CLK_SRC` (0x101c cmd, 0x1020 cfg) read cfg 0 = source 0 = bi_tcxo. The 180/267/355/430 MHz of
  the DT are voltage levels (ARC), the clock itself is set by the GMU firmware from the DCVS table that the vendor driver sends over HFI, which
  this driver does not use (it only boots the GMU through the OOB boot path).
- 0174: switch the GFX3D RCG to GPLL0 (600 MHz, GCC `0x52000` bit 0 vote, `0x52004` bit 15 GPU branch; RCG source 5 + half-integer divider field
  2*div-1, then UPDATE): boot-arg `sweetgpuclk` 200/300 (default)/400/600 MHz, `sweetgpulvl` = GX vote level (default 192 = SVS_L1;
  64 for 180 MHz, 128 for 267, 192 for 355, 256 for 430). Probe: 150 MHz counter = **300 MHz core**.

Result (unique command buffers of backboardd, commit to completion): median 723 -> 48 ms, 1-3 draws 243 -> 24 ms, 4-30 draws 422 -> 42 ms,
over 30 draws (transitions) 1856 -> 125 ms; driver submit time avg 66-115 -> 5.9 ms, max 1.8 s -> 104 ms.
The earlier theories (sysmem mode, Reduce Transparency, the DDR vote, 10 Hz panel copies) were not the cause (the 10 Hz cap of
`sweetfb_hz` was real but secondary: 0171 made it 30 Hz).

### Default clock 400 MHz (patch 0175, 2026-11-20, kernel run 44729614830, logs/ft6.txt and ft7.txt)

`sweetgpuclk=400 sweetgpulvl=256` (NOM vote, vendor table: 430 MHz at that level) is stable (no hang, no SMMU fault, probes read a 200 MHz counter = 400 MHz
core in every idle test) and is now the default: unique backboardd command buffers median 34-44 ms (300 MHz: 48), transitions (over 30 draws) 107-113 ms
(300 MHz: 125, crystal: 1856). Swap bursts during a Home press are about 2.4 fps against 1.0 (crystal, GPU path) and 5 (CPU fallback path, which was not the
GPU). The GPU is no longer the limit for most frames; what is left is CPU time in the translation layer, memory bandwidth (GPU buffers are mapped
non-cacheable, no DDR vote on this phone) and the 30 Hz panel copy. Next: a DDR/BCM vote for the SDM670 RSC (the generic RPMh code of patch 0102 is
written for the SM8150 RSC at 0x18220000, SDM670's apps_rsc is at 0x179e0000), and a look at what backboardd spends its time on between submits.

## Visual state with the fast GPU (2026-11-20 12:00-12:20, kernel run 44729614830, normal iOS appearance; logs/vis1.png, vis300.png, vis400b.png, hid2.png)

How to look: boot-arg `sweetshot=SECS` makes the kernel save the panel frame every SECS seconds (two slots at the end of userdata); `ci-cycle.sh` with `SHOT=1` pulls the
newest one into logs/NAME.png (the screenshot is the frame at the END of the run; without sweetshot= it is stale console text). `sweethidtest=30` presses Home every 30 s.

What renders correctly: the Home Screen (icons, labels, dock and Search pill as translucent blue, battery, clock), the lock screen with a gradient wallpaper, the
rounded corners. Known wrong or missing: the Photos widget is a solid white box (the widget's own content, chronod); a 4x18 pixel black/white bar near the screen
centre (x 510-513, y 1431-1448) appears in some frames and not in others (source not found: nothing in the patches draws it, it may be a layer of iOS itself).

Transient status-bar glitch: vis1.png (400 MHz) and vis300.png (300 MHz, another boot) show the SAME garbled top strip (streaks of white and green lines where the
clock and battery should be, ragged black corners), vis400b.png (same boot-args as vis1, another boot) shows a clean status bar "4:11". Identical garbage in two
boots at different clocks means a deterministic frame of the animation (the snapshots are taken at a fixed time after the same Home press and boot timing
varies by a few seconds), not random corruption and not clock related. The streaks are sheared copies of the status bar's white (clock) and green (battery)
elements: the signature of a texture read with a wrong row pitch, or of stale CPU-cache lines in a buffer the CPU just redrew (SpringBoard draws the status bar on the CPU into
an IOSurface that the GPU then samples through a non-coherent mapping; the kernel leaves cache maintenance of imported pages to the client: patch 0115 note).
Next steps: capture more transition frames (sweetshot=1 with sweetreboot aligned to a press), compare the IOSurface bytesPerRow of the status bar layer with the pitch the
bridge gives Turnip, and check where the bridge flushes the CPU cache before a submit (its own `_sys_dcache_flush`).

### Lead on the status bar glitch: textures over no-copy buffers with an unaligned row pitch (2026-11-20, mtl2.log of the vis boots)

The bridge's own warnings (/var/local/mtl2.log, `W` records, one run of backboardd): 16 x `VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT` and 3 x
"a 2D BGRA8Unorm texture of 194x237 over a buffer at offset 0 with rows of 784 bytes: the buffer is a no-copy buffer, and the device makes no linear ...".
The bridge makes a Metal texture over a buffer by creating a Vulkan image with `DRM_FORMAT_MOD_LINEAR` and an explicit plane layout over the buffer's memory; Turnip
refuses a row pitch that is not what it computes itself (194 px x 4 B = 776, padded to 784 by the client, the driver wants a multiple of 64: 832), so the bridge uses
its "shadowed texture" path (a separate image kept in step with the buffer by copies, `metal2vulkan-degrade-shadowed-texture` / `staged-texture-buffer` in the binary).
That path depends on WHEN the copy reads the CPU-written bytes: the CPU writes the buffer through its cache and the GPU copy reads DRAM, so a missing cache
clean (the bridge has its own `_sys_dcache_flush`) or a copy taken before the CPU finished would show as half-old rows, and a wrong pitch in the copy as sheared rows.
Not verified: which texture is the garbled one. Try: (1) in the bridge, flush the CPU cache of a no-copy buffer's range right before its shadow copy is recorded;
(2) log the pitch used by the copy; (3) look at `W` records against the time of garbled frames; (4) as a test only, make the client's pitch 64-aligned (a UIKit/CA backing
store setting) to see whether the warning and the glitch go away together.

## ROOT CAUSE OF THE SMEARED / "TV STATIC" SCREEN (2026-11-20, patch 0178; user photo logs/newss.jpg, captures sw1/sm1/tu1 vs fx1-fx4)

Symptom (the user's photo): whenever something happens on the screen the whole home screen turns into slanted white and coloured bands (icons, the widget, the dock icons
stretched across the full width), while uniform areas (wallpaper) look fine. Captured with the swipe stimulus (patch 0177 `sweetswipe=SECS`, `sweetshot=1`): logs/sw1.png,
sm1.png (Turnip sysmem), tu1.png (noubwc): the same bands, so neither UBWC nor tiled (GMEM) rendering is involved.

Proof that it is a pure row pitch error: flatten the panel snapshot into one pixel stream and cut it again at 1088 pixels (4352 bytes) instead of 1080 (4320): the vertical
coherence drops from 8.3 to 2.8 and the home screen comes out clean (icons, labels, widget, dock; the script is in this session's history: flatten, try pitches 1056..1152, pick the minimum).
The slope of the bands (about 8 pixels per row) is exactly 4352 - 4320 = 32 bytes.

Cause: `AppleCLCD::start` (SweetFramebuffer.cpp) made the panel's default surface with the GPU's 4352-byte rows whenever `sweetgpu` is set (a comment from the Pixel 4, where the
GPU present path later switches the display itself to 4352-byte rows). On bonito that switch never happens (`sweet_gpu_fb_ready()` is false), so the display kept scanning the
panel memory (the ABL splash buffer at 0x9c000000) at 4320 bytes per row while the surface said 4352, and that surface was handed to clients as THE default surface. The render server
alternates between it and its own surface: a frame rendered into the default surface went straight into scanout memory with the wrong stride (the copy code skips a swap of the panel's own
surface: "already in place"), a frame in the other surface was copied correctly, so the panel flipped between a sheared and a good frame whenever the screen changed (TV static in a photo)
and at rest showed whichever came last (about half of the idle captures had a garbled status bar). The earlier findings in this file about the status bar (cache, unaligned pitch)
were real but smaller effects of the same mismatch family.

Fix (0178): the panel surface keeps the display's stride (4320, WC mapping, console and CPU copy target); with the GPU on, clients get a separate RAM surface with
4352-byte rows (`sweet_surface_create_ram`) as the default surface, and every present copies it to the panel like any other surface (3-5 ms at 30 Hz).
Result: four swipe-loop boots (logs/fx1-fx4.png) all coherent at the panel's pitch (score 2.8-3.8, the idle reference is 2.86; the sheared ones scored 8.3-8.6); fx3.png shows the App Library
page sliding in with crisp icons and a blurred dock behind it.
Related earlier fixes that stay: 0176 (clean the CPU cache of the source surface before the CPU copy), bridge 0102 (report 64-byte linear alignment), the no-copy buffer flush in the Metal frontend.
Test tools: patch 0177 `sweetswipe=SECS` (swipes the touch screen: Spotlight pull-down, home gesture, page swipes), `sweetshot=1` + `SHOT=1 ci-cycle.sh`.

### 15 minute stability run on the 0178 kernel (2026-11-20 18:00-18:15, kernel run 44767945185, logs/long1.txt, long1.png)

`cpus=8 sweetgpu=6 sweetmtl=2 sweetgpudelay=5 sweetshot=5 sweethidtest=60 sweetswipe=75 sweetreboot=900`: ran to the timed reset; no crash of backboardd or SpringBoard, no GPU hang,
no SMMU fault, no panic; 136+ gesture rounds; presents stay at a median of 27 per 5 s (max 41) from start to end (no drift); the final frame is coherent at the panel's pitch
(score 1.75) and shows the widgets page: the Batteries widget renders ("100%" with its ring), the other widgets (and the Smart Stack card behind the first) are plain white boxes.
The page-fault livelock workaround (0170) fires about 100 times in the run, as designed.

Where the time goes now during animation (bridge telemetry of backboardd, cumulative): commit-to-complete mean 109 ms per command buffer; the GPU is 30-53 % busy
(about 130 submits per second of 3.5 ms) and only 1.2 % of backboardd's thread samples are running (4 of 328): it waits, it is not CPU bound. Cost items: the no-copy
cache flush of the Metal frontend (new, ~18 GB flushed per 200 s, ~7 ms CPU per command buffer: could be limited to what the command buffer really reads/writes), pipeline
first use (103 pipelines, 62 ms each on average, up to 1 s: hitches in the first minute of an effect; pre-warming from m2v-cache/pipelines.log would hide them), and serial
submits (the driver waits for the GPU after every IB; a call with up to 16 IBs could queue them all and wait once).

### Batch submit (0179) and a crash found by validating it (2026-11-20 evening)

- 0179 queues up to 16 IBs of one submit in the ring with one fence: submits per second fall from ~130 to ~11, but the GPU stays ~46-50 % busy and presents stay at a median of 17-29 per 5 s: no gain,
  so the idle half is not between IBs (it is CPU side before the submit, or waits in the compositor). Kept: harmless, fewer round trips.
- The validation boots showed a backboardd crash (SIGSEGV, 2 of 6 boots, then 5 crashes in 3 of 6 boots with a first fix): `dc civac` in the plugin's `sys_dcache_flush` faults at the first byte after
  a mapped region (far = end of a mapping, page aligned). The Metal frontend's new no-copy flush (flushNoCopy) rounded a texture-over-client-memory range up to a page (fixed: it now uses the caller's length) and the
  bridge/frontend round other lengths to rows and padded pitches, so the final fix is in the flush itself: it clamps every range to mapped, readable memory with vm_region_64 (about 3 us per call).
- Userland CI: the Turnip build broke on the 2026-11-18 runner image (ir3_lexer.c.o: UTIL_ARCH_LITTLE_ENDIAN unset, PIPE_FORMAT_* undeclared; Xcode 16.4, rustc 1.98, brew tools float). Workaround:
  bonito/userland/turnip-prebuilt-run names an earlier run whose Turnip library the workflow reuses (the library is byte-identical to the one that was verified on the phone).
- Disk: the container fills up (test images, CI downloads); tests then die silently. `df -h /` before long sweeps; regenerable: bonito/shim/out/*.img, work/*.payload, work/ci-*, scratchpad downloads.

### Asynchronous GPU submits (0180, 0181; 2026-11-21, kernel run 44818193116; logs/ay1-4, as1)

Why: during animation backboardd's render thread is busy about 60 % of the time (about 50 ms of encoding as user time, then about 56 ms spinning in the kernel for the GPU, counted as system time) and the
two phases were strictly one after the other (a frame took about 110 ms commit to complete, the GPU idle half of the time). The client ABI already has Submit and WaitFence methods and a dependency fence on Submit, but the driver completed
every submit before returning.
What: a v2 Submit queues its packets (up to 16 IBs behind one CACHE_FLUSH_TS fence) and returns the fence at once; WaitFence waits for the GPU's own fence write (sleeping in 200 us timer steps after 0.4 ms of spinning); unmap, pool free and
window free wait for the GPU to be idle first; the ring checks free space before it writes; at most 3 batches may be in flight (back-pressure). Boot-arg `sweetgpuasync=0` turns it off.
Result (windows with 5 or more presents after +130 s, the gesture loop of 0177): presents per 5 s median 42 (mean 42.9, quartiles 34 and 51) against 28 (27.6, 20 and 32) with synchronous submits: about 50 % more frames, 8.4 fps. 4 of 4 boots without panic, hang dump
or backboardd crash, all final frames coherent.
The first version (0180) panicked in the first boot (`kmem_alloc ... failed with 3`, five cores at the same pc, false hang dumps): its waiters spun in delay() for the whole wait, several threads kept most cores busy, and poll-style WaitFence calls with tiny timeouts
were reported as hangs. 0181 fixes exactly that (sleeping waits, hang dump only after a wait of a second or more, back-pressure). Lesson: validate every kernel scheduling change with several boots before a hands-on boot.
Remaining per-frame costs (all on the render thread): about 50 ms of encoding (user time), the no-copy cache flush (about 7 ms per command buffer), the first use of each pipeline (60 ms on average, up to 1 s).

### GPU clock 600 MHz (2026-11-21, logs/ck1-3, kernel 0181)

`sweetgpuclk=600 sweetgpulvl=384` (GPLL0 / 1, vote 384): the clock probe measures 300 MHz on RBBM_ALWAYS_COUNT, which counts at half the core clock, so the core runs at 600 MHz (the 400 MHz default reads 200). 3 of 3 boots without panic, hang dump or crash.
Presents per 5 s in the gesture loop: median 42.5, mean 44.0, against median 42.0, mean 42.9 at 400 MHz: no gain. A 50 % faster GPU does not make the animation faster, so the GPU is no longer the limit with asynchronous submits;
the default stays 400 MHz / vote 256 (less power, lower vote). Telemetry of the same run (logs, mtl2.log of boot prof4): a typical frame is 11 render passes, 101 draws and 7.2 Mpixels shaded (3.1 screens of blur and compositing), command buffer commit to complete
130-170 ms, no-copy cache flush 9 ms and surface cache flush 3 ms per command buffer (54 flushes, 54 MB per frame), 107 pipelines built in 5.6 s over the whole boot. What is left is the render thread's own CPU time (about 50 ms of encoding per frame).

### Torn frames after asynchronous submits, and patch 0182 (2026-11-21, logs/zt1-2, ck3, fx1-4)

The user saw a badly broken screen during the 0181 tests. Snapshots confirmed it (zt1, ck3: tile-shaped blocks of the previous frame inside the new one, a doubled search field); my row-pitch score (coherence at pitch 1080) does not see this kind of damage, so snapshots must be looked at, not only scored.
Cause: the panel copy runs in the vsync tick from the pending swap record and CoreAnimation swaps at "scheduled"; before 0180 a submit returned when the GPU was done, so every surface was complete when it was copied. Fix (0182): `sweet_gpu_wait_submitted()`
(bonito_gpu.c) waits, at most 100 ms, for the fence that was the newest when it is called, and `sweet_fb_flush_src` calls it before the copy reads a surface. The wait is on the tick thread; the render thread is not held.
Result, kernel run 44835558964, 4 boots without panic, hang dump or crash: 3 of 4 snapshots clean, no tile-shaped blocks; fx2 shows a horizontal split at about 20 % of the height (top: a newer, blurred frame; bottom: the previous frame): the panel is scanned out
while the CPU copy writes it, a plain scan-line tear, still there. Swaps per second (from the real time between "SWEET FB: 5s" lines, which are 7 s apart after 0182 because the waits stretch the tick; boot time 100-215 s of the gesture loop):
0179 synchronous 5.0 (median), 0181 7.0, 0182 8.6. The wait did not cost frame rate. Next for the tearing: copy in scan order or double-buffer the panel copy.
In-process profiler (SweetMetal2.mm, marker file /var/local/mtl2-prof; only in userland runs 44832564660 and 44835114391): samples with no frame in our own images are system-library waits and drown the rest; the frontend's per-call rows
(Z call ..., run 44828719915) say per frame: draws 32 ms (65 at 0.5 ms), commit 24 ms, endEncoding 10 ms, flushes 5.6 ms, new render encoder 4 ms. The phone currently runs the plugin of run 44828719915 (no profiler).

### 0183: copy on the swap's fence at the vsync (2026-11-21, kernel run 44839915279, logs/gv1-4)

0182 made the copy wait for the GPU inside the vsync tick: an average copy took 55-78 ms (6 ms without the wait) and started at any point of the panel's scan-out. 0183 replaces the wait: `AppleCLCD::swap` records `sweet_gpu_latest_fence()` with the pending swap, the tick copies only when
`sweet_gpu_fence_done(fence)` (held back at most 6 ticks, then it copies anyway), otherwise the next tick tries again. No thread waits; a copy starts on a vsync tick.
Result, 4 boots (two of 963 s), no panic, hang dump or crash: average copy 6.5 ms again, swaps per second median 7.8 (mean 7.9; 0182 8.6, 0181 7.0, 0179 5.0: within the noise of 0181/0182). Snapshots: gv1-gv3 clean; gv4 (mid-animation) shows three horizontal seams (y 247, 1080, 1797 of 2000) between sharp and blurred bands.
No tile-shaped block of an old frame in any of the 8 snapshots of 0182/0183 (zt1 and ck3 of 0181 had them). The horizontal seams (fx2 had one) are probably the snapshot itself: sweetshot reads the panel memory, which is uncached and slow (some tens of ms for 9 MB), while the 30 Hz copy keeps
writing it, so one snapshot spans several copies and shows a seam at each copy boundary; a tear on the glass cannot be read by a snapshot at all. Not proven. To prove it: take the snapshot from the source surface or while the copy lock is held.

### Blocks of old frames: 0188, the 224x288 bin grid and the detector (2026-11-21)

What the user saw ("the GPU only updates parts of the image, chunks use the old frame") was real and had three causes, in the order they were found:
1. **Torn panel copy** (0182/0183/0187): bands of two frames, because the CPU copy overlapped the display engine's read of the framebuffer. Kick mode (0184/0185: autorefresh off, CTL_START per copy) froze the panel (a lone CTL_START starts no transfer); the panel read pointer (PP+0x14, low half, counts 0x872..0x10e4 per 16.7 ms frame) is what 0187 times the copy from (boot-arg sweetfb_sync=PERMILLE, 450). Bands in snapshots can also be the snapshot itself (the panel memory is uncached: a snapshot read spans several copies).
2. **Dropped tiles after batching (0188)**: 0179 put up to 16 IBs behind one fence flush, and each IB starts with PC_CCU_INVALIDATE_COLOR: the invalidate discarded the previous IB's dirty color lines, so its last tiles never reached memory and the older frame stayed there (dock icons, half a search field, white widget boxes: widgets worked after 0188). 0188 flushes (CACHE_FLUSH_TS into scratch+0x100) between the IBs of a batch.
3. **Still about 1 boot in 12 after 0188**: whole GPU render bins keep an older frame. Every aligned edge the detector found lies at x = multiple of 224 or y = multiple of 288, so the GMEM bin is 224x288 pixels. Experiments queued (kernel 0190: sweetgpuwfi=1 CP_WAIT_FOR_IDLE after every IB, sweetgpubatch=N, sweetgpuasync=0 = old behavior; TU_DEBUG=nobin and =sysmem through /var/local/mtl2-tu-debug).

Tools: `bonito/tools/shot-seams.py IMAGE...` (pure Python; aligned step edges at multiples of 32 pixels, the rectangles they form, full-width seams; the score counts blocks and aligned edges, not bands: the keyboard top and the dock are natural full-width edges), `sweet-shot-pull.py` (both slots, or all 16 with patch 0189: newest = NAME.png, then -prev, -sNN), `seam-sweep.sh KERNEL_RUN NAME N "boot-args"` + `sweep-report.py` (one line per boot: hangs, crashes, swaps/s, images with a block; `logs/seam-sweep.txt`), `set-phone-file.sh` (replace a file on the phone's volume after a journal-flush boot). Pitfalls: `sweetswipe=SECS` is the START delay of the gestures, not their length (SWIPE=210 meant no gestures before the reset: a whole series of idle boots); sweetshot=2 made 3 of 10 boots die about 28 s in (the first snapshot, during the boot storm), sweetshot=7 did not; the pull tool shows an earlier boot's frames when the boot died before its first snapshot (check the log length); never `pkill`/`ps|grep|kill` with a pattern that occurs in your own command line (it killed the shell twice).

### Root cause of the blocks of older frames: the panel copy ran while the GPU still rendered (0192; series of 2026-11-21)

Series of 10 gesture boots each (16 snapshots a boot, `bonito/tools/seam-sweep.sh`, `logs/seam-sweep.txt`), kernel 0188 plus the switches of 0190/0191, share of snapshots with a rectangle on the 224x288 bin grid:
| setting | images with a block | GPU hangs | swaps/s |
|---|---|---|---|
| 16 IBs per fence (the 0188 default) | 11 of 160 (6.9 %) | 0 | 9.6 |
| + CP_WAIT_FOR_IDLE after every IB (sweetgpuwfi=1) | 14 of 160 (8.8 %) | 0 | 9.6 |
| 1 IB per fence (sweetgpubatch=1) | 0 of 160 | 0 | 6.5 |
| TU_DEBUG=nobin (no hardware binning: every draw in every bin) | 24 of 160 (15.0 %) | 0 | 9.4 |
| **16 IBs per fence + the copy fix of 0192** | **0 of 160** | 0 | **9.5** |
(sweetswipe=SECS is the START delay of the gestures: a series with 210 saw no gestures; sweetshot=2 killed 3 of 10 boots about 28 s in.)
Reading: the more GPU work in flight (slower GPU with nobin: more, batches: fewer fences) the more half-rendered frames the CPU copy caught; the cause is in the copy, not in the GPU's writes. CoreAnimation swaps at "scheduled", possibly before the frame's work was submitted, so
the fence recorded at the swap (0183) was an older one; the copy ran during the render, the bins rendered last (the bottom rows) were still old, and since no swap follows the half-rendered picture stayed on the panel. 0192: the tick copies only while the GPU is idle (all submitted work done, at most
12 ticks of waiting) and copies again after any GPU work that followed the last copy. 0193 makes 16 IBs per fence and sweetfb_sync=450 the defaults (no boot-args needed).

### The panel copy per frame (0194, 0195) and the result of 2026-11-21

What the user sees is the panel copy, not what the render server produced: before 0192 the panel got 5.0 copies a second for 9.8 swaps/s, with 0192 (copy only while the GPU is completely idle) 3.6, with 0194 (one snapshot of "the newest fence" two ticks after the latest swap) 3.4:
a frame's GPU latency (about 150 ms from commit to completion) is longer than the time between swaps (about 100 ms), so every new swap restarted the wait and the copy happened only when the wait was forced (12 ticks), sometimes showing an unfinished frame (the 1.2 % blocks of 0194).
0195 keeps a queue of the last 8 swaps, each with the newest GPU fence noted two ticks after its swap (CoreAnimation swaps at "scheduled", before the frame's work was submitted), and copies the newest queued swap whose own fence is done (or that waited 30 ticks).
Series `la` (kernel 0195, no boot-args except the test's): 10 boots, 160 snapshots, **0 with a block, 0 GPU hangs, swaps/s 9.6, panel copies/s 8.2**. 0193 made 16 IBs per fence and the panel-frame copy timing (sweetfb_sync=450) the defaults.
Still unverified by anything but the user's eyes: horizontal tear lines (whether the copy window of 0187 is right), and whether the whole thing looks smooth.

### Performance run of 2026-11-21: 5.0 to 12.4 frames a second, what each step gave

Measured with the gesture series (`bonito/tools/seam-sweep.sh`: swaps per second the render server produced, panel copies per second the panel actually got, 16 snapshots a boot through the block detector; 0 stale blocks and 0 GPU hangs in every series from `la` on):
| step | swaps/s | panel copies/s |
|---|---|---|
| synchronous GPU submits (0179) | 5.0 | 5.0 |
| asynchronous submits (0180/0181) | 7.0 | 5.0 |
| async + the copy waits for the GPU (0182/0183) | 7.8 | 5.0 |
| copy of the newest swap whose own fence is done (0195) | 9.6 | 8.2 |
| device answers cached, profiler off (plugin) | 10.0 | 8.0 |
| translation on a worker thread (plugin) | 10.0 | 8.0 (no gain: the worker is busy only half of the time, the limit is frames in flight times a frame's latency, 221 ms from commit to completion) |
| worker bound to gold core 6, busiest caller to gold core 7 (kern.sweet_bind_cpu, 0196) | 10.7 | 8.4 (a draw call 0.56 ms to 0.26 ms) |
| GPU at 600 MHz, vote 384 (0197 default) | 12.3 | 10.0 (the GPU was the limit again once the CPU side got faster; at 400 MHz nothing changed before that) |
| in-flight limit 6 instead of 3 | 12.5 | 9.6 (no gain, stays 3) |
| **defaults of kernel 0197 and plugin run 44911909012, no boot-args** | **12.4** | **10.3** |
Findings on the way: the kernel schedules the eight cores as one cluster but 0-5 are Cortex-A55 (853 M dependent adds/s) and 6-7 A75 (1995 M/s): a hot thread ran on a silver core three times in four. The in-process profiler (SweetMetal2.mm, /var/local/mtl2-prof starting with 1) with the unstripped
plugin of the CI artifact (`symprof2.py`) put about 10 % of the render server's CPU into the runtime's device capability queries (cached now) and about 15 to 20 % into the cache cleaning loops (flushNoCopy/flushSurfaces, partly on the completion thread; not changed).
Switches: /var/local/mtl2-threads (0 = no worker), /var/local/mtl2-bind (0 = no binding), boot-args sweetgpuclk, sweetgpulvl, sweetgpuinflight, sweetgpubatch, sweetfb_sync, sweetfb_hz. Left: the GPU takes about 73 ms a command buffer (7 Mpixel of blur and compositing a frame), the translation about 90 ms a command buffer on the worker.

### Toward 20 fps: what the measurements say (2026-11-21, evening)

- Real use (the hands-on session of the user, 31 minutes, kernel 0197): active windows median 7.8 swaps/s and 6.0 panel copies/s, peaks 35 and 22 (simple animations already pass 20 fps); the gesture test is the lighter case (12.4 and 10.3).
- Per command buffer in real use (1143 of them): the app creates and commits it in a median of 16 ms, commit to scheduled (translation queue, flush, submit) 39 ms, scheduled to completed (GPU queue and execution) 68 ms, 147 ms commit to completed, one commit every 106 ms.
- `sweetgpustat=1` (patch 0198: the GPU's busy state sampled every 100 us, logged every 5 s): the GPU is busy 69 % (60 to 76 %) of the gesture loop, a fence outstanding 69 % of it, the ring has work 50 to 57 %: so it is NOT saturated; at 10.7 frames/s that is 64 ms of GPU work a frame, a ceiling of about 15.5 frames/s even when fed perfectly.
  Both stages matter: the render server keeps about two frames in flight, so throughput is about two over the latency through translation, submit and GPU.
- Reduce Transparency (EnhancedBackgroundContrastEnabled in com.apple.Accessibility.plist, restored afterwards): 14.2 swaps/s (10.7), GPU busy 57-62 % (so about 21 ms of GPU time a frame less), panel copies 9.0/s (8.0). A visual trade-off, not made the default.
- TU_DEBUG=sysmem (Turnip without its tile memory; now the plugin's default, file content "default" turns it off): series ta 13.8 swaps/s, 10.8 copies/s against 10.7 and 8.0 with the same sampler on (ra), but the later series ua (no sampler) 12.0 and 9.5 against 12.4 and 10.3 without it (qa): the series are noisy, individual boots fall into two groups
  (about 10.8 and about 13.2 swaps/s; the slow ones have 175 ms commit-to-completion against 140 ms: background daemons of the boot storm share the two gold cores with the pinned threads). Three or four boots cannot resolve differences under about 10 %: sysmem is not proven to help.
- Not done: a GPU memory bandwidth benchmark (the clock-independent part of the GPU time, about 57 of 81 ms a frame at 600 MHz, points at memory or per-pass overhead), a DDR/BCM vote (never implemented for this chip), cheaper blur passes, a quiet measurement window (gestures from 150 s).

## DDR and memory NoC vote (2026-11-22, patches 0203 and 0204, kernel run 45019756272)

The GPU moved only about 4.5 GB/s in a copy (vktest bandwidth test: fill 1.18 GB/s, copy 2.25 read + 2.25 write) against a peak of about 14.9 GB/s (two 16-bit LPDDR4X channels
at 1866 MHz, Snapdragon 670/710), and nothing voted the memory bus up. Patch 0203 sends the BCM votes MC0 (DDR, command database address 0x50000) and SH0 (memory NoC, 0x5003c)
to their top levels through the SDM670's apps RSC (DRV 2 at 0x179e0000, TCS from 0x179e0d00; `sweet_rpmh_set_rsc`; the RSC id 0x10100, 8 TCS, 16 commands per TCS answered as expected,
both votes completed with return code 0). Patch 0204 makes it the default (boot-arg `sweetddr=0` turns it off).

| Boots (same gesture test) | swaps/s | panel copies/s | blocks |
|---|---|---|---|
| before, many series | 9.4 to 13.6 (bimodal) | 7.2 to 11.1 | 0 |
| vote on, series dd (4 boots) | 14.6, 15.0, 16.0, 16.3 (median 15.5) | 10.8 to 11.7 (median 11.4) | 0 of 64 images |

One boot with the vote: 1444 command buffers in the run against 828, commit to complete 131 ms against 168. The things tried before it (separate submit thread, early release of
waitUntilScheduled, a render area limited to the drawn bounds, Turnip's default render mode, write-back GPU mapping `sweetgpuattr=1`) moved nothing beyond the boot-to-boot spread.

## 2026-11-23: the default DDR/bus vote (0204) makes the screen show rectangles of other parts of the frame

The user's hands-on boot of the standalone image (kernel 45029003673, which has patch 0204's DDR/NoC vote on by default) showed, only while things move, rectangles that hold other
parts of the frame (not stale-frame blocks). The automated gesture run on that kernel looked clean (0 blocks in 16 snapshots, no hang, 16.4 swaps/s, 12.0 panel copies/s: the
detector reads the panel memory, not the glass, and does not see this). The same kernel booted by hand with `sweetddr=0` (vote off) is fixed on the user's screen. The last kernel
the user judged hands-on was 0197 (before the vote); the vote is the only default that changed since. Cost: the vote is what made the GPU faster (gesture run 16.4 swaps/s against about 10
without), so the cause is probably a timing/coherency hazard between the faster GPU and the panel copy (or the SH0 shared-memory vote), not the vote itself. Standalone images use `sweetddr=0`.
Open: find the hazard and make the vote safe, or make `sweetddr=0` the default (patch 0204's `on = 1`).

