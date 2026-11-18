#!/usr/bin/env python3
"""Pull the latest framebuffer snapshot the iOS kernel saved (xnu patch 0140, boot-arg sweetshot=SECS) and write a PNG.

  sweet-shot-pull.py [OUT.png]       default logs/ios-shot-YYYYmmdd-HHMMSS.png  (both slots are read, the newer one wins)

Slots sit just below the console log area at the end of userdata (see sweet-log-pull.py). Pixels are BGRA bytes.
"""
import os, struct, sys, time, zlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ums import UMS, gpt

LOG_BLOCKS = 1 + (4 << 20) // 512
SHOT_BLOCKS = 1 + (10 << 20) // 512

def png(path, w, h, rgb):
    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    raw = b''.join(b'\0' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))

def main():
    u = UMS()
    ud = [p for p in gpt(u) if p[0] == 'userdata'][0]
    logbase = ud[2] + 1 - LOG_BLOCKS
    best = None
    for i in (0, 1):
        slot = logbase - 8 - (1 + i) * SHOT_BLOCKS
        h = u.read(slot, 1)
        magic, ver, w, hh, rb, seq, secs = struct.unpack_from('<8sIIIIQQ', h, 0)
        if magic == b'SWEETSHT' and (best is None or seq > best[0]):
            best = (seq, slot, w, hh, rb)
    if best is None:
        sys.exit('no SWEETSHT header: no snapshot was saved (boot with sweetshot=SECS and a kernel with patch 0140)')
    seq, slot, w, hh, rb = best
    data = u.read(slot + 1, (rb * hh + 511) // 512)[:rb * hh]
    rgb = bytearray(w * hh * 3)
    for y in range(hh):
        row = data[y * rb:y * rb + w * 4]
        rgb[y * w * 3 + 0:(y + 1) * w * 3:3] = row[2::4]      # R  (bytes are B,G,R,A)
        rgb[y * w * 3 + 1:(y + 1) * w * 3:3] = row[1::4]      # G
        rgb[y * w * 3 + 2:(y + 1) * w * 3:3] = row[0::4]      # B
    alpha = data[3::rb][:0]
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'logs',
                                                              time.strftime('ios-shot-%Y%m%d-%H%M%S.png'))
    png(out, w, hh, bytes(rgb))
    a = bytes(data[3 + y * rb + x * 4] for y in range(0, hh, 97) for x in range(0, w, 61))
    print(f'{out}: snapshot #{seq}, {w}x{hh}, rowbytes {rb}; alpha byte min {min(a)} max {max(a)} (sampled)', file=sys.stderr)

main()
