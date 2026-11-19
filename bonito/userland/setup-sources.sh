#!/bin/sh
# Fetch the upstream sources of the userland GPU stack into work/ (never committed) and apply the patches.
#   bonito/userland/setup-sources.sh        (run on macOS: CI job .github/workflows/userland.yml, or a Mac)
#
#   work/turnip/mesa      Mesa at the commit UniXNU pins (scripts/data/adreno-compiler-inputs.json) + UniXNU's patches/mesa (in that
#                         order) + phone/turnip/patches (our one patch)
#   work/metal2vulkan     steelbrain/metal2vulkan 720ef1ce (UniXNU's pin) + phone/metal2/bridge/patches (UniXNU's 0001-0006, ours 0100+)
#   work/userland/bin     ios-cc: clang for iOS 16 arm64 userland
# Pins: UniXNU 31506f7e (22 Oct 2026, what ios-flame built against). See ../../ATTRIBUTION.md for licences.
set -eu
cd "$(dirname "$0")"
ROOT=$PWD
W=$ROOT/work
UNIXNU_SHA=31506f7e6fd0bbb7fd596f928c9251761743af3a
M2V_SHA=720ef1ce9ae2a2068999a8675717418b9416283e
mkdir -p "$W/userland/bin" "$W/turnip"

cat > "$W/userland/bin/ios-cc" <<'EOF'
#!/bin/sh
# clang for iOS 16 arm64 userland tools
exec "$(xcrun -f clang)" -arch arm64 -isysroot "$(xcrun --sdk iphoneos --show-sdk-path)" -miphoneos-version-min=16.0 "$@"
EOF
chmod +x "$W/userland/bin/ios-cc"

fetch_commit() {    # fetch_commit DIR SHA URL...  (shallow fetch of one commit, first URL that works)
	dir=$1; sha=$2; shift 2
	rm -rf "$dir"; mkdir -p "$dir"; git -C "$dir" init -q
	for url in "$@"; do
		if git -C "$dir" fetch -q --depth 1 "$url" "$sha" 2>/dev/null; then
			git -C "$dir" checkout -q FETCH_HEAD && return 0
		fi
		echo "fetch from $url failed, trying the next source" >&2
	done
	echo "cannot fetch $sha" >&2; return 1
}

echo "== UniXNU $UNIXNU_SHA (patch list and Mesa pin only)"
fetch_commit "$W/unixnu" "$UNIXNU_SHA" https://github.com/UniXNU/UniXNU
INPUTS=$W/unixnu/scripts/data/adreno-compiler-inputs.json
MESA_SHA=$(jq -r .mesa.commit "$INPUTS")
echo "== Mesa $MESA_SHA"
fetch_commit "$W/turnip/mesa" "$MESA_SHA" https://gitlab.freedesktop.org/mesa/mesa \
	https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa
for p in $(jq -r '.patches[]' "$INPUTS"); do
	echo "   apply $p"
	git -C "$W/turnip/mesa" apply --whitespace=nowarn "$W/unixnu/$p"
done
for p in "$ROOT"/phone/turnip/patches/*.patch; do
	echo "   apply $(basename "$p")"
	git -C "$W/turnip/mesa" apply --whitespace=nowarn "$p"
done

echo "== metal2vulkan $M2V_SHA"
fetch_commit "$W/metal2vulkan" "$M2V_SHA" https://github.com/steelbrain/metal2vulkan
for p in "$ROOT"/phone/metal2/bridge/patches/*.patch; do
	echo "   apply $(basename "$p")"
	git -C "$W/metal2vulkan" apply --whitespace=nowarn "$p"
done
echo "sources ready in $W"
