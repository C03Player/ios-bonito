#!/usr/bin/env python3
"""Show the panic of the NEWEST boot in an iOS console log with its backtrace named from the kernel image's symbol table.

  ksym-panic.py logs/NAME.txt KERNEL_IMAGE      (KERNEL_IMAGE = work/ci-RUN/kernel.development.sweet; needs llvm-nm)

The log file starts with the newest boot and continues with an older boot's tail (timestamps jump back): only the first part is used.
Frames are unslid kernel addresses (the shim loads the kernel at its link address: "Kernel text base 0xfffffe0007004000").
"""
import bisect, re, subprocess, sys

log, kern = sys.argv[1], sys.argv[2]
lines, prev = [], None
for l in open(log, errors='replace').read().split('\n'):
    m = re.match(r'\[\s*([0-9.]+)\]', l)
    if m:
        t = float(m.group(1))
        if prev is not None and t < prev - 5:
            break
        prev = t
    lines.append(l)
nm = subprocess.run(['llvm-nm', '-n', kern], capture_output=True, text=True).stdout
syms = sorted((int(p[0], 16), p[2]) for p in (l.split() for l in nm.split('\n')) if len(p) >= 3 and p[1] in 'tTsSwW')
addrs = [a for a, _ in syms]


def name(a):
    i = bisect.bisect_right(addrs, a) - 1
    return '%s+0x%x' % (syms[i][1], a - syms[i][0]) if i >= 0 else '?'


panic = [i for i, l in enumerate(lines) if re.search(r'SWEET PANIC|panic\(cpu', l)]
print('newest boot: %d lines, last timestamp %s' % (len(lines), prev))
if not panic:
    print('no panic in the newest boot; last lines:')
    print('\n'.join(l[:160] for l in lines[-6:]))
    sys.exit(0)
i = panic[0]
print('before the panic:')
for l in lines[max(0, i - 6):i]:
    print('   ', l[:170])
print('PANIC:', lines[i][:200])
for l in lines[i:i + 90]:
    m = re.search(r'Panicked (task|thread).*', l)
    if m:
        print('   ', l[:170])
    m = re.search(r'lr: (0x[0-9a-f]+)', l)
    if m:
        print('    ', m.group(1), name(int(m.group(1), 16)))
