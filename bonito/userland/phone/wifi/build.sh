#!/bin/sh
# Build wland (the userland Wi-Fi driver: modem boot through the MBA, GLINK, QMI wlfw, the WCN3990 copy engines, en0 bridge) for iOS 16 arm64
# into phone/wifi/out, signed ad hoc. Origin: ios-flame phone/wifi (MIT), see CREDITS in docs/WIFI.md.
set -e
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
mkdir -p out
"$ROOT/work/userland/bin/ios-cc" -O2 -Wall -Wextra -Wno-unused-parameter -o out/wland wland.c smem.c pmem.c glink.c link.c qmi.c rmtfs.c pdmapper.c memshare.c tftp.c scm.c wlfw.c radio.c ipnet.c bridge.c
codesign -f -s - out/wland 2>/dev/null
ls -la out/wland
