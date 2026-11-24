#!/usr/bin/env python3
"""Add (or remove) the dropbear ssh server launchd job to the iOS root's launchd.plist (same mechanism and safety rules as add-wland-job.py).

  add-shell-job.py IMAGE [UNDO_FILE TOUCHED_FILE]        job: org.iosonpixel.dropbear   (key-only root login on port 22, host keys made on first start)
  add-shell-job.py IMAGE --remove [UNDO TOUCHED]

IMAGE may be adb:/dev/block/mmcblk0p72 (the live volume, journal flushed first). Read the plist back and parse it before booting iOS (a damaged tail
once made iOS panic "No service cache").
"""
import os, plistlib, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PLIST = '/System/Library/xpc/launchd.plist'
JOBS = {
    '/System/Library/LaunchDaemons/org.iosonpixel.dropbear.plist': {
        'Label': 'org.iosonpixel.dropbear',
        'ProgramArguments': ['/private/var/local/bin/dropbear', '-F', '-E', '-R', '-p', '22'],
        'UserName': 'root',
        'RunAtLoad': True,
        'KeepAlive': True,
        'EnvironmentVariables': {'PATH': '/private/var/local/bin:/usr/bin:/bin:/usr/sbin:/sbin'},
        'StandardOutPath': '/private/var/local/dropbear.log',
        'StandardErrorPath': '/private/var/local/dropbear.log',
    },
}


def main():
    a = sys.argv[1:]
    remove = '--remove' in a
    a = [x for x in a if x != '--remove']
    image = a[0]
    extra = []
    if len(a) >= 3:
        extra = ['--undo', a[1], '--touched', a[2]]
    tmp = tempfile.mkdtemp()
    cur = os.path.join(tmp, 'launchd.plist')
    rd = ['--phone'] if image.startswith('adb:') else [image]       # reads of a live phone volume go through the UMS LUN
    subprocess.run([sys.executable, '-I', os.path.join(HERE, 'hfsx.py'), *rd, 'cat', PLIST, cur], check=True, capture_output=True)
    with open(cur, 'rb') as f:
        p = plistlib.load(f)
    jobs = p['LaunchDaemons']
    for k, v in JOBS.items():
        if remove:
            jobs.pop(k, None)
        else:
            jobs[k] = v
    new = os.path.join(tmp, 'new.plist')
    with open(new, 'wb') as f:
        plistlib.dump(p, f, fmt=plistlib.FMT_BINARY)
    subprocess.run([sys.executable, '-I', os.path.join(HERE, 'hfsx_write.py'), image, *extra, 'replace', new, PLIST], check=True)
    print(('removed' if remove else 'added') + f' {len(JOBS)} jobs; launchd.plist now {os.path.getsize(new)} bytes, {len(jobs)} jobs')


main()
