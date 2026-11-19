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
