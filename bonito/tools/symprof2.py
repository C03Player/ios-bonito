#!/usr/bin/env python3
"""Rank the functions the in-process profiler of SweetMetal2 (SweetMetal2.mm, marker file /var/local/mtl2-prof) caught the render server in.

  symprof2.py MTL2_LOG BUNDLE_DIR [N]

MTL2_LOG: /var/local/mtl2.log read from the phone (hfsx.py --phone cat ...). BUNDLE_DIR: the SweetMetal2.bundle of the same build, with its symbols (the CI
artifact). The newest "profiler started" block of backboardd is used: the 30 s dumps are cumulative, so its last "H samples kept K seen S" and the
"HL count pc" (leaf address) and "HA count addr" (every address of the 5-deep chain) rows after it. Only samples with a frame in SweetMetal2 or
libvulkan_freedreno were kept. Prints the top N (default 25) functions by leaf samples and by chain samples (a function counted once per frame it is
in, so a caller shows up for everything below it).
"""
import bisect, collections, re, subprocess, sys

log, bundle = sys.argv[1], sys.argv[2].rstrip('/') + '/'
top = int(sys.argv[3]) if len(sys.argv) > 3 else 25
L = open(log, errors='replace').read().split('\n')
starts = [i for i, l in enumerate(L) if 'backboardd]: profiler started' in l]
if not starts:
    sys.exit('no profiler block in the log')
i0 = starts[-1]
img = [l for l in L[:i0 + 3] if 'backboardd]: images: SweetMetal2' in l][-1]
m = re.search(r'SweetMetal2 (0x[0-9a-f]+), libvulkan_freedreno (0x[0-9a-f]+)', img)
bases = {'sm': int(m.group(1), 16), 'vk': int(m.group(2), 16)}
blk = L[i0:]
heads = [k for k, l in enumerate(blk) if re.search(r'\] ?H samples kept|backboardd\]: H samples kept', l)]
if not heads:
    sys.exit('the profiler has not dumped yet')
last = blk[heads[-1]:]
mh = re.search(r'kept (\d+) seen (\d+)', last[0])
kept, seen = int(mh.group(1)), int(mh.group(2))
HL, HA = [], []
for l in last[1:]:
    m = re.search(r'backboardd\]: H([LA]) (\d+) ([0-9a-f]+)$', l)
    if m:
        (HL if m.group(1) == 'L' else HA).append((int(m.group(2)), int(m.group(3), 16)))
    elif 'H samples kept' in l:
        break


def syms(path):
    out = subprocess.run(['llvm-nm', '-n', '--defined-only', path], capture_output=True, text=True).stdout.split('\n')
    a, n = [], []
    for l in out:
        p = l.split()
        if len(p) >= 3:
            try:
                a.append(int(p[0], 16))
                n.append(p[2])
            except ValueError:
                pass
    return a, n


SY = {'sm': syms(bundle + 'SweetMetal2'), 'vk': syms(bundle + 'libvulkan_freedreno.dylib')}
order = sorted(bases.items(), key=lambda kv: kv[1])


def short(n):
    n = re.sub(r'^_?_R[A-Za-z0-9_]*?metal2vulkan_runtime', 'rt::', n)
    return n[:96]


def sym(a):
    for idx, (k, b) in enumerate(order):
        hi = order[idx + 1][1] if idx + 1 < len(order) else b + 0x1400000
        if b <= a < hi:
            aa, nn = SY[k]
            off = a - b
            best = None
            for o in (off, off + 0x100000000):
                j = bisect.bisect_right(aa, o) - 1
                if j >= 0 and o - aa[j] < 0x40000 and (best is None or o - aa[j] < best[0]):
                    best = (o - aa[j], j)
            return k + ':' + (short(nn[best[1]]) if best else '+%#x' % off)
    return None


def rank(rows):
    c = collections.Counter()
    for n, a in rows:
        s = sym(a)
        if s:
            c[s] += n
    return c


print('profiler: %d samples kept of %d seen (running threads, 5 ms apart); %d leaf rows, %d chain rows' % (kept, seen, len(HL), len(HA)))
for title, rows in (('leaf (where the thread was)', HL), ('in the chain (this function or something it called)', HA)):
    c = rank(rows)
    tot = sum(c.values()) or 1
    print('--- ' + title)
    for k, v in c.most_common(top):
        print('%6d %5.1f%%  %s' % (v, 100.0 * v / tot, k))
