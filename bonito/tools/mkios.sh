#!/bin/bash
# Build a bootable bonito iOS test image with the standard native-size configuration.
#
#   bonito/tools/mkios.sh KERNEL NAME "extra boot-args"     -> bonito/shim/out/NAME.img
#
# KERNEL is a kernel.development.sweet from the CI artifact. The standard arguments give iOS at the panel's native 1080x2160
# with the SpringBoard status-bar override, the touch/button drivers, the persistent log, and the unattended-restart options
# of xnu patch 0150 (sweetpanicreset=1 sweetwdt=25 sweettime=now always on; add sweetreboot=SECS yourself; sweetshot=15 was dropped for the 591-character limit). The GPU firmware from
# AUTORESET= (set, but empty) leaves out the panic reset and the watchdog: an image for a person to use by hand (bonito/tools/boot-manual.sh).
# CPUS=N starts only N cores (default 8): to tell SMP problems from single-core ones.
# /usb/work/gpufw is packed in; DENY=a,b overrides the process deny list (frees command-line room); SAMPLE=name,name turns the process stack sampler on for those processes (see sweetsampledelay) (local only, never committed). Needs /tmp/adt-work/bonito-native.adt (mkadt.py output).
set -euo pipefail
cd "$(dirname "$0")/../.."
KERNEL=${1:?usage: mkios.sh KERNEL NAME \"extra boot-args\"}
NAME=${2:?usage: mkios.sh KERNEL NAME \"extra boot-args\"}
EXTRA=${3:-}
ADT=${ADT:-/tmp/adt-work/bonito-native.adt}
if [ ! -f "$ADT" ]; then
	mkdir -p "$(dirname "$ADT")"
	CANVAS=1080x2160 python3 bonito/tools/mkadt.py "$ADT" >/dev/null
fi
BASE="cpus=${CPUS:-3} debug=0xc serial=3 keepsyms=1 -v wdt=-1 rd=ufsroot0 ufsrw=1 rootrw=1 ufslogsecs=1 sweetusb=0 sweetfb_noconsole=1 sweetfb_native=1 sweetsample=${SAMPLE:--} ${AUTORESET-sweetpanicreset=1 sweetwdt=25 }sweettime=$(date +%s) sweetargs=SpringBoard,UIStatusBarVisualProviderClassName,_UIStatusBarVisualProvider_Split1080 sweethide=com.apple.MobileGestalt.plist sweetdeny=${DENY:-WirelessRadioManagerd,bluetoothd,fairplayd,AegirPoster,SiriTTSSynthesizerAU,MacinTalkAUSP,KonaSynthesizer} unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin launchd_unsecure_cache=1"
python3 bonito/tools/mkpayload.py "$KERNEL" "$ADT" "work/$NAME.payload" --gpufw /usb/work/gpufw --cmdline "$BASE $EXTRA" | tail -1
PAYLOAD=$PWD/work/$NAME.payload bonito/shim/build.sh "$PWD/bonito/shim/out/$NAME.img" 2>&1 | grep -i "error" || true
ls -la "bonito/shim/out/$NAME.img"
