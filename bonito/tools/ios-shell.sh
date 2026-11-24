#!/bin/bash
# Boot iOS with the USB Ethernet gadget, start the userspace USB bridge when the phone appears, and run commands in iOS over ssh (dropbear).
#   bonito/tools/ios-shell.sh ['commands to run on the phone']          env: KR=kernel run (default 45423074766)  SECS=how long iOS stays up (default 420)
#                                                                       HOLD=seconds to keep the ssh port open after the commands (default 0)
# The phone must be in the bootloader and install-shell.sh must have run. This BOOTS iOS (fastboot boot) and the image resets itself after SECS
# (sweetreboot). While it is up: ssh -p 2222 -i /usb/work/shell/id_ed25519 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@127.0.0.1
cd "$(dirname "$0")/../.." || exit 2
KR=${KR:-45423074766}; SECS=${SECS:-420}; HOLD=${HOLD:-0}
CMD=${1:-'uname -a; id; echo; ls /private/var/local/bin; echo; ps -ax | head -5'}
KEY=/usb/work/shell/id_ed25519
SSH="ssh -p 2222 -i $KEY -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8 -o LogLevel=ERROR root@127.0.0.1"
USB=2 bonito/tools/ci-cycle.sh "$KR" ishell "sweetreboot=$SECS" $((SECS + 300)) "sweet_usb" > /tmp/ishell-cycle.log 2>&1 &
CYC=$!
trap 'kill $BR 2>/dev/null' EXIT
echo "waiting for the gadget ..."
for i in $(seq 1 120); do grep -q -i "sweet ios" /sys/bus/usb/devices/*/product 2>/dev/null && break; sleep 2; done
grep -q -i "sweet ios" /sys/bus/usb/devices/*/product 2>/dev/null || { echo "the gadget never appeared"; wait $CYC; exit 1; }
echo "gadget up after ~$((i * 2)) s; starting the bridge"
sleep 2
python3 -I bonito/tools/usbbridge.py serve 2222:22 > /tmp/ishell-bridge.log 2>&1 &
BR=$!
OK=0
for i in $(seq 1 40); do
	if OUT=$($SSH "$CMD" 2>&1); then OK=1; break; fi
	sleep 5
done
if [ $OK = 1 ]; then echo "=== ssh works; output:"; echo "$OUT"; else echo "=== ssh did not work after ~200 s; last error:"; echo "$OUT" | tail -n 5; echo "--- bridge log:"; tail -n 5 /tmp/ishell-bridge.log; fi
[ "$HOLD" -gt 0 ] && { echo "holding the port open for $HOLD s"; sleep "$HOLD"; }
kill $BR 2>/dev/null
wait $CYC
echo "=== cycle: $(tail -n 2 /tmp/ishell-cycle.log | tr '\n' ' ')"
[ $OK = 1 ]
