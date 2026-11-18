#!/bin/bash
# Fetch a finished CI kernel, build the standard test image and run one unattended cycle on the phone.
#
#   bonito/tools/ci-cycle.sh RUN_ID NAME "boot-args" [WAIT_S] [MARKER]
#
# RUN_ID: the GitHub Actions run (C03Player/ios-bonito) whose "xnu-kernel" artifact to use. NAME: image and log name
# (logs/NAME.txt). boot-args: extra kernel boot-args (add sweetreboot=SECS!). MARKER: a string that must be inside the kernel
# (checked with strings) so that a skipped patch is noticed before anything is booted (the CI silently skips patches that fail
# git apply --check). Log: logs/NAME.txt; the cycle's own output is printed. Exit status is cycle.sh's (or 4: kernel missing
# the marker, 5: no artifact).
cd "$(dirname "$0")/../.." || exit 5
RUN=${1:?usage: ci-cycle.sh RUN_ID NAME \"boot-args\" [WAIT_S] [MARKER]}
NAME=${2:?}
ARGS=${3:-}
WAIT=${4:-420}
MARKER=${5:-}
D=work/ci-$RUN
if [ ! -f "$D/kernel.development.sweet" ]; then
	rm -rf "$D"
	bonito/tools/fetch-artifact.sh C03Player/ios-bonito "$RUN" xnu-kernel "$D" >/dev/null 2>&1 || { echo "ci-cycle: no artifact for run $RUN"; exit 5; }
	(cd "$D" && unzip -qo xnu-kernel.zip kernel.development.sweet) || { echo "ci-cycle: cannot unzip"; exit 5; }
fi
if [ -n "$MARKER" ] && ! strings "$D/kernel.development.sweet" | grep -q -- "$MARKER"; then
	echo "ci-cycle: the kernel of run $RUN does not contain \"$MARKER\" (a patch was skipped?)"
	exit 4
fi
bonito/tools/mkios.sh "$D/kernel.development.sweet" "$NAME" "$ARGS" | tail -1
exec bonito/tools/cycle.sh "bonito/shim/out/$NAME.img" "$NAME" "$WAIT"
