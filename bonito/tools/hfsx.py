#!/usr/bin/env python3
"""Minimal READ-ONLY HFS+/HFSX reader (volume header, catalog and extents-overflow B-trees, data forks, hard links).

  hfsx.py IMAGE ls /path
  hfsx.py IMAGE cat /path [OUTFILE]          (whole file; large files stream in 8 MiB pieces)
  hfsx.py IMAGE stat /path

IMAGE is a raw volume file (e.g. /usb/build/ios16-root-20H392.img) or, with --phone, the iOS root on the phone's userdata
partition read through the log-reading recovery (bonito/tools/ums.py). Nothing is ever written.
"""
import os, struct, sys

class FileSource:
    def __init__(self, path):
        self.f = open(path, 'rb')
    def read(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

class PhoneSource:
    """The volume starts at the first block of the userdata partition."""
    def __init__(self):
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from ums import UMS, gpt
        self.u = UMS()
        self.base = [p for p in gpt(self.u) if p[0] == 'userdata'][0][1]
    def read(self, off, n):
        first = off // 512; last = (off + n + 511) // 512
        data = self.u.read(self.base + first, last - first)
        return data[off - first * 512:off - first * 512 + n]

class HFS:
    def __init__(self, src):
        self.src = src
        vh = src.read(1024, 512)
        if vh[:2] not in (b'H+', b'HX'):
            raise SystemExit(f'not an HFS+ volume (signature {vh[:2]!r})')
        self.case_sensitive = vh[:2] == b'HX'
        self.bs = struct.unpack('>I', vh[40:44])[0]
        self.catalog_fork = self._fork(vh[272:352])
        self.extents_fork = self._fork(vh[192:272])
        self.catalog_data = None
        self._index = None
        self._ovf = None

    @staticmethod
    def _fork(b):
        size = struct.unpack('>Q', b[:8])[0]
        ext = [struct.unpack('>II', b[16 + 8 * i:24 + 8 * i]) for i in range(8)]
        return size, [e for e in ext if e[1]]

    def _fork_bytes(self, fork, off, n):
        size, ext = fork
        n = min(n, size - off)
        out = b''
        pos = 0
        for start, cnt in ext:
            ln = cnt * self.bs
            if off < pos + ln and n > 0:
                o = off - pos if off > pos else 0
                take = min(ln - o, n)
                out += self.src.read(start * self.bs + o, take)
                off += take; n -= take
            pos += ln
        return out

    # --- B-trees
    def _read_tree(self, fork):
        hdr = self._fork_bytes(fork, 0, 512)
        node_size = struct.unpack('>H', hdr[32:34])[0]
        first_leaf = struct.unpack('>I', hdr[24:28])[0]
        return node_size, first_leaf

    def _leaf_records(self, fork):
        node_size, node = self._read_tree(fork)
        while node:
            data = self._fork_bytes(fork, node * node_size, node_size)
            flink = struct.unpack('>I', data[:4])[0]
            n = struct.unpack('>H', data[10:12])[0]
            offs = [struct.unpack('>H', data[node_size - 2 * (i + 1):node_size - 2 * i])[0] for i in range(n + 1)]
            for i in range(n):
                yield data[offs[i]:offs[i + 1]]
            node = flink

    def _build_index(self):
        if self._index is not None:
            return
        idx = {}
        for rec in self._leaf_records(self.catalog_fork):
            klen = struct.unpack('>H', rec[:2])[0]
            parent = struct.unpack('>I', rec[2:6])[0]
            nlen = struct.unpack('>H', rec[6:8])[0]
            name = rec[8:8 + 2 * nlen].decode('utf-16-be', 'replace')
            start = 2 + klen
            start += start & 1
            body = rec[start:]
            if len(body) < 2:
                continue
            kind = struct.unpack('>h', body[:2])[0]
            if kind in (1, 2):
                idx[(parent, name)] = (kind, body)
        self._index = idx

    def _overflow(self, fid, fork_type=0):
        if self._ovf is None:
            self._ovf = {}
            for rec in self._leaf_records(self.extents_fork):
                ft, _, f_id, start = struct.unpack('>BBII', rec[2:12])
                start = 2 + struct.unpack('>H', rec[:2])[0]
                body = rec[start:start + 64]
                ext = [struct.unpack('>II', body[8 * i:8 * i + 8]) for i in range(8)]
                self._ovf.setdefault((f_id, ft), []).append((start, [e for e in ext if e[1]]))
        return sorted(self._ovf.get((fid, fork_type), []))

    def _all_extents(self, fork_bytes, fid):
        size, ext = self._fork(fork_bytes)
        total = sum(c for _, c in ext)
        ext = list(ext)
        for start, more in self._overflow(fid):
            ext += more
        return size, ext

    # --- lookups
    def lookup(self, path):
        self._build_index()
        cur = 2
        parts = [p for p in path.split('/') if p]
        rec = None
        for i, p in enumerate(parts):
            rec = self._index.get((cur, p))
            if rec is None and not self.case_sensitive:
                low = p.lower()
                rec = next((v for (par, n), v in self._index.items() if par == cur and n.lower() == low), None)
            if rec is None:
                raise FileNotFoundError('/' + '/'.join(parts[:i + 1]))
            kind, body = rec
            if kind == 1:
                cur = struct.unpack('>I', body[8:12])[0]
            elif i != len(parts) - 1:
                raise NotADirectoryError(p)
        if rec is None:
            return (1, 2, None)
        kind, body = rec
        if kind == 1:
            return (1, struct.unpack('>I', body[8:12])[0], None)
        fid = struct.unpack('>I', body[8:12])[0]
        # hard link? bsdInfo.special = iNodeNum, fileType 'hlnk' creator 'hfs+'
        ftype = body[48:52]
        if ftype == b'hlnk':
            inode = struct.unpack('>I', body[44:48])[0]
            priv = next((v for (par, n), v in self._index.items() if par == 2 and n.startswith('\0\0\0\0HFS+ Private')), None)
            pdir = struct.unpack('>I', priv[1][8:12])[0]
            body = self._index[(pdir, 'iNode%d' % inode)][1]
            fid = struct.unpack('>I', body[8:12])[0]
        return (2, fid, body)

    def listdir(self, path):
        kind, fid, _ = self.lookup(path)
        self._build_index()
        return sorted(n for (par, n) in self._index if par == fid)

    def file_size(self, path):
        kind, fid, body = self.lookup(path)
        return struct.unpack('>Q', body[88:96])[0]

    def read_file(self, path, off=0, n=None):
        kind, fid, body = self.lookup(path)
        if kind != 2:
            raise IsADirectoryError(path)
        fork = self._all_extents(body[88:168], fid)
        size = fork[0]
        if n is None or off + n > size:
            n = size - off
        return self._fork_bytes(fork, off, n)

if __name__ == '__main__':
    args = sys.argv[1:]
    phone = '--phone' in args
    if phone: args.remove('--phone')
    src = PhoneSource() if phone else FileSource(args.pop(0))
    h = HFS(src)
    cmd, path = args[0], args[1]
    if cmd == 'ls':
        print('\n'.join(h.listdir(path)))
    elif cmd == 'stat':
        kind, fid, body = h.lookup(path)
        print('dir' if kind == 1 else 'file', 'id', fid, 'size', h.file_size(path) if kind == 2 else '-')
    elif cmd == 'cat':
        if len(args) > 2:
            size = h.file_size(path)
            with open(args[2], 'wb') as out:
                off = 0
                while off < size:
                    chunk = h.read_file(path, off, 8 << 20)
                    out.write(chunk); off += len(chunk)
            print(f'{args[2]}: {size} bytes')
        else:
            sys.stdout.buffer.write(h.read_file(path))
