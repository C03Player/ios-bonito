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
