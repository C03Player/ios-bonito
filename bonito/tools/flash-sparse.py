#!/usr/bin/env python3
"""Flash a large raw image to a fastboot partition WITHOUT libsparse (Debian's fastboot 29 / img2simg silently sent 0 KB here).

  flash-sparse.py IMAGE PARTITION [WINDOW_MIB]

The image is read in WINDOW_MIB windows (default 128, below the bootloader's 256 MiB download limit). Each window becomes one Android
sparse file: DONT_CARE for everything before it, RAW chunks for non-zero runs, FILL(0) chunks for zero runs (so the partition ends up
bit-identical to the image, zeros included), DONT_CARE after it. Each is flashed with `fastboot flash PARTITION file`. Stops at the first
error. Run only when `fastboot getvar product` says bonito and the bootloader is unlocked.
"""
import os, struct, subprocess, sys, time

BLK = 4096
MAGIC, RAW, FILL, DONT_CARE = 0xED26FF3A, 0xCAC1, 0xCAC2, 0xCAC3


def chunk_hdr(kind, blocks, payload):
    return struct.pack('<HHII', kind, 0, blocks, 12 + payload)


def build(total_blks, start, data, tmp):
    """Write a sparse file covering blocks [start, start+len(data)/BLK) of total_blks; data is the window's bytes."""
    n = len(data) // BLK
    chunks = []
    if start:
        chunks.append(chunk_hdr(DONT_CARE, start, 0))
    zero = bytes(BLK)
    i = 0
    mv = memoryview(data)
    while i < n:
        j = i
        z = mv[i * BLK:(i + 1) * BLK] == zero
        while j < n and (mv[j * BLK:(j + 1) * BLK] == zero) == z:
            j += 1
        if z:
            chunks.append(chunk_hdr(FILL, j - i, 4) + b'\0\0\0\0')
        else:
            chunks.append(chunk_hdr(RAW, j - i, (j - i) * BLK) + bytes(mv[i * BLK:j * BLK]))
        i = j
    tail = total_blks - start - n
    if tail:
        chunks.append(chunk_hdr(DONT_CARE, tail, 0))
    hdr = struct.pack('<IHHHHIIII', MAGIC, 1, 0, 28, 12, BLK, total_blks, len(chunks), 0)
    with open(tmp, 'wb') as f:
        f.write(hdr)
        for c in chunks:
            f.write(c)


def main():
    img, part = sys.argv[1], sys.argv[2]
    win = int(sys.argv[3]) if len(sys.argv) > 3 else 128
    size = os.path.getsize(img)
    assert size % BLK == 0, 'image size is not a multiple of 4096'
    total_blks = size // BLK
    wbytes = win * 1024 * 1024
    tmp = os.path.join(os.path.dirname(os.path.abspath(img)) if os.access(os.path.dirname(os.path.abspath(img)), os.W_OK) else '/tmp', '.flash-window.simg')
    t0 = time.time()
    sent = 0
    with open(img, 'rb') as f:
        off = 0
        while off < size:
            data = f.read(min(wbytes, size - off))
            build(total_blks, off // BLK, data, tmp)
            r = subprocess.run(['fastboot', 'flash', part, tmp], capture_output=True, text=True)
            out = r.stdout + r.stderr
            if r.returncode != 0 or 'FAILED' in out or 'OKAY' not in out:
                print('FASTBOOT ERROR at offset %d MiB:\n%s' % (off >> 20, out), flush=True)
                sys.exit(1)
            if 'Sending sparse' not in out and 'Sending' not in out:
                print('unexpected output at %d MiB:\n%s' % (off >> 20, out), flush=True)
                sys.exit(1)
            if off < 3 * wbytes or off >= size - wbytes:
                print('   fastboot says:', ' | '.join(l.strip() for l in out.strip().splitlines() if 'Sending' in l or 'Writing' in l), flush=True)
            off += len(data)
            sent += os.path.getsize(tmp)
            print('%5.1f%%  %6d MiB of %d MiB  (sparse bytes sent %.1f GiB, %.0f s)' % (100.0 * off / size, off >> 20, size >> 20, sent / 2**30, time.time() - t0), flush=True)
    os.unlink(tmp)
    print('DONE: %d MiB written to %s in %.0f s' % (size >> 20, part, time.time() - t0), flush=True)


main()
