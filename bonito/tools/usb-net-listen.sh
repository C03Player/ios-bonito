#!/bin/bash
# Run on the Pi HOST as root (not in the rootless container): configure the phone's USB Ethernet gadget and save its live kernel log.
#   sudo bash bonito/tools/usb-net-listen.sh
# The phone (XNU with boot-arg sweetusb=2) shows up as USB device 1209:0001 (CDC-ECM, 192.168.7.2) and sends its console log as
# UDP to 192.168.7.1:5140. This finds the interface, gives the Pi 192.168.7.1/24, and appends the log to <repo>/logs/xnu-live.log
# (readable from the container). Ctrl-C to stop.
set -euo pipefail
[ "$(id -u)" = 0 ] || exec sudo bash "$0" "$@"
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
LOG=$ROOT/logs/xnu-live.log
mkdir -p "$ROOT/logs"

echo "waiting for the phone's USB network interface (usb 1209:0001) ..."
IF=
while [ -z "$IF" ]; do
	for d in /sys/class/net/*; do
		n=$(basename "$d")
		[ -e "$d/device" ] || continue
		p=$(readlink -f "$d/device")
		while [ "$p" != "/" ] && [ ! -e "$p/idVendor" ]; do p=$(dirname "$p"); done
		[ -e "$p/idVendor" ] || continue
		if [ "$(cat "$p/idVendor")" = "1209" ] && [ "$(cat "$p/idProduct")" = "0001" ]; then IF=$n; fi
	done
	[ -n "$IF" ] || sleep 1
done
echo "found interface $IF"
ip addr flush dev "$IF" 2>/dev/null || true
ip addr add 192.168.7.1/24 dev "$IF"
ip link set "$IF" up
echo "listening on UDP 5140, appending to $LOG"
exec python3 -u - "$LOG" <<'PY'
import socket, sys, time
log = open(sys.argv[1], 'ab', buffering=0)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('0.0.0.0', 5140))
log.write(('\n=== xnu live log %s ===\n' % time.strftime('%F %T')).encode())
while True:
    data, addr = s.recvfrom(65536)
    log.write(data)
    sys.stdout.buffer.write(data); sys.stdout.flush()
PY
