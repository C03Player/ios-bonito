#!/usr/bin/env python3
"""Add boot-time GPU test jobs to the iOS root image's launchd.plist (no mounting: bonito/tools/hfsx_write.py replaces the file in place).

  add-gpu-test-jobs.py IMAGE [UNDO_FILE TOUCHED_FILE]      jobs: com.sweet.vktest (Turnip smoke test), com.sweet.m2vtest (metal2vulkan bridge)
  add-gpu-test-jobs.py IMAGE --remove [UNDO TOUCHED]       take them out again

The programs are in /private/var/local/bin; their output goes to /private/var/local/vktest.log and m2vtest.log (read them with hfsx.py --phone cat in the recovery).
IMAGE may be adb:/dev/block/mmcblk0p72 (the live volume, see hfsx_write.py). Each job is retried every 30 s until it exits 0: the GPU is only ready some 60 s after boot (boot-arg sweetgpudelay).
Run the --touched log through flash-delta.py right after a full flash and BEFORE the phone has booted iOS once.
"""
import os, plistlib, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PLIST = '/System/Library/xpc/launchd.plist'
LIB = '/private/var/local/bin/libvulkan_freedreno.dylib'
JOBS = {
    '/System/Library/LaunchDaemons/com.sweet.vktest.plist': {
        'Label': 'com.sweet.vktest',
        'ProgramArguments': ['/private/var/local/bin/vktest', LIB],
        'UserName': 'root',
        'RunAtLoad': True,
        'KeepAlive': {'SuccessfulExit': False},
        'ThrottleInterval': 30,
        'StandardOutPath': '/private/var/local/vktest.log',
        'StandardErrorPath': '/private/var/local/vktest.log',
    },
    '/System/Library/LaunchDaemons/com.sweet.m2vtest.plist': {
        'Label': 'com.sweet.m2vtest',
        'ProgramArguments': ['/private/var/local/bin/m2vtest', LIB, '/private/var/local/m2v-cache'],
        'UserName': 'root',
        'RunAtLoad': True,
        'KeepAlive': {'SuccessfulExit': False},
        'ThrottleInterval': 30,
        'JetsamProperties': {'JetsamMemoryLimit': 1400},     # the QuartzCore step was killed for its per-process memory limit (jetsam reason 7)
        'StandardOutPath': '/private/var/local/m2vtest.log',
        'StandardErrorPath': '/private/var/local/m2vtest.log',
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
