# Evaluation: hosting XNU inside Linux (AFK-surf/xnulinux) instead of driving the phone's hardware from XNU (2026-11-26)

Question: load our XNU into a Linux kernel on the Pixel 3a XL (as xnulinux does) so Linux owns the hardware (GPU, Wi-Fi, modem, audio, power), instead of porting each driver into XNU.
Sources: the repo (`/usb/work/xnulinux/repo`, commit b5ff094, README + docs + fki-ko + xnu-patches read, not run), a mechanical patch test against xnu-8796.141.3, our Lineage kernel tree, and the web (postmarketOS wiki, sdm670-mainline tags).

## What xnulinux is
XNU (macOS 27, xnu-12377) as a loadable module `xnu.ko` in a stock arm64 Linux. Each XNU CPU is a Linux kernel thread pinned to a CPU, run with IRQs/preemption off (`fki_enter`), with XNU's own virtual interrupt flag; the two kernels swap TTBR0/TCR at each world switch;
Linux keeps its exception vectors and forwards XNU faults through kprobes on `__do_kernel_fault` / `die`. `fki.ko` (about 4,400 lines, of which `fki_arm64.c` 649 + common 546 + hostcall 374 are the native backend; `fki_l4h.c` is another backend) gives XNU memory (a carve-out or CMA), timers, files, a net link, IPIs, host calls.
Devices are kexts over FKI: disk (a file), display (framebuffer), input, Ethernet (netdev `xnu0`), keystore, CPU, GPU bridge. GPU: Metal -> metal2vulkan -> Mesa Venus guest driver in macOS -> host call -> `xnudrmd` (virglrenderer Venus on the host's Vulkan driver, on a machine without virtio-gpu it is the virtio-gpu device itself).
SMP works up to 6+ CPUs, 16 KB XNU pages over the 4 KB granule (`vm16k`, PAGE_RATIO 4). Status: proof of concept (docs/production.md lists the stopgaps); verified on Apple Silicon VMs and a DGX Spark only.

## Measured compatibility

| Item | xnulinux assumes | This phone | Verdict |
| --- | --- | --- | --- |
| XNU version | xnu-12377 (macOS 27), `xnu-patches/0001-0012` (567 KB) | xnu-8796.141.3 + our 205 patches (iOS 16, iPhone X userland) | **Hard**: applying their 12 patches to pristine xnu-8796: 541 hunks, **297 rejected (55%)**, 11 files missing; the machine layer patch 0001: 428 hunks, 251 rejected. Conflicts: `osfmk/arm64` (locore.s 26, machine_routines_asm.s 15, copyio.c 12, tlb.h, caches_asm.s, cswitch.s) and `osfmk/vm` (**vm_map.c 55**, vm_fault.c 13, vm_kern.c 10). 20 files are touched by both their series and ours (arm_init.c, pmap.c, arm_vm_init.c, sleh.c, machine_routines.c, kern_exec.c, ...). A manual port, not a rebase |
| Pointer auth (PAC) | required by the README (macOS 27 is arm64e); in the code PAC is an optional capability (`FKI_CAP_PAC`, keys swapped only if present) | SDM670 (Cortex-A75/A55, ARMv8.2) has none; iOS 16 on an A11-class device is plain arm64 | **Fine for iOS 16** (a port to xnu-8796 drops PAC) |
| RAM | >= 16 GB host, 8 GB for XNU | 4 GB | **Tight**: about 2.5-3 GB carve-out for iOS 16 (the iPhone X has 3 GB), Linux with a minimal userland about 300 MB. Needs a reserved-memory region in the device tree; a fixed carve-out works (`carve_base`/`carve_mb`, `nokaslr`) |
| Page size | 4 KB Linux host; XNU 16 KB pages emulated (PAGE_RATIO 4) | our port already runs XNU 16 KB pages over this CPU | Fine; and it ends our GPU 4K-vs-16K mismatch class of bug (Linux maps the GPU) |
| Host kernel | stock distro Linux 7.0, built out of tree; uses xarray, minmax.h, kprobes, `alloc_contig_pages`, CMA, new `__do_kernel_fault` signature | Lineage msm-4.9.337: no xarray/minmax, KPROBES off, `ARM64_SW_TTBR0_PAN=y`, `RANDOMIZE_BASE=y`; modules, CMA, KALLSYMS_ALL on | **4.9: a backport** (xarray -> radix tree/IDR, kprobe argument order, config changes, a rebuilt kernel). **Mainline: the same generation**: [sdm670-mainline](https://gitlab.com/sdm670-mainline/linux/-/tags/sdm670-v6.14.5) tags up to `sdm670-v7.3.5_beta2` (2026-11-17) |
| Hardware under Linux | virtio-gpu / DGX Spark | [postmarketOS wiki, google-bonito](https://wiki.postmarketos.org/wiki/Google_Pixel_3a_XL_(google-bonito)) (mainline kernel listed 6.14.5): works: USB networking, storage, screen, touchscreen, **3D acceleration (Adreno 615)**, Bluetooth, mobile data, SMS, sensors; partial: Wi-Fi, battery, audio, camera, calls; broken: GPS, USB OTG | **Strongly in favour**: mainline already has what our XNU lacks (GPU, Wi-Fi/modem stack, power). The panel needs checking: the 3a XL has two panel variants and sdm670-mainline's notes say one (Samsung) is supported |
| GPU path | Venus (virglrenderer on the host Vulkan driver) | Turnip + msm DRM works on mainline | Plausible but unproven here; slower than our direct path in principle (command serialisation), but it is stable and others maintain it |
| Android 16 ports | n/a | no official LineageOS 23 for bonito found; LineageOS kernel for bonito stays msm-4.9 (last commit 2026-08-15) | Irrelevant: a newer Android on this phone is still a 4.9 kernel; the candidates are 4.9 (Lineage) or mainline |

## Verdict
Worth doing in principle, and the direction I would take if the XNU port can be done: Linux on a mainline SDM670 kernel owns the hardware, XNU (our iOS 16 personality) gets disk/display/input/net/GPU through FKI.
It is **not** a drop-in: the real cost is porting xnulinux's machine layer (about 300 conflicting hunks in the exception, asm and VM code) onto xnu-8796 together with our 205 iOS patches, and the unknowns are stability and performance on Kryo cores with only 4 GB.
What would be thrown away: our XNU drivers for UFS, display, touch and the GPU kernel side. What survives: all iOS userland work (SweetMetal2, wallpapers, shell/dropbear, tools).

## Staged plan with go/no-go gates
1. **Mainline host on the phone (about 1-2 days, no XNU).** Boot an sdm670-mainline kernel with our PC-driven fastboot cycle (`fastboot boot`, no flashing). Check: the panel variant, USB networking, `CONFIG_MODULES`/kprobes/CMA, a reserved-memory carve-out of about 2.5-3 GB, GPU (Turnip on msm DRM), Wi-Fi/modem state. Also valuable by itself: a Linux reference for our Wi-Fi/GPU work.
2. **xnu.ko world switch on the Kryo cores** (days): build their `fki.ko`/`xnu.ko` (+ XNU 12377 from their scripts, possible on GitHub's arm64 Linux runners) against that kernel and see the hosted kernel boot to its first stage markers (no userland needed). Validates CPU, IPI and memory mechanics on this hardware.
3. **Port the machine layer to xnu-8796** (weeks): the 297 rejected hunks, then our iOS patches that still apply (they mostly live in the layers Linux would now replace), then the kexts (XnuDisk, XnuDisplay, XnuInput, XnuEthernet, XnuDrm) against iOS 16's IOKit.
Gate 1 and 2 are cheap and decide whether gate 3 is realistic. Stop after gate 1 or 2 if the carve-out, the world switch or the panel do not work.
