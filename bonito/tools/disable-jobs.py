#!/usr/bin/env python3
"""Disable launchd jobs in the iOS root image's launchd.plist (no mounting: bonito/tools/hfsx_write.py replaces the file in place).

  disable-jobs.py IMAGE [--undo-file U --touched-file T] LABEL [LABEL ...]      set Disabled=true on the LaunchDaemons with these labels
  disable-jobs.py IMAGE --enable LABEL [LABEL ...]                              remove the Disabled key again

Why: on the bonito several daemons cannot work (Bluetooth, the cellular radio manager, FairPlay, ...) and the kernel refuses to start some of them
(boot-arg sweetdeny), so launchd restarts them over and over: a 3 hour hands-on boot logged 8551 xpcproxy launches (lskdd every 1 s,
atc/remotepairingdeviced/useractivityd every 4 s, bluetoothd/WirelessRadioManagerd/fairplayd.H2 every 10 s). Every launch costs CPU on cores
that are already slow. A job with Disabled=true is never started, not even on demand.

IMAGE may be adb:/dev/block/mmcblk0p72 (the live volume in the recovery; needs a clean journal, see xnu patch 0159 sweetnoinit=1) or an image file.
"""
import os, plistlib, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PLIST = '/System/Library/xpc/launchd.plist'


def main():
    a = sys.argv[1:]
    enable = '--enable' in a
    a = [x for x in a if x != '--enable']
    undo = touched = None
    for opt in ('--undo-file', '--touched-file'):
        if opt in a:
            i = a.index(opt)
            val = a[i + 1]
            del a[i:i + 2]
            if opt == '--undo-file':
                undo = val
            else:
                touched = val
    image, labels = a[0], set(a[1:])
    if not labels:
        sys.exit(__doc__)
    tmp = tempfile.mkdtemp()
    cur = os.path.join(tmp, 'launchd.plist')
    rd = ['--phone'] if image.startswith('adb:') else [image]
    subprocess.run([sys.executable, '-I', os.path.join(HERE, 'hfsx.py'), *rd, 'cat', PLIST, cur], check=True, capture_output=True)
    with open(cur, 'rb') as f:
        p = plistlib.load(f)
    found = set()
    for key, job in p['LaunchDaemons'].items():
        if isinstance(job, dict) and job.get('Label') in labels:
            found.add(job['Label'])
            if enable:
                job.pop('Disabled', None)
            else:
                job['Disabled'] = True
    missing = labels - found
    if missing:
        print('no such job (not changed):', ', '.join(sorted(missing)))
    new = os.path.join(tmp, 'new.plist')
    with open(new, 'wb') as f:
        plistlib.dump(p, f, fmt=plistlib.FMT_BINARY)
    extra = []
    if undo and touched:
        extra = ['--undo', undo, '--touched', touched]
    subprocess.run([sys.executable, '-I', os.path.join(HERE, 'hfsx_write.py'), image, *extra, 'replace', new, PLIST], check=True)
    print(('enabled' if enable else 'disabled') + ' ' + ', '.join(sorted(found)))


main()
