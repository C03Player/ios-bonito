# Wi-Fi (WCN3990) on the Pixel 3a XL (bonito)

Status 2026-11-22: planning and inputs. Nothing has run on the phone yet. User request: Wi-Fi only, no Bluetooth.

## Blueprint and what differs

The Pixel 4 port (`third_party/ios-flame/phone/wifi`, "wland", about 5900 lines of C) runs the WCN3990-class radio from userland:
SMEM and GLINK to the modem, QMI (`wlfw`) to the WLAN firmware, the `ath` copy-engine driver, a firmware file server for the modem (tftp and rmtfs), and the
kernel glue `bsd/dev/flame_wlan.c`, `flame_sta.c` and the `redwood_*` apple80211 files (xnu patch 0123, already in the bonito kernel). The modem there is booted by
TrustZone (`PAS`, a few SCM calls).

The bonito's modem is different: `qcom,pil-q6v55-mss` at `0x4080000` with `qcom,pil-self-auth`, a host-managed boot through the MBA image
(downstream `drivers/soc/qcom/pil-msa.c`, `pil-q6v5.c`, `pil-q6v5-mss.c` in `third_party/lineage-kernel-msm-4.9`; read with `git show HEAD:path`, the checkout is sparse).
That part of wland (`boot_modem`) has to be replaced; the rest is reused with the addresses below.

## Inputs (the user's own firmware, kept out of Git, in `/usb/work/modem`)

- `modem_a.img` (FAT16, 4096-byte sectors; `bonito/tools/fat.py`), `fsg.img`, `fsc.img`, `modemst1.img`, `modemst2.img` read read-only from the phone's recovery.
- `fw/`: `mba.mbn`, `modem.mdt`, `modem.b00` to `b30`, `qdsp6m.qdb`, `modemr.jsn` (from the modem partition's `/image`), and from the vendor image
  (`ext4.py --base $((4492*4096))`): `wlanmdsp.mbn`, `bdwlan-bonito.bin` (+ `-USA`, `-ROW`). `SHA256SUMS` next to them.

## Addresses (stock DT, `stock/dtbs/0.dts`)

| What | Value | flame |
|---|---|---|
| modem region | `0x8b000000`, **0x9800000** (the B4 dtbo overlay; the base DT says 0x7e00000; the modem image's segments run to 0x94800000) | `0x8dc00000` |
| WLAN MSA | **`0x94d00000`**, 0x100000 (overlay; base DT 0x93300000) | `0x8bc00000` |
| MBA region | **`0x95600000`**, 0x200000 (overlay; base DT 0x93c00000, which lies inside the modem region on the bonito) | not used |
| GLINK smem | `0x86000000`; irq register `0x1799000c`, mask `0x1000` | `0x17c0000c` |
| QDSP6 SS | `0x4080000` (0x100); halt q6 `0x1f63000`, modem `0x1f65000`, nc `0x1f64000`; RMB `0x4180000`; restart `0xc2b0000`; pdc sync `0xb2e0100`; alt reset `0x4180044` | |
| cmd-db locator | `0x0c3f000c` (same), apps RSC DRV2 `0x179e0000` (works since patch 0203) | |
| WCSS | `0x18800000` (check) | `0x18800000` |

## The boot sequence to write (downstream pil-msa.c / pil-q6v5.c, `qdsp6v65-1-0`)

1. Proxy votes: vdd_cx, vdd_mx, vdd_mss to the top levels (ARC resources `cx.lvl`, `mx.lvl`, `mss.lvl` through `PMEM_RPMH`, as wland's `vote()`), XO and prng clock,
   AOSS message "mss-pil" (wland's QMP code).
2. `pil_mss_power_up`: external BHS (`cxrail_bhs`, if the DT has it: no register in this DT, skip).
3. `pil_mss_enable_clks`: GCC branch clocks iface (cfg_ahb), bus (q6_bimc/memnoc axi), mem, gpll0_mss, snoc_axi, mnoc_axi (CBCR bit 0, wait for bit 31 to clear); register offsets from `gcc-sdm845.c` (work/ref) and the downstream `clk-gcc-sdm670`.
4. Save `QDSP6SS_DBG_CFG`; assert resets (pdc sync bit 9 set, alt reset 1, restart reg 1), wait, deassert (restart 0, wait 200 us, alt reset 0, pdc sync clear).
5. Load `mba.mbn` (at most 1 MB) into the MBA region (`0x93c00000`); write its address to `RMB_MBA_IMAGE` (rmb +0x00); `RMB_PMI_CODE_START` and `_LENGTH` = 0 (no debug policy).
6. `__pil_q6v55_reset` then `__pil_q6v65_reset`: ACC override (`QDSP6SS_STRAP_ACC` = 0x20), assert resets and stop core, XO branch clock (QDSP6SS +0x38 bit 0, wait bit 31),
   BHS on (`PWR_CTL` bit 24), wait BHS ack, LDO and memory power sequence, clamp release, sleep clock (`QDSP6SS_SLEEP` +0x3c), `BOOT_CORE_START` (+0x400) = 1, `BOOT_CMD` (+0x404) = 1,
   wait `MSS_STATUS` (rmb +0x40) bit 0.
7. Wait `RMB_PBL_STATUS` (rmb +0x04) = 1, then `RMB_MBA_STATUS` (+0x0c) = 1 (XPU unlocked).
8. Metadata: copy the `.mdt` (ELF header, program headers and hash segment) into a 4 KiB-aligned physical buffer, `RMB_PMI_META_DATA` (+0x10) = its address,
   `RMB_MBA_COMMAND` (+0x08) = 1, wait `RMB_MBA_STATUS` = 3 (headers authenticated), negative = error.
9. Load every modem segment (`modem.bNN` into the modem region at its program header's physical address); `RMB_PMI_CODE_START` = first segment address, command 2 (`CMD_LOAD_READY`),
   `RMB_PMI_CODE_LENGTH` = running length (the MBA authenticates while it grows); wait `RMB_MBA_STATUS` = 4 (`AUTH_COMPLETE`).
10. The modem runs: SMEM, GLINK (`mpss` edge), QMI, then wlfw (existing wland code), MSA at `0x93300000`, `wlanmdsp.mbn` through the modem's tftp requests, `bdwlan-bonito.bin`.

No hyp-assign is needed: this DT has no `qcom,subsys-vmid`; the MBA protects the regions itself.

## Open questions

- Whether the GCC clock register offsets and the QDSP6 v65 power sequence below `__pil_q6v55_reset` (BHS ack, LDO, memory sequence) need the full downstream text (read it from `pil-q6v5.c` lines 470-640 when writing the code).
- The modem needs `modemst1/2`, `fsg`, `fsc` through rmtfs (wland's `rmtfs.c`, partition names `/boot/modem_fs1`, `/boot/modem_fs2`, `/boot/modem_fsg`, `/boot/modem_fsc`).
- iOS side: `en0` and apple80211 come from the flame kernel glue; whether iOS's `wifid` accepts it on this phone is unknown.

## Credits

`bonito/userland/phone/wifi` starts as a copy of ios-flame's `phone/wifi` (ios-flame commit 745c434, MIT, Copyright (c) 2026 dipa-studio / the ios-flame contributors;
itself a fork of ios-sweet). Changed for bonito: the modem boot (TrustZone PAS replaced by the MBA sequence of section "The boot sequence to write"), the IPC register
(`0x1799000c`), the modem, MBA and WLAN MSA regions, the board data names (`bdwlan-bonito*.bin`), and the WLAN supplies (PM660 L5 0.8 V, L9 1.8 V, L6 1.3 V, L19 3.1 V
instead of the Pixel 4's PM8150 LDOs). Not done and untested on the phone: everything.

## Status of the code (2026-11-22)

- `wland.c` `boot_modem()` now does: ARC votes (cx, mx, mss), metadata from `modem.mdt`, `mba.mbn` into `0x93c00000`, AOP "load_state" message, `mba_start()` (GCC clocks, subsystem
  reset, `RMB_MBA_IMAGE`, QDSP6 boot FSM, PBL/MBA status), header authentication (`RMB_PMI_META`, command 1, status 3), each segment announced as it is loaded (command 2, growing
  `RMB_PMI_LENGTH`), status 4, then waits for the SMP2P "ready" bit as before.
- It compiles in a syntax check only (host gcc with a sysctl stub). The iOS build is a step of the userland workflow (artifact `userland-gpu`, file `wland`).
- Needed on the phone before a run: `/var/local/fw/modem/` with `modem.mdt`, `modem.bNN`, `mba.mbn`, `wlanmdsp.mbn`, `bdwlan-bonito*.bin`; `/var/local/fw/rmtfs/` images
  (`modemst1.img`, `modemst2.img`, `fsg.img`, `fsc.img`, see `rmtfs.c`), kernel boot-arg `sweetpmem=1`, run `wland` as root.

## Memory map lesson (2026-11-22)

The device tree in the stock boot image (`stock/dtbs/0.dtb`) is the base SDM670 tree. The phone's real reserved memory comes from the model's dtbo overlay
(`stock/dtbo.img`, entry 8 = "Google Inc. MSM sdm670 B4 PVT v1.0": B4 = bonito, S4 = sargo; decompile with `dtc -I dtb -O dts`). Its regions: modem `0x8b000000+0x9800000`,
video `0x94800000+0x500000`, WLAN MSA `0x94d00000+0x100000`, cdsp `0x94e00000+0x800000`, MBA `0x95600000+0x200000`, adsp `0x95800000+0x2200000`. Using the base tree's addresses put
the MBA and the metadata inside the modem region, so the modem's own segments collided with them and the load stalled.

## Results of the first bring-up runs (2026-11-22)

What works on the bonito, from `wland uptest 100 120` (`bonito/userland/phone/wifi`, run as a launchd job from the phone's boot, kernel 0204+0206 with `sweetpmem=1`):

- The WLAN SMMU stream is set up (apps SMMU `0x15000000`, stream `0x40` mask 1 from icnss `iommus`), the four WLAN rails (PM660 L5, L9, L6, L19) and `rfclka2` are on, the QDSS clock message is acked.
- The modem boots through the MBA: GCC clocks, subsystem reset, boot FSM, PBL and MBA status 2, the metadata in the MBA region, `hyp_assign` of the MBA, the metadata, and the whole modem region (to the kernel and the modem's VM),
  69 MB of segments (rate: the whole load takes about 5 s once the phone's boot has settled), status 4 "image authenticated".
- The modem says ready 100 ms later; GLINK (`glink_ssr`, `IPCRTR`, ...) and QRTR work; rmtfs (modem_fs1, fs2, fsg, fsc; 2 MiB shared buffer), pd-mapper and TFTP serve the modem; it registers about 15 QRTR services.

What still fails: about 0.9 s after "ready" the modem reports ERR-FATAL with the crash reason (SMEM item 421) `:0:[rf]EX:kernel:0:rf:171:PC=d949b6d4`, identically in every run; WLFW (QMI 0x45) is never published, so the radio never starts.
Tried without effect: the three proxy-consumer rails (`ldoa1` 1.2 V, `ldoa11` 1.8 V, `ldob1` 0.88 V; the stock DT marks them `qcom,proxy-consumer-enable`).

Pitfalls found on the way (each cost a test cycle):
- The stock boot image's DT is the generic SDM670 one; the real memory map is in the model's dtbo overlay (see "Memory map lesson").
- A 32-bit RMB register cannot take an address above 4 GiB (managed RAM): the MBA's metadata lives in the MBA region.
- The per-process jetsam memory limit kills the job silently (kernel log: `CRASH wland sig 9, reason ns 1 code 0x7`): `memorystatus_control(7, ...)` first.
- Reading the firmware during the phone's own boot is very slow (the eMMC is saturated): wait about 100 s.
- `sweetpmem` has no effect unless `sweet_pmem_init()` is called (patch 0205: 0126 had removed it); the RPMh RSC default must be the bonito's (patch 0206).
- After `hfsx_write.py` in the recovery, `adb shell sync` before reading back; a read in the same session may show the old data.

Also tried without effect: blank modem storage (all four rmtfs images zeroed on the phone, so the modem sees a never-provisioned EFS/NV/calibration): identical crash `rf:171:PC=d949b6d4`, same timing, same tftp requests. The stored (encrypted-looking) modemst/fsg data is not the cause.
Findings on the crash address: no program header of `modem.mdt` covers `0xd949b6d4` (segment 16, va `0xc0add000`, is read-only data; the "pointers" near that address in it are a table of NV-variant descriptors, not vtables). The modem's user-PD `wlan_pd` (`modemuw.jsn`, loaded from `wlanmdsp.mbn` over tftp) is never requested before the crash.
Crash-context findings (runs with `wland` dumping the modem's memory after ERR-FATAL):
- The modem region in DDR matches the segment files for every read-only segment (`verify:` lines; one word differs at `+0x3c` of segment 2, probably a boot-time patch), including segment 16, the compressed demand-paged code. The loader and the memory path are not the cause.
- The faulting task is `rf` (the RF command thread). The `0xd8..`/`0xd9..` range holds executable code (callback tables mix `0xc066....` and `0xd9491cc8`-style pointers), about 28 MB, probably the decompressed view of segment 16 (the modem has a `Q6zip_Clk` task); no register or TLB state was found.
- SMEM item 602 (minidump table) has two entries, neither the modem's: one with 18 regions is the AOP (`MSGRAM`, `CODERAM`, `DATARAM`); the other points at `0x8575c018`. Reading an address outside SMEM and the modem region from the AP stalls the bus (one run hung): only read those two ranges.
- The B4 overlay changes only the memory map for the modem (no MSS/RF properties); the code's MBA (`0x95600000`) and MSA (`0x94d00000`) match it.
- memshare (QMI service 0x34, instance 1; `memshare.c`, sizes as the stock DT: client 1 = 5 MiB, clients 0 and 2 = 0): the modem DOES use it (it queried client 1's size and allocated the block as soon as the server existed), so it was a real gap; the crash is unchanged (same PC, the `rf` thread's tid moves 171 -> 172 because one more thread exists). The modem's message database also shows `dynamic_heap.c` ("get_heap_memory: HLOS returned error") as a memshare client; it did not ask.
- Exception record in the DDR image: 16 bytes `SP FP LR PC` = `c3b53f60 c3b53ff8 d949b680 d949b6d4` about 0xea bytes before the `EX:kernel` text; the PC value also sits (with LR `d949b680` next to it) at five other places (stacks / exception areas). Walking the FP chain from `0xc3b53ff8` (PA = VA - 0x35000000) gives eight valid frames whose return addresses (`0x562205c9`, `0x4f0b3e11`, ...) are outside every segment of `modem.mdt`: the callers are dynamically loaded modules (the image also carries a Hexagon shared object in segment 29 and a loader stub in segment 26). Not resolved.
- Segment 26 is Hexagon code (PC-relative loader stubs); segment 29 is an ET_DYN ELF for Hexagon (`.dynsym` 0x271 symbols, no function names resolved yet for the d9.. range).
Pitfall: a `launchd.plist` written by `add-wland-job.py` once read back with a damaged tail (valid length, invalid bplist trailer) and iOS panicked "No service cache"; always read the plist back and parse it before booting. Restore from the stock image: `hfsx.py /usb/build/ios16-root-20H392.img cat /System/Library/xpc/launchd.plist`.

Candidates for the RF crash, not tried: what else Android's kernel does before the modem starts (TLMM pin ownership for the RF front end, clocks the downstream driver enables for the modem such as `gcc_mss_axis2`,
`rfclka1`, other ARC levels), RF calibration in `modemcal`/`persist` (the modem-side calibration partitions, separate from modemst/fsg).
The test job has been removed from the phone's launchd.plist (and the files stay in `/private/var/local/fw`, `/private/var/local/bin/wland`).

## The RF crash is solved (2026-11-23): the modem needs its board identity in SMEM

Found by running the stock downstream kernel's own modem bring-up in the Linux recovery (vendor image loop-mounted from tmpfs, the real `modem_a` at `/firmware`, Android's
`rmt_storage`, `tftp_server`, `pd-mapper`, `irsc_util`; `setprop ctl.stop sweetreboot` first) and diffing its logs (`/sys/kernel/debug/ipc_logging`, `msm_ipc_router/dump_servers`,
`ipclog_smem`) against ours. That run is healthy: ready at +0.05 s, Power/Clock ready at +0.9 s, `wlan_pd` and the WLFW service at +1.1 s.

- **Root cause:** the downstream `modemsmem` driver allocates SMEM item **134** (32 bytes, any host) and fills it with the board identity the modem uses to pick its RF card config:
  `{version 0, modem_flag 0, major_id 3, minor_id 0x28, subtype 1, platform 5, efs_magic 0, ftm_magic 0}` (this phone, from `modemsmem/modem_smem` in sysfs; CDT hwid 0x05032801).
  `wland` never wrote it, so the RF task dereferenced a NULL config pointer (`rf:171:PC=d949b6d4`, SSR cause 0x70 = data-read TLB miss at address 4) and the modem also asked for
  `/boot/modem_fsg_oem_*` that Android's modem never asks for. `modemsmem_publish()` in `wland.c` (called from `smp2p_create()`) fixes it: the modem now raises handover and registers ~30 services.
- Also matching Android: RFSA (QMI service 0x1c instance 1, `rfsa.c`, answers client 1 with the 2 MB RMTFS buffer), no memshare (a healthy router table has no 0x34; `MEMSHARE=1` re-enables it),
  no WLFW host-capabilities request by default (this firmware answers MALFORMED_MSG; `WLFW_HOSTCAP=1`).
- **TFTP speed:** the 3 MB `wlanmdsp.mbn` must arrive before the user-PD grace timer (~30 s). 7680-byte blocks with the window's one-shot GLINK intents requested together
  (`link_want_intents`) take 396 blocks / ~1.6 s of intent waiting (was >40 s and a duplicate-ACK resend storm with 1 KB blocks).
- State when the Wi-Fi work was paused: `wlfw: server at 0:51`, registered for indications; the next step in a run is MSA info/ready, capability, board data download (not reached yet).
- Tools for the comparison: `/usb/work/android-probe/run/` (`run_probe.sh`, `phone_setup.sh`, captures in `cap/`), the full modem DDR dump `/usb/work/modem-dump/modem-ddr-wf23.bin`.
