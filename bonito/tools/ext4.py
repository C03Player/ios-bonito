#!/usr/bin/env python3
"""Minimal READ-ONLY ext4 reader (extents, linear and hashed directories, symlinks) for Android partition images.

  ext4.py [--base BYTES] IMAGE ls /path     (--base: the filesystem starts BYTES into the image)
  ext4.py IMAGE cat /path [OUTFILE]
  ext4.py IMAGE find NAMEPART            every path whose last component contains NAMEPART (case-insensitive)

IMAGE is a raw partition dump (for example the phone's vendor_a read through the log-reading recovery). Nothing is
ever written. Used to take firmware inputs from the phone's own partitions; those files stay local, out of Git.
"""
import os, struct, sys

class _Off:
    """File wrapper that adds a fixed base offset (for a filesystem that starts inside a larger dump)."""
    def __init__(self, f, base):
        self.f, self.base = f, base
    def seek(self, n):
        self.f.seek(self.base + n)
    def read(self, n):
        return self.f.read(n)

class Ext4:
    def __init__(self, path, base=0):
        self.f = _Off(open(path, 'rb'), base)
        self.f.seek(1024)
        sb = self.f.read(1024)
        if struct.unpack_from('<H', sb, 56)[0] != 0xEF53:
            raise SystemExit('not an ext4 image')
        self.bs = 1024 << struct.unpack_from('<I', sb, 24)[0]
        self.ipg = struct.unpack_from('<I', sb, 40)[0]
        self.first_data = struct.unpack_from('<I', sb, 20)[0]
        self.inode_size = struct.unpack_from('<H', sb, 88)[0] or 128
        incompat = struct.unpack_from('<I', sb, 0x60)[0]
        self.desc_size = struct.unpack_from('<H', sb, 0xFE)[0] if incompat & 0x80 else 32
        self.desc_size = self.desc_size or 32
        self.gdt_block = self.first_data + 1
        self._gd = {}

    def blk(self, n, count=1):
        self.f.seek(n * self.bs)
        return self.f.read(self.bs * count)

    def _group_inode_table(self, g):
        if g not in self._gd:
            off = self.gdt_block * self.bs + g * self.desc_size
            self.f.seek(off)
            d = self.f.read(self.desc_size)
            lo = struct.unpack_from('<I', d, 8)[0]
            hi = struct.unpack_from('<I', d, 0x28)[0] if self.desc_size >= 64 else 0
            self._gd[g] = lo | (hi << 32)
        return self._gd[g]

    def inode(self, n):
        g, idx = divmod(n - 1, self.ipg)
        off = self._group_inode_table(g) * self.bs + idx * self.inode_size
        self.f.seek(off)
        raw = self.f.read(self.inode_size)
        mode, = struct.unpack_from('<H', raw, 0)
        size_lo, = struct.unpack_from('<I', raw, 4)
        flags, = struct.unpack_from('<I', raw, 0x20)
        size_hi, = struct.unpack_from('<I', raw, 0x6C)
        return {'mode': mode, 'size': size_lo | (size_hi << 32), 'flags': flags, 'iblock': raw[0x28:0x28 + 60]}

    def _extents(self, node):
        magic, entries, _, depth = struct.unpack_from('<HHHH', node, 0)
        if magic != 0xF30A:
            raise ValueError('bad extent header')
        out = []
        for i in range(entries):
            e = node[12 + 12 * i:24 + 12 * i]
            if depth == 0:
                lblk, ln, hi, lo = struct.unpack('<IHHI', e)
                unwritten = ln > 32768
                ln = ln - 32768 if unwritten else ln
                out.append((lblk, ln, lo | (hi << 32), unwritten))
            else:
                lblk, lo, hi = struct.unpack_from('<IIH', e, 0)
                out += self._extents(self.blk(lo | (hi << 32)))
        return out

    def read(self, ino, off=0, n=None):
        i = self.inode(ino) if isinstance(ino, int) else ino
        size = i['size']
        if n is None or off + n > size:
            n = size - off
        if i['flags'] & 0x10000000:      # inline data
            return i['iblock'][off:off + n]
        if not (i['flags'] & 0x80000):
            raise NotImplementedError('block-mapped inode')
        ext = self._extents(i['iblock'])
        out = bytearray(n)
        for lblk, ln, pblk, unwritten in ext:
            s, e = lblk * self.bs, (lblk + ln) * self.bs
            lo, hi = max(s, off), min(e, off + n)
            if lo >= hi or unwritten:
                continue
            self.f.seek(pblk * self.bs + (lo - s))
            out[lo - off:hi - off] = self.f.read(hi - lo)
        return bytes(out)

    def listdir(self, ino):
        data = self.read(ino)
        out = {}
        for b in range(0, len(data), self.bs):
            blk = data[b:b + self.bs]
            p = 0
            while p + 8 <= len(blk):
                inum, rec, nl, ft = struct.unpack_from('<IHBB', blk, p)
                if rec < 8 or p + rec > len(blk):
                    break
                if inum and nl:
                    out[blk[p + 8:p + 8 + nl].decode('utf8', 'replace')] = (inum, ft)
                p += rec
        return out

    def lookup(self, path, follow=True, depth=0):
        cur = 2
        parts = [x for x in path.split('/') if x]
        for k, name in enumerate(parts):
            ent = self.listdir(cur).get(name)
            if ent is None:
                raise FileNotFoundError(path)
            cur = ent[0]
            i = self.inode(cur)
            if (i['mode'] & 0xF000) == 0xA000 and depth < 8:
                tgt = self.read(cur).decode()
                rest = '/'.join(parts[k + 1:])
                base = '/' if tgt.startswith('/') else '/' + '/'.join(parts[:k]) + '/'
                return self.lookup(os.path.normpath(base + tgt) + ('/' + rest if rest else ''), follow, depth + 1)
        return cur

    def walk(self, ino=2, prefix=''):
        for name, (inum, ft) in sorted(self.listdir(ino).items()):
            if name in ('.', '..'):
                continue
            yield prefix + '/' + name, inum, ft
            if ft == 2:
                yield from self.walk(inum, prefix + '/' + name)

if __name__ == '__main__':
    base = 0
    if len(sys.argv) > 2 and sys.argv[1] == '--base':      # ext4.py --base BYTES IMAGE cmd ...
        base = int(sys.argv[2], 0)
        del sys.argv[1:3]
    e = Ext4(sys.argv[1], base)
    cmd = sys.argv[2]
    if cmd == 'ls':
        for n, (i, ft) in sorted(e.listdir(e.lookup(sys.argv[3])).items()):
            print(('d ' if ft == 2 else 'l ' if ft == 7 else '- ') + n)
    elif cmd == 'cat':
        data = e.read(e.lookup(sys.argv[3]))
        if len(sys.argv) > 4:
            open(sys.argv[4], 'wb').write(data)
            print(f'{sys.argv[4]}: {len(data)} bytes')
        else:
            sys.stdout.buffer.write(data)
    elif cmd == 'find':
        pat = sys.argv[3].lower()
        for p, i, ft in e.walk():
            if pat in p.rsplit('/', 1)[-1].lower():
                print(p)
