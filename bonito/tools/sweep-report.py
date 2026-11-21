#!/usr/bin/env python3
"""One line per boot of a sweep: what a series of gesture boots (NAME1..NAMEn, logs/NAMEi.txt and the snapshots NAMEi.png / NAMEi-prev.png) showed.

  sweep-report.py NAME N [N_FIRST]      appends to logs/seam-sweep.txt and prints
Per boot: GPU hang dumps, crashes of backboardd/SpringBoard, swaps per second in the gesture loop (median), and shot-seams.py's blocks / aligned edges /
score for both snapshots. The last line sums the series: boots with a block, boots with a hang, mean score.
"""
import os, re, statistics, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..', '..')


def boot_log(name):
    L = open(os.path.join(ROOT, 'logs', name + '.txt'), errors='replace').read().split('\n')
    i = max((k for k, l in enumerate(L) if 'SWEET EMMC: CID' in l), default=0)
    out, last = [], 0.0
    for l in L[i:]:
        m = re.match(r'\[\s*([\d.]+)\]', l)
        if m:
            t = float(m.group(1))
            if t < last - 1:
                break
            last = t
        out.append(l)
    return out, last


def swaps_per_s(c):
    m0 = re.match(r'\[\s*([\d.]+)\]', c[0])
    t0 = float(m0.group(1)) if m0 else 0.0
    pts = []
    for l in c:
        m = re.match(r'\[\s*([\d.]+)\] SWEET FB: 5s: (\d+) swaps', l)
        if m:
            pts.append((float(m.group(1)) - t0, int(m.group(2))))
    r = [n / (t - pts[k - 1][0]) for k, (t, n) in enumerate(pts) if k and 100 <= t <= 230 and t - pts[k - 1][0] > 1 and n >= 5]
    return statistics.median(r) if r else 0.0


def main():
    name, n = sys.argv[1], int(sys.argv[2])
    first = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    lines, tot = [], {'boots': 0, 'blockboots': 0, 'hangboots': 0, 'score': 0, 'images': 0, 'fps': []}
    for i in range(first, first + n):
        nm = '%s%d' % (name, i)
        try:
            c, last = boot_log(nm)
        except OSError:
            lines.append('%s: no log' % nm)
            continue
        hang = sum(1 for l in c if 'GPU615: hang:' in l and 'IB1 ' in l)
        crash = sum(1 for l in c if re.match(r'\[\s*[\d.]+\] CRASH (backboardd|SpringBoard)\[', l))
        func = sum(1 for l in c if 'is not a MTLFunction' in l)
        fps = swaps_per_s(c)
        imgs = [p for p in (os.path.join(ROOT, 'logs', nm + '.png'), os.path.join(ROOT, 'logs', nm + '-prev.png')) if os.path.exists(p)]
        blocks = score = 0
        if imgs:
            import json
            res = json.loads(subprocess.run([sys.executable, '-I', os.path.join(HERE, 'shot-seams.py'), '--json'] + imgs, capture_output=True, text=True).stdout or '[]')
            blocks = sum(len(r['blocks']) for r in res)
            score = sum(r['score'] for r in res)
        lines.append('%-10s log %4.0fs | hang %d crash %d notMTLFunction %d | swaps/s %.1f | images %d blocks %d score %d' % (nm, last, hang, crash, func, fps, len(imgs), blocks, score))
        tot['boots'] += 1
        tot['blockboots'] += 1 if blocks else 0
        tot['hangboots'] += 1 if hang else 0
        tot['score'] += score
        tot['images'] += len(imgs)
        if fps:
            tot['fps'].append(fps)
    lines.append('SERIES %s: %d boots, %d with a block, %d with a GPU hang, %d images, mean score per image %.1f, swaps/s median %.1f' % (
        name, tot['boots'], tot['blockboots'], tot['hangboots'], tot['images'], tot['score'] / max(1, tot['images']), statistics.median(tot['fps']) if tot['fps'] else 0))
    with open(os.path.join(ROOT, 'logs', 'seam-sweep.txt'), 'a') as f:
        f.write('\n'.join(lines) + '\n')
    print('\n'.join(lines))


main()
