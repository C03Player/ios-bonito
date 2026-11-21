#!/usr/bin/env python3
"""Find the harsh edges that blocks of an old frame leave in a framebuffer snapshot (PNG from sweet-shot-pull.py).

  shot-seams.py [--json] [--t N] IMAGE.png [IMAGE.png ...]

Two kinds of damage have been seen on the bonito panel (docs/GPU.md):
  * BLOCKS: rectangles whose edges lie on the GPU's 32-pixel tile grid and that hold an older frame (a dropped tile flush: patch 0188; before it,
    dock icons missing, half a search field, a black bottom third). They show as long straight step edges at x or y multiples of 32.
  * BANDS: full-width horizontal seams where the panel copy overlapped the display engine's read (docs/GPU.md, 0187): above the seam one frame, below
    another. They show as a row where most columns jump (or the structure of the page breaks) between two neighbouring rows.

For each image it prints the aligned step edges found (x or y, extent, strength), the rectangles they form (a vertical and a horizontal aligned edge
that meet), the full-width seams, and a score: blocks*10 + aligned edges (higher = more likely damaged; bands are listed but not scored: natural full-width edges such as the keyboard top and the dock trigger them). Natural UI edges that happen to lie on the
grid exist (1 in 32 of the straight ones), so the score is for comparing sets of images, not a verdict on one: look at the rectangles it lists.
Pure Python (no numpy in the container): about 2 s per image.
"""
import json, struct, sys, zlib

T = 10          # minimum step (largest channel difference) of an edge pixel
SMOOTH = 6      # the pixels next to the step, on each side, differ by less than this: a step between two flat sides
MINRUN = 96     # shortest straight edge counted (pixels)
GRID = 32


def load(path):
    d = open(path, 'rb').read()
    pos, idat, w, h = 8, b'', 0, 0
    while pos < len(d):
        ln, = struct.unpack('>I', d[pos:pos + 4])
        t, body = d[pos + 4:pos + 8], d[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if t == b'IHDR':
            w, h = struct.unpack('>II', body[:8])
        elif t == b'IDAT':
            idat += body
    raw = zlib.decompress(idat)
    stride = w * 3 + 1
    return w, h, [raw[y * stride + 1:(y + 1) * stride] for y in range(h)]


def runs(flags, minrun):
    """(start, length) of the runs of true values (a gap of up to 3 false values does not break a run)."""
    out, start, gap = [], None, 0
    for i, f in enumerate(flags + [False] * 4):
        if f:
            if start is None:
                start = i
            gap = 0
        elif start is not None:
            gap += 1
            if gap > 3:
                end = i - gap + 1
                if end - start >= minrun:
                    out.append((start, end - start))
                start, gap = None, 0
    return out


def px(row, x):
    return row[3 * x], row[3 * x + 1], row[3 * x + 2]


def step(a, b):
    return max(abs(a[0] - b[0]), abs(a[1] - b[1]), abs(a[2] - b[2]))


def analyse(path, t=T):
    w, h, rows = load(path)
    res = {'image': path, 'size': [w, h], 'vedges': [], 'hedges': [], 'blocks': [], 'bands': []}
    # vertical edges on the grid: column x (x % 32 == 0), a step between pixel x-1 and x, smooth either side
    for x in range(GRID, w - 2, GRID):
        flags = []
        for y in range(h):
            r = rows[y]
            a, b = px(r, x - 1), px(r, x)
            flags.append(step(a, b) >= t and step(px(r, x - 2), a) < SMOOTH and step(b, px(r, x + 1)) < SMOOTH)
        for start, ln in runs(flags, MINRUN):
            s = sum(step(px(rows[y], x - 1), px(rows[y], x)) for y in range(start, start + ln, 4)) / max(1, len(range(start, start + ln, 4)))
            res['vedges'].append({'x': x, 'y0': start, 'y1': start + ln, 'strength': round(s, 1)})
    # horizontal edges on the grid: row y (y % 32 == 0), step between row y-1 and y, smooth above and below
    for y in range(GRID, h - 2, GRID):
        up2, up, dn, dn2 = rows[y - 2], rows[y - 1], rows[y], rows[y + 1]
        flags = []
        for x in range(w):
            a, b = px(up, x), px(dn, x)
            flags.append(step(a, b) >= t and step(px(up2, x), a) < SMOOTH and step(b, px(dn2, x)) < SMOOTH)
        for start, ln in runs(flags, MINRUN):
            s = sum(step(px(up, x), px(dn, x)) for x in range(start, start + ln, 4)) / max(1, len(range(start, start + ln, 4)))
            res['hedges'].append({'y': y, 'x0': start, 'x1': start + ln, 'strength': round(s, 1)})
    # rectangles: a vertical and a horizontal aligned edge that meet (within 8 px)
    for v in res['vedges']:
        for hh in res['hedges']:
            if hh['x0'] - 8 <= v['x'] <= hh['x1'] + 8 and v['y0'] - 8 <= hh['y'] <= v['y1'] + 8:
                res['blocks'].append({'corner': [v['x'], hh['y']], 'v': [v['y0'], v['y1']], 'h': [hh['x0'], hh['x1']]})
    # full-width seams: rows where at least 55 % of the sampled columns jump by 8 or more against the row above while the rows around the seam do not
    xs = range(0, w, 4)
    jump = []
    for y in range(1, h):
        a, b = rows[y - 1], rows[y]
        n = 0
        for x in xs:
            i = 3 * x
            if abs(a[i] - b[i]) >= 8 or abs(a[i + 1] - b[i + 1]) >= 8 or abs(a[i + 2] - b[i + 2]) >= 8:
                n += 1
        jump.append(n / len(xs))
    for y in range(2, len(jump) - 2):
        if jump[y] >= 0.55 and jump[y - 1] < 0.25 and jump[y + 1] < 0.25:
            res['bands'].append({'y': y + 1, 'columns_jumping': round(jump[y], 2)})
    res['score'] = len(res['blocks']) * 10 + len(res['vedges']) + len(res['hedges'])   # bands are listed but not scored: the keyboard top, the dock and text rows are natural full-width edges
    return res


def main():
    args = sys.argv[1:]
    as_json = '--json' in args
    args = [a for a in args if a != '--json']
    t = T
    if '--t' in args:
        i = args.index('--t')
        t = int(args[i + 1])
        del args[i:i + 2]
    if not args:
        sys.exit(__doc__)
    results = [analyse(p, t) for p in args]
    if as_json:
        print(json.dumps(results, indent=1))
        return
    for r in results:
        print('%-26s score %3d | aligned edges v%d h%d | blocks %d | bands %d' % (r['image'].split('/')[-1], r['score'], len(r['vedges']), len(r['hedges']),
              len(r['blocks']), len(r['bands'])))
        for b in r['blocks'][:6]:
            print('    block corner x=%d y=%d  (vertical edge y %d..%d, horizontal edge x %d..%d)' % (b['corner'][0], b['corner'][1], b['v'][0], b['v'][1], b['h'][0], b['h'][1]))
        for v in r['vedges'][:6]:
            print('    vertical edge x=%d y %d..%d strength %.0f' % (v['x'], v['y0'], v['y1'], v['strength']))
        for e in r['hedges'][:6]:
            print('    horizontal edge y=%d x %d..%d strength %.0f' % (e['y'], e['x0'], e['x1'], e['strength']))
        for b in r['bands'][:6]:
            print('    band seam at y=%d (%.0f%% of columns jump)' % (b['y'], 100 * b['columns_jumping']))


main()
