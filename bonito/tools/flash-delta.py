#!/usr/bin/env python3
"""Flash only the byte ranges a hfsx_write.py run touched (its --touched log) to the phone's partition: seconds instead of an hour.

  flash-delta.py IMAGE PARTITION TOUCHED_FILE [--dry-run]

ONLY valid while the phone's partition is still bit-identical to IMAGE as it was BEFORE those edits (right after a full flash-sparse.py of that
image, before iOS has booted from it even once: iOS rewrites metadata at every boot). The ranges are rounded to 4 KiB blocks, merged, and sent
as Android sparse files (DONT_CARE everywhere else, at most 64 MiB of data each) with `fastboot flash`.
"""
import os, struct, subprocess, sys

BLK = 4096
MAGIC, RAW, DONT_CARE = 0xED26FF3A, 0xCAC1, 0xCAC3


def chunk_hdr(kind, blocks, payload):
    return struct.pack('<HHII', kind, 0, blocks, 12 + payload)


def ranges(touched):
    blocks = set()
    with open(touched) as f:
        for line in f:
            off, n = map(int, line.split())
            for b in range(off // BLK, (off + n - 1) // BLK + 1):
                blocks.add(b)
    runs = []
    for b in sorted(blocks):
        if runs and runs[-1][0] + runs[-1][1] == b:
            runs[-1][1] += 1
        else:
            runs.append([b, 1])
    return runs


def sparse(total_blks, img, runs, tmp):
    chunks, pos = [], 0
    with open(img, 'rb') as f:
        for start, cnt in runs:
            if start > pos:
                chunks.append(chunk_hdr(DONT_CARE, start - pos, 0))
            f.seek(start * BLK)
            chunks.append(chunk_hdr(RAW, cnt, cnt * BLK) + f.read(cnt * BLK))
            pos = start + cnt
    if total_blks > pos:
        chunks.append(chunk_hdr(DONT_CARE, total_blks - pos, 0))
    hdr = struct.pack('<IHHHHIIII', MAGIC, 1, 0, 28, 12, BLK, total_blks, len(chunks), 0)
    with open(tmp, 'wb') as f:
        f.write(hdr)
        for c in chunks:
            f.write(c)


def main():
    a = [x for x in sys.argv[1:] if x != '--dry-run']
    dry = '--dry-run' in sys.argv
    img, part, touched = a
    size = os.path.getsize(img)
    total = size // BLK
    runs = ranges(touched)
    nblk = sum(c for _, c in runs)
    print(f'{len(runs)} runs, {nblk} blocks ({nblk * BLK / 2**20:.1f} MiB) of {size >> 20} MiB')
    if dry:
        return
    tmp = os.path.join(os.path.dirname(os.path.abspath(img)), '.flash-delta.simg')
    groups, cur, cnt = [], [], 0
    for r in runs:                      # split runs so each sparse file carries at most 64 MiB
        s, c = r
        while c:
            take = min(c, 16384 - cnt)
            cur.append([s, take]); s += take; c -= take; cnt += take
            if cnt >= 16384:
                groups.append(cur); cur, cnt = [], 0
    if cur:
        groups.append(cur)
    for g in groups:
        sparse(total, img, g, tmp)
        r = subprocess.run(['fastboot', 'flash', part, tmp], capture_output=True, text=True)
        out = r.stdout + r.stderr
        if r.returncode != 0 or 'FAILED' in out or 'OKAY' not in out:
            print('FASTBOOT ERROR:\n' + out)
            sys.exit(1)
        print('  sent', ' | '.join(l.strip() for l in out.strip().splitlines() if 'Sending' in l or 'Writing' in l))
    os.unlink(tmp)
    print('DONE')


main()
