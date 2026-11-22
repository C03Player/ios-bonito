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
