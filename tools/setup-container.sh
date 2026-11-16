#!/bin/bash
# Restore the tool chain after the dev container is rebuilt. Everything under /workspace persists; apt packages,
# the GitHub login and the git credential helper do not. Run as root:  bash tools/setup-container.sh
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)

# hfsprogs (mkfs.hfsplus) is in contrib/non-free on Debian 12
if ! grep -rqs "contrib" /etc/apt/sources.list.d/; then
	cat > /etc/apt/sources.list.d/extra.sources <<'EOF'
Types: deb
URIs: http://deb.debian.org/debian
Suites: bookworm
Components: contrib non-free
Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg
EOF
fi
apt-get update -qq
apt-get install -y -qq adb fastboot usbutils clang lld llvm lz4 device-tree-compiler cpio unzip xxd file gh git curl \
	python3 rsync hfsprogs fuse3 libfuse3-dev cmake g++ make libbz2-dev zlib1g-dev libicu-dev libattr1-dev pkg-config

# apfs-fuse (reads the IPSW's APFS images); source and build tree live in the workspace
if [ ! -x "$ROOT/third_party/apfs-fuse/build/apfs-fuse" ]; then
	[ -d "$ROOT/third_party/apfs-fuse" ] || git clone -q --recursive --depth 1 https://github.com/sgan81/apfs-fuse.git "$ROOT/third_party/apfs-fuse"
	mkdir -p "$ROOT/third_party/apfs-fuse/build" && cd "$ROOT/third_party/apfs-fuse/build"
	cmake .. -DCMAKE_BUILD_TYPE=Release >/dev/null && make -j"$(nproc)" >/dev/null
fi
ln -sf "$ROOT/third_party/apfs-fuse/build/apfs-fuse" /usr/local/bin/apfs-fuse

gh auth setup-git 2>/dev/null || true
echo "tools ready. Still needed by hand: 'gh auth login' (token is not stored in the workspace)."
echo "check: fastboot devices; mkfs.hfsplus; apfs-fuse; /dev/fuse; /dev/loop-control; the USB stick mount"
