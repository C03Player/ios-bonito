#!/usr/bin/env python3
"""Objective-C metadata reader for a dyld shared cache image (arm64, small relative method lists).

  objc.py DIR IMAGE-SUBSTRING find CLASS-SUBSTRING          list matching classes and their methods (+IMP)
  objc.py DIR IMAGE-SUBSTRING imp ADDR                       which method contains (unslid) ADDR
"""
import struct, sys
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from dsc import DSC

M40 = 0x000000FFFFFFFFFF
def macho_secs(c, addr):
    h = c.read(addr, 32); ncmds, sz = struct.unpack('<II', h[16:24])
    cmds = c.read(addr + 32, sz); off = 0; secs = {}
    for _ in range(ncmds):
        cmd, cl = struct.unpack('<II', cmds[off:off + 8])
        if cmd == 0x19:
            ns = struct.unpack('<I', cmds[off + 64:off + 68])[0]
            for i in range(ns):
                s = cmds[off + 72 + 80 * i:off + 72 + 80 * (i + 1)]
                secs[s[:16].split(b'\0')[0].decode()] = struct.unpack('<QQ', s[32:48])
        off += cl
    return secs

class ObjC:
    def __init__(self, c, imgname):
        self.c = c
        self.addr = [a for a, n in c.images() if imgname in n][0]
        self.secs = macho_secs(c, self.addr)
        lo = [a for a, n in c.images() if n.endswith('libobjc.A.dylib')][0]
        opt = macho_secs(c, lo)['__objc_opt_ro'][0]
        # direct selectors: offsets are relative to (objc_opt_ro header address + relativeMethodSelectorBaseAddressOffset)
        self.selbase = opt + struct.unpack('<Q', c.read(opt + 40, 8))[0]
        self._methods = None

    def p(self, a):
        return struct.unpack('<Q', self.c.read(a, 8))[0] & M40
    def u32(self, a):
        return struct.unpack('<I', self.c.read(a, 4))[0]
    def i32(self, a):
        return struct.unpack('<i', self.c.read(a, 4))[0]

    def classes(self):
        a, sz = self.secs['__objc_classlist']
        for i in range(sz // 8):
            yield self.p(a + 8 * i)

    def class_info(self, cls):
        bits = self.p(cls + 32) & 0x00007ffffffffff8
        name = self.c.cstr(self.p(bits + 24), 100)
        return name, bits

    def method_list(self, ml, meta=False):
        out = []
        if not ml: return out
        ml &= ~3
        flags, count = struct.unpack('<II', self.c.read(ml, 8))
        small = flags & 0x80000000
        for i in range(count):
            e = ml + 8 + i * (12 if small else 24)
            if small:
                noff, toff, ioff = struct.unpack('<iii', self.c.read(e, 12))
                imp = e + 8 + ioff
                nm = self.sel_small(e, noff)
            else:
                n, t, ip = struct.unpack('<QQQ', self.c.read(e, 24)); imp = ip & M40; nm = self.c.cstr(n & M40, 100)
            out.append((nm, imp))
        return out

    def sel_small(self, e, noff):
        # small method: name field = offset (from the field itself) to a selector-reference slot holding the string pointer
        def ok(t): return t and all(32 <= ord(ch) < 127 for ch in t) and len(t) < 90
        try:
            t = self.c.cstr(self.p(e + noff), 100)
            if ok(t): return t
        except Exception:
            pass
        try:   # direct selector: offset from the selector base in libobjc
            t = self.c.cstr(self.selbase + noff, 100)
            if ok(t): return t
        except Exception:
            pass
        return '?'

    def all_methods(self):
        if self._methods is None:
            ms = []
            for cls in self.classes():
                name, ro = self.class_info(cls)
                for plus, k in ((False, cls), (True, self.p(cls) )):   # instance methods: cls ro; class methods: metaclass ro
                    try:
                        r = ro if not plus else (self.p(k + 32) & 0x00007ffffffffff8)
                        for nm, imp in self.method_list(self.p(r + 32)):
                            ms.append((imp, ('+' if plus else '-') + '[' + name + ' ' + nm + ']'))
                    except Exception:
                        pass
            self._methods = sorted(ms)
        return self._methods

if __name__ == '__main__':
    d, img, cmd, *a = sys.argv[1:]
    o = ObjC(DSC(d), img)
    if cmd == 'find':
        for cls in o.classes():
            name, ro = o.class_info(cls)
            if a[0] in name:
                print('class', name)
                for nm, imp in o.method_list(o.p(ro + 32)):
                    print('   -', nm, hex(imp))
    elif cmd == 'imp':
        import bisect
        ms = o.all_methods(); keys = [m[0] for m in ms]
        for x in a:
            v = int(x, 0); i = bisect.bisect_right(keys, v) - 1
            print(hex(v), ms[i][1], '+0x%x' % (v - ms[i][0]))
