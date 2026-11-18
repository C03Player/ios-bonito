#!/usr/bin/env python3
"""Symbolicate runtime addresses from a phone crash log: sym.py SLIDE ADDR [ADDR...]  (needs /usb/work/dyld)."""
import sys, bisect
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from dsc import DSC
from objc import ObjC
slide = int(sys.argv[1], 0); c = DSC('/usb/work/dyld'); cache = {}
for a in sys.argv[2:]:
    v = int(a, 0) - slide
    im = c.image_for(v)
    if not im: print(a, '?'); continue
    name = im[1]; short = name.split('/')[-1]
    if name not in cache:
        try: cache[name] = ObjC(c, name).all_methods()
        except Exception: cache[name] = []
    ms = cache[name]; keys = [m[0] for m in ms]
    i = bisect.bisect_right(keys, v) - 1
    sym = f'{ms[i][1]} +0x{v - ms[i][0]:x}' if i >= 0 and v - ms[i][0] < 0x4000 else f'(no objc method) +0x{v - im[0]:x}'
    print(f'{a}  {short}  {sym}')
