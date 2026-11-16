# Dev container requirements

Persisted (in /workspace): repo, `images/`, `stock/`, `downloads/` (AOSP mkbootimg/avb), `third_party/` (upstream clones, apfs-fuse),
`work/` (CI kernels, payloads). Lost on rebuild: apt packages, the GitHub login. Run `bash tools/setup-container.sh`, then `gh auth login`.

What the container must be given at `docker run` time (all host-side):
- USB devices (already done): phone in fastboot/adb.
- **The 64 GB USB stick** for the IPSW and root image. Simplest: mount it on the host (e.g. `/mnt/usb`) and add `-v /mnt/usb:/usb`
  (or `--device /dev/sde1`; `mknod` is refused in the current container, so block devices are not visible).
- **Building the iOS root image** (tools/mkiosroot.sh-style): `/dev/fuse` + `--cap-add SYS_ADMIN` (apfs-fuse), loop devices
  (`/dev/loop-control` and `/dev/loop*`) and a host kernel with HFS+ support (`modprobe hfsplus`; `grep hfsplus /proc/filesystems`).
  `--privileged` covers all of it. Not verifiable from inside the old container (no /lib/modules, no /proc/config.gz).
