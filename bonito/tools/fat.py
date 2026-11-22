#!/usr/bin/env python3
"""Minimal read-only FAT12/16/32 reader for partition images (the phone's modem partition: FAT16 with 4096-byte sectors, long names).

  fat.py IMAGE ls /image
  fat.py IMAGE cat /image/mba.mbn OUTFILE
  fat.py IMAGE extract /image DIR          copy every file of a directory (not recursive) into DIR

Pure Python, no mounting (the container has no loop devices). The modem and its Wi-Fi firmware are the user's own inputs: keep them out of Git.
"""
import os, struct, sys


class Fat:
    def __init__(self, path):
        self.f = open(path, 'rb')
        b = self.f.read(512)
        self.bps, self.spc, self.rsv, self.nfat = struct.unpack_from('<HBHB', b, 11)
        self.root_entries, self.tot16 = struct.unpack_from('<HH', b, 17)
        self.spf16 = struct.unpack_from('<H', b, 22)[0]
        self.tot32 = struct.unpack_from('<I', b, 32)[0]
        self.spf32 = struct.unpack_from('<I', b, 36)[0]
        self.root32 = struct.unpack_from('<I', b, 44)[0]
        self.spf = self.spf16 or self.spf32
        total = self.tot16 or self.tot32
        self.root_sectors = (self.root_entries * 32 + self.bps - 1) // self.bps
        self.data_start = self.rsv + self.nfat * self.spf + self.root_sectors
        clusters = (total - self.data_start) // self.spc
        self.kind = 12 if clusters < 4085 else 16 if clusters < 65525 else 32
        self.csize = self.spc * self.bps
        self.fat_off = self.rsv * self.bps
        self.f.seek(self.fat_off)
        self.fat = self.f.read(self.spf * self.bps)

    def next(self, c):
        if self.kind == 32:
            v = struct.unpack_from('<I', self.fat, c * 4)[0] & 0x0fffffff
            return None if v >= 0x0ffffff8 or v < 2 else v
        if self.kind == 16:
            v = struct.unpack_from('<H', self.fat, c * 2)[0]
            return None if v >= 0xfff8 or v < 2 else v
        o = c + c // 2
        v = struct.unpack_from('<H', self.fat, o)[0]
        v = v >> 4 if c & 1 else v & 0xfff
        return None if v >= 0xff8 or v < 2 else v

    def chain(self, c):
        while c:
            yield c
            c = self.next(c)

    def cluster(self, c):
        self.f.seek((self.data_start + (c - 2) * self.spc) * self.bps)
        return self.f.read(self.csize)

    def read_chain(self, c, size=None):
        out = b''.join(self.cluster(x) for x in self.chain(c))
        return out if size is None else out[:size]

    def dir_bytes(self, c):
        if c == 0 and self.kind != 32:
            self.f.seek((self.rsv + self.nfat * self.spf) * self.bps)
            return self.f.read(self.root_sectors * self.bps)
        return self.read_chain(c or self.root32)

    def entries(self, c):
        data = self.dir_bytes(c)
        lfn = []
        for o in range(0, len(data), 32):
            e = data[o:o + 32]
            if e[0] == 0:
                break
            if e[0] == 0xe5:
                lfn = []
                continue
            if e[11] == 0x0f:
                part = (e[1:11] + e[14:26] + e[28:32]).decode('utf-16le', 'ignore')
                lfn.insert(0, part.split('\x00')[0].split('￿')[0])
                continue
            name = ''.join(lfn) if lfn else (e[0:8].decode('latin1').rstrip() + ('.' + e[8:11].decode('latin1').rstrip() if e[8:11].strip() else ''))
            lfn = []
            if e[11] & 0x08:
                continue
            hi = struct.unpack_from('<H', e, 20)[0] if self.kind == 32 else 0
            first = struct.unpack_from('<H', e, 26)[0] | (hi << 16)
            size = struct.unpack_from('<I', e, 28)[0]
            yield name, bool(e[11] & 0x10), first, size

    def lookup(self, path):
        cur = 0
        isdir, size = True, 0
        for part in [p for p in path.split('/') if p]:
            for name, d, first, sz in self.entries(cur):
                if name.lower() == part.lower():
                    cur, isdir, size = first, d, sz
                    break
            else:
                raise FileNotFoundError(path)
        return cur, isdir, size


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    fat = Fat(sys.argv[1])
    cmd, path = sys.argv[2], sys.argv[3]
    cur, isdir, size = fat.lookup(path)
    if cmd == 'ls':
        for name, d, first, sz in fat.entries(cur):
            if name not in ('.', '..'):
                print(('d ' if d else '- ') + '%9d %s' % (sz, name))
    elif cmd == 'cat':
        data = fat.read_chain(cur, size)
        if len(sys.argv) > 4:
            open(sys.argv[4], 'wb').write(data)
            print('%s: %d bytes' % (sys.argv[4], len(data)))
        else:
            sys.stdout.buffer.write(data)
    elif cmd == 'extract':
        os.makedirs(sys.argv[4], exist_ok=True)
        for name, d, first, sz in fat.entries(cur):
            if not d and name not in ('.', '..'):
                open(os.path.join(sys.argv[4], name), 'wb').write(fat.read_chain(first, sz))
                print('%s %d' % (name, sz))
    else:
        sys.exit(__doc__)


main()
