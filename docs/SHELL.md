# A shell in iOS on bonito (work in progress, 2026-11-24)

Goal: `ssh` into iOS on the Pixel 3a XL. The Pixel 4 (flame) port got there with USB Ethernet + dropbear (third_party/ios-flame/HANDOFF.md, "SSH into iOS works").

## Status
| Piece | State |
| --- | --- |
| dropbear 2025.88 + dash 0.5.12 + toybox 0.8.12 for iOS 16 arm64 | **built** by `.github/workflows/shell.yml` (`bonito/userland/shell/build.sh`), no weak imports; saved in `/usb/work/shell/bin` |
| install onto the phone | `bonito/tools/install-shell.sh KERNEL_RUN` written, **not yet run** (needs the phone in the bootloader) |
| launchd job for dropbear | `bonito/tools/add-shell-job.py` (job `org.iosonpixel.dropbear`, key-only root login, port 22) |
| host side | `bonito/tools/usbbridge.py`: userspace libusb bridge (the dev container has no CAP_NET_ADMIN or tun, so the host's cdc_ether interface cannot be used): ARP, ICMP, TCP relay `serve 2222:22`. Self-tested against a fake phone only |
| **USB link from iOS** | **never worked on bonito** ("the XNU USB gadget never enumerates", docs/RECOVERY.md). Kernel driver: xnu patches 0072-0077, 0127 (SMMU stream 0x740), 0128 (QUSB2 PHY init), boot-arg `sweetusb=2` |

## Next step (needs the phone)
`bonito/tools/usb-test.sh` does one `sweetusb=2` boot (it boots iOS) while the host watches `lsusb` for 1209:0001 and prints the driver's `SWEET USB:` log lines
(soft reset, SMMU bank, QUSB2 before/after, VBUS override, DSTS / GUSB2PHYCFG / GUSB3PIPECTL every 5 s, bus reset / connect events, SMMU faults). Then fix what that shows,
run `install-shell.sh`, boot with `sweetusb=2`, `python3 bonito/tools/usbbridge.py serve` and `ssh -p 2222 -i /usb/work/shell/id_ed25519 root@127.0.0.1`.

## Pitfalls found
- `mkios.sh` put `sweetusb=0` first in the command line and XNU takes the first match: use `USB=2`, an appended `sweetusb=2` is ignored.
- Toybox in CI needs GNU sed, `CC` as a single command (wrapper script), a stub `<sys/disk.h>`, and no ifconfig / netstat / lsof / uptime (iOS SDK lacks the headers or Linux-only calls).
- dash needs `make CC="$CC -std=gnu99"` (autoconf 2.72 adds `-std=gnu23`; dash uses K&R definitions) and `ac_cv_func_strchrnul=no` (iOS 16 lacks `strchrnul`; the SDK's weak import would crash with PC 0).
- iOS root's shell in /etc/passwd is `/bin/sh`, which does not exist on this root: install-shell.sh writes dash there. No symlinks can be created with hfsx_write: toybox applets are shell functions in `/var/root/.profile`.
- Without a USB link, a non-interactive fallback is possible: a launchd job that runs commands from a file and writes the output to a file read back in the recovery (not built).

## First USB test (2026-11-24)
`usb-test.sh` on kernel 45029003673: the driver reached `ud_smmu_setup()` and gave up there (`SMMU smr4 80000740 already matches 0x740`): ABL had already mapped the DWC3's stream 0x740 to context bank 2,
and the code taken from flame (where ABL had not) treated an existing match as an error, so the device was never started and the host saw nothing. Patch 0207 reuses ABL's bank instead.

## USB enumerates (2026-11-24, patches 0207-0210)
Three fixes made the iOS kernel's USB Ethernet gadget enumerate on the host: (0208) reuse the SMMU mapping ABL already made for stream 0x740, without reading its context bank (the bank belongs to the
hypervisor: reading it stalls the CPU, 0207 panicked); (0209) the downstream Linux bring-up order: HS PHY init first, and after the controller reset the dwc3-msm "HS-only" wrapper sequence that switches the pipe clock domain to
the UTMI clock (the SS PHY is not running), otherwise ep0 SETEPCONFIG times out; (0210) a USB id that the dev container may open (it is rootless: only devices with a host udev ACL, such as Google's 18d1, can be opened
read-write; 1209:0001 enumerated but got "Access denied"). `sweetusbfix` (bit 0 post-reset sequence, bit 1 PHY first, bit 2 downstream-style core reset) and `sweetusbvid` / `sweetusbpid` are boot-args.
