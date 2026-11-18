#!/usr/bin/env python3
"""Read-only access to an iOS dyld shared cache (split subcache files) for symbolicating phone backtraces.

  dsc.py DIR image ADDR [ADDR ...]            which library an (unslid) address is in, with offset
  dsc.py DIR dis START END [--slide S]        disassemble [START, END) (unslid VM addresses; runtime = unslid + slide)
  dsc.py DIR str ADDR                         C string at ADDR
  dsc.py DIR ptr ADDR                         64-bit pointer at ADDR

Addresses printed by the phone's crash log are runtime addresses: pass --slide with the logged `slide` to unslide them
(`--slide 0x49b28000` subtracts it first). Needs aarch64-linux-gnu-objdump for `dis`.
"""
import glob, os, struct, subprocess, sys, tempfile

class DSC:
    def __init__(self, d):
        self.maps = []   # (addr, size, file offset, path)
        self.files = {}
        for p in sorted(glob.glob(os.path.join(d, 'dyld_shared_cache_arm64*'))):
            if p.endswith('.symbols'):
                continue
            f = open(p, 'rb'); self.files[p] = f
            h = f.read(0x40)
            if h[:15] != b'dyld_v1   arm64':
                continue
            moff, mcnt = struct.unpack('<II', h[0x10:0x18])
            f.seek(moff); raw = f.read(32 * mcnt)
            for i in range(mcnt):
                a, sz, fo, mx, ip = struct.unpack('<QQQII', raw[32 * i:32 * i + 32])
                self.maps.append((a, sz, fo, p))
        self.main = os.path.join(d, 'dyld_shared_cache_arm64')
        self._images = None

    def read(self, vm, n):
        out = b''
        while n > 0:
            for a, sz, fo, p in self.maps:
                if a <= vm < a + sz:
                    take = min(n, a + sz - vm)
                    f = self.files[p]; f.seek(fo + vm - a); out += f.read(take)
                    vm += take; n -= take
                    break
            else:
                raise KeyError(f'address 0x{vm:x} not mapped')
        return out

    def images(self):
        if self._images is None:
            f = self.files[self.main]
            f.seek(0); h = f.read(0x200)
            off, cnt = struct.unpack('<II', h[0x1c0:0x1c8])
            if not cnt:
                off, cnt = struct.unpack('<II', h[0x18:0x20])
            f.seek(off); raw = f.read(32 * cnt)
            imgs = []
            for i in range(cnt):
                a, mt, ino, po, _ = struct.unpack('<QQQII', raw[32 * i:32 * i + 32])
                f.seek(po); name = f.read(256).split(b'\0')[0].decode()
                imgs.append((a, name))
            self._images = sorted(imgs)
        return self._images

    def image_for(self, addr):
        best = None
        for a, n in self.images():
            if a <= addr: best = (a, n)
            else: break
        return best

    def cstr(self, addr, maxlen=200):
        b = self.read(addr, maxlen)
        return b.split(b'\0')[0].decode('latin1')

def main():
    d, cmd, *args = sys.argv[1:]
    slide = 0
    if '--slide' in args:
        i = args.index('--slide'); slide = int(args[i + 1], 0); del args[i:i + 2]
    c = DSC(d)
    if cmd == 'image':
        for a in args:
            addr = int(a, 0) - slide
            im = c.image_for(addr)
            print(f'0x{int(a, 0):x} -> unslid 0x{addr:x}  {im[1]} +0x{addr - im[0]:x}' if im else f'0x{addr:x} ?')
    elif cmd == 'dis':
        s, e = int(args[0], 0) - slide, int(args[1], 0) - slide
        data = c.read(s, e - s)
        with tempfile.NamedTemporaryFile(delete=False) as t:
            t.write(data); path = t.name
        subprocess.run(['aarch64-linux-gnu-objdump', '-D', '-b', 'binary', '-m', 'aarch64', f'--adjust-vma=0x{s:x}', path])
        os.unlink(path)
    elif cmd == 'str':
        print(c.cstr(int(args[0], 0) - slide))
    elif cmd == 'ptr':
        print(hex(struct.unpack('<Q', c.read(int(args[0], 0), 8))[0]))

if __name__ == '__main__':
    main()
