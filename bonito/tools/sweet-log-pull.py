#!/usr/bin/env python3
"""Pull the iOS kernel's persistent console log (xnu patch 0047) from the phone through the log-reading recovery.

  sweet-log-pull.py [OUT.txt]       default logs/ios-console-YYYYmmdd-HHMMSS.txt

The log is the last 1 + 4 MiB/512 blocks of the userdata partition: block 0 header ("SWEETLOG", u32 version, u32 ring
size, u64 total bytes, u64 flush seq), then the ring; byte n of the console stream is at ring offset n % size.
Needs the phone in the recovery from mk-linux-recovery.py (USB 18d1:d0b1), see docs/RECOVERY.md.
"""
import os, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ums import UMS, gpt

def main():
    u = UMS()
    ud = [p for p in gpt(u) if p[0] == 'userdata'][0]
    blocks = 1 + (4 << 20) // 512
    base = ud[2] + 1 - blocks
    data = u.read(base, blocks)
    magic, version, size, total, seq = struct.unpack_from('<8sIIQQ', data, 0)
    if magic != b'SWEETLOG':
        sys.exit('no SWEETLOG header: the kernel never flushed a log (boot without sweetlog=0 and with ufsrw=1?)')
    ring = data[4096:4096 + size]
    text = ring[:total] if total <= size else ring[total % size:] + ring[:total % size]
    out = text.decode('utf-8', 'replace').replace('\r', '')
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'logs',
                                                              time.strftime('ios-console-%Y%m%d-%H%M%S.txt'))
    open(path, 'w').write(out)
    print(f'{path}: version {version}, ring {size}, {total} bytes total{" (wrapped, oldest lost)" if total > size else ""}, '
          f'flush #{seq}, {out.count(chr(10))} lines', file=sys.stderr)

main()
