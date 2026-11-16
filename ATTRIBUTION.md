# Attribution and upstream revisions

All upstream material lives (git-ignored) in `third_party/`; nothing from Apple, Google
factory images or Qualcomm firmware is committed.

| Upstream | Revision | Licence | Used for |
|---|---|---|---|
| dipa-studio/ios-sweet | 45263bee51df4a0f55cdf9c649af886399488ddb | MIT (XNU patches: APSL 2.0) | XNU patch series 0001-0081 (not yet applied here) |
| NWylynko/ios-flame | 745c4341af86089efb0df851fa8a27fb8face624 | MIT (+ per-file licences in its CREDITS.md) | shim base (`start.S`, `console.*`, `font.h`, `util.*`, `fdt.*`, `log.*`, `spmi.*`, `shim.ld`, build method), patches 0082-0124 (not yet applied) |
| joranv1/ios-sailfish | 6f27e41ff249f2680540ebe8ee5ab982c5e641c9 | (no LICENSE file found; reference only, nothing copied) | reference |
| LineageOS/android_device_google_bonito | bde25fa67a06be65cdfac2f2e424fc5e7550f524 (lineage-23.0) | Apache-2.0 | board facts (eMMC, boot header v2 offsets) |
| LineageOS/android_kernel_google_msm-4.9 | df5cccf (branch lineage-22.2, shallow) | GPL-2.0 | SDE 4.x register offsets (read, nothing copied) |
| AOSP system/tools/mkbootimg | d2bb0af5ba6d3198a3e99529c97eda1be0b5a093 | Apache-2.0 | boot image packing (host tool) |
| AOSP external/avb | 3328b7b60ae384e1a92f44d7e6f3ac021c2b05be | Apache-2.0 | AVB hash footer (host tool) |
| Google bonito factory image | SP2A.220505.008, sha256 db19d2aaff36f29b7f58f11acad856f9382dafa1ae9160a692782e27cd7b0002 | Google ToS (not redistributed) | recovery; stock DTB/DTBO facts |

Files under `bonito/shim/` derived from ios-flame keep its MIT licence (Copyright (c) 2026
dipa-studio / ios-flame contributors). Not checked: ios-xbill9 trees (only HEADs listed:
xbill9/android_device_google_bonito f64ad1276818e2cd72fcf3f51dbfa50dfb37bfbf).

## XNU patch series (baseline, unmodified)
`xnu/patches/` is a verbatim copy of ios-flame @ 745c4341af86089efb0df851fa8a27fb8face624 `xnu/patches/` (ios-sweet 0001-0081 by
dipa-studio, MIT; flame 0082-0124, MIT), which modify Apple's XNU (xnu-8796.141.3, Apple Public Source License 2.0). The patched files
stay under the APSL 2.0. `.github/workflows/xnu.yml` is ios-sweet's/ios-flame's (MIT) with one change: runs-on macos-14 -> macos-15 (macos-14 images are no longer offered). The first CI run uses this baseline
with MACHINE_CONFIG=SWEET to prove the pipeline; bonito patches (0125+) are not written yet.
