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
