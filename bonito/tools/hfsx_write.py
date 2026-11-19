#!/usr/bin/env python3
"""Minimal HFSX writer: add folders and files to a case-sensitive HFS+ volume image WITHOUT mounting it (the dev container has no loop devices).

  hfsx_write.py IMAGE [--undo FILE] mkdir /abs/dir [MODE]
  hfsx_write.py IMAGE [--undo FILE] put LOCAL_FILE /abs/path [MODE]
  hfsx_write.py IMAGE [--undo FILE] replace LOCAL_FILE /abs/path         (new contents for an existing file)
  hfsx_write.py IMAGE [--undo FILE] install-tree LOCAL_DIR /abs/dir      (copies the tree; executable files get 0755, others 0644, dirs 0755)
  hfsx_write.py IMAGE undo UNDO_FILE                                      (put every region back that an earlier run overwrote)

What it does, the way Apple's HFS+ does it: new catalog records (folder or file record plus its thread record) are inserted into the catalog
B-tree (leaf and index node splits, new root, node allocation from the header's map record), file data goes into free allocation blocks found in the
allocation bitmap, and the volume header (counts, free blocks, next CNID, write count) and its backup at the end of the volume are updated. Owner
root:wheel. Only for an image that is NOT mounted and has an empty journal (the build script's images: journal info flags 'needs initialization').
--touched FILE appends "offset length" for every byte range written (flash-delta.py sends exactly those to the phone).
Every region that is overwritten is first saved to the undo file (512-byte granules), file data blocks excepted (they were free). Check the result
with `fsck.hfsplus -n -f IMAGE`. Limits: HFSX only (binary key compare), ASCII-ish names, catalog fork without overflow extents, no extended attributes.
"""
import os, struct, sys, time

MAC_EPOCH = 2082844800
CHUNK = 512


class Overflow(Exception):
    pass


def u16(b, o=0): return struct.unpack('>H', b[o:o + 2])[0]
def u32(b, o=0): return struct.unpack('>I', b[o:o + 4])[0]
def u64(b, o=0): return struct.unpack('>Q', b[o:o + 8])[0]


class Img:
    def __init__(self, path, undo=None, touched=None):
        self.touched = open(touched, 'a') if touched else None
        self.f = open(path, 'r+b')
        self.f.seek(0, 2)
        self.size = self.f.tell()
        self.undo = open(undo, 'ab') if undo else None
        self.saved = set()

    def read(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def write(self, off, data):
        """Write metadata: the original 512-byte granules are saved to the undo file first."""
        if self.undo is not None:
            first, last = off // CHUNK, (off + len(data) - 1) // CHUNK
            for c in range(first, last + 1):
                if c not in self.saved:
                    self.saved.add(c)
                    orig = self.read(c * CHUNK, CHUNK)
                    self.undo.write(struct.pack('>QI', c * CHUNK, len(orig)) + orig)
            self.undo.flush()
            os.fsync(self.undo.fileno())
        self.f.seek(off)
        self.f.write(data)
        self.log(off, len(data))

    def write_raw(self, off, data):
        self.f.seek(off)
        self.f.write(data)
        self.log(off, len(data))

    def log(self, off, n):
        if self.touched is not None:
            self.touched.write(f'{off} {n}\n')
            self.touched.flush()

    def sync(self):
        self.f.flush()
        os.fsync(self.f.fileno())


def undo_all(imgpath, undopath):
    recs = []
    with open(undopath, 'rb') as u:
        while True:
            h = u.read(12)
            if len(h) < 12:
                break
            off, n = struct.unpack('>QI', h)
            recs.append((off, u.read(n)))
    with open(imgpath, 'r+b') as f:
        for off, data in reversed(recs):
            f.seek(off)
            f.write(data)
        f.flush()
        os.fsync(f.fileno())
    print(f'restored {len(recs)} regions')


def utf16(name):
    return struct.unpack('>%dH' % (len(name.encode('utf-16-be')) // 2), name.encode('utf-16-be'))


def mk_key(parent, name):
    nb = name.encode('utf-16-be')
    klen = 6 + len(nb)
    return struct.pack('>HIH', klen, parent, len(nb) // 2) + nb


def key_of(rec):
    parent = u32(rec, 2)
    n = u16(rec, 6)
    return (parent, struct.unpack('>%dH' % n, rec[8:8 + 2 * n]) if n else ())


def key_len(rec):
    return 2 + u16(rec, 0)


class HFSW:
    def __init__(self, img):
        self.img = img
        self.vh = bytearray(img.read(1024, 512))
        if self.vh[:2] != b'HX':
            raise SystemExit('not an HFSX volume (this writer only does the case-sensitive variant)')
        self.bs = u32(self.vh, 40)
        self.total = u32(self.vh, 44)
        self.cat = self._fork(272)
        self.alloc = self._fork(112)
        for nm, fk in (('catalog', self.cat), ('allocation', self.alloc)):
            if sum(c for _, c in fk[2]) != fk[1]:
                raise SystemExit(f'{nm} file has overflow extents: not supported')
        hdr = self.cat_read(0, 512)
        self.nsize = u16(hdr, 32)
        self.hdr = bytearray(self.cat_read(0, self.nsize))
        if self.hdr[37 + 14] != 0xBC:
            raise SystemExit('catalog is not binary-compare (not HFSX)')
        self.bitmap = None
        self.dirty_bits = set()
        self.now = int(time.time()) + MAC_EPOCH

    # ----- forks
    def _fork(self, o):
        size = u64(self.vh, o)
        blocks = u32(self.vh, o + 12)
        ext = [(u32(self.vh, o + 16 + 8 * i), u32(self.vh, o + 20 + 8 * i)) for i in range(8)]
        return size, blocks, [e for e in ext if e[1]]

    def _fork_io(self, fork, off, n, data=None):
        out = b''
        pos = 0
        for start, cnt in fork[2]:
            ln = cnt * self.bs
            if off < pos + ln and n > 0:
                o = max(off - pos, 0)
                take = min(ln - o, n)
                if data is None:
                    out += self.img.read(start * self.bs + o, take)
                else:
                    self.img.write(start * self.bs + o, data[len(data) - n:len(data) - n + take])
                off += take
                n -= take
            pos += ln
        if n:
            raise SystemExit('fork access beyond its extents')
        return out

    def cat_read(self, off, n):
        return self._fork_io(self.cat, off, n)

    def cat_write(self, off, data):
        self._fork_io(self.cat, off, len(data), data)

    # ----- nodes
    def node_read(self, n):
        return bytearray(self.cat_read(n * self.nsize, self.nsize))

    def node_write(self, n, data):
        assert len(data) == self.nsize
        self.cat_write(n * self.nsize, bytes(data))

    def parse(self, node):
        nrec = u16(node, 10)
        offs = [u16(node, self.nsize - 2 * (i + 1)) for i in range(nrec + 1)]
        return [bytes(node[offs[i]:offs[i + 1]]) for i in range(nrec)]

    def fits(self, recs):
        return 14 + sum(len(r) for r in recs) + 2 * (len(recs) + 1) <= self.nsize

    def build(self, flink, blink, kind, height, recs):
        if not self.fits(recs):
            raise Overflow()
        n = bytearray(self.nsize)
        struct.pack_into('>IIbBHH', n, 0, flink, blink, kind, height, len(recs), 0)
        off = 14
        offs = []
        for r in recs:
            assert len(r) % 2 == 0, 'odd record length'
            offs.append(off)
            n[off:off + len(r)] = r
            off += len(r)
        offs.append(off)
        for i, o in enumerate(offs):
            struct.pack_into('>H', n, self.nsize - 2 * (i + 1), o)
        return n

    # ----- header record fields (node 0, header record at 14)
    def H(self, name):
        o = {'depth': (14, 2), 'root': (16, 4), 'leafrecs': (20, 4), 'first': (24, 4), 'last': (28, 4), 'total': (36, 4), 'free': (40, 4)}[name]
        return int.from_bytes(self.hdr[o[0]:o[0] + o[1]], 'big')

    def Hset(self, name, v):
        o = {'depth': (14, 2), 'root': (16, 4), 'leafrecs': (20, 4), 'first': (24, 4), 'last': (28, 4), 'total': (36, 4), 'free': (40, 4)}[name]
        self.hdr[o[0]:o[0] + o[1]] = v.to_bytes(o[1], 'big')

    def alloc_node(self):
        mapoff = 248
        total = self.H('total')
        for i in range(total):
            byte, bit = mapoff + i // 8, 0x80 >> (i % 8)
            if not self.hdr[byte] & bit:
                self.hdr[byte] |= bit
                self.Hset('free', self.H('free') - 1)
                return i
        raise SystemExit('the catalog B-tree has no free node (growing it is not supported)')

    # ----- tree search / insert
    def find_leaf(self, key):
        path = []
        n = self.H('root')
        while True:
            node = self.node_read(n)
            kind = struct.unpack('b', node[8:9])[0]
            if kind == -1:
                return path, n, node
            recs = self.parse(node)
            idx = 0
            for i, r in enumerate(recs):
                if key_of(r) <= key:
                    idx = i
                else:
                    break
            path.append((n, idx))
            n = u32(recs[idx], len(recs[idx]) - 4)

    def lookup(self, parent, name):
        key = (parent, utf16(name))
        _, n, node = self.find_leaf(key)
        for i, r in enumerate(self.parse(node)):
            if key_of(r) == key:
                return n, i, r
        return None

    def insert(self, rec):
        key = key_of(rec)
        path, n, node = self.find_leaf(key)
        recs = self.parse(node)
        pos = 0
        while pos < len(recs) and key_of(recs[pos]) < key:
            pos += 1
        if pos < len(recs) and key_of(recs[pos]) == key:
            raise SystemExit(f'catalog record exists already: {key}')
        assert not (pos == 0 and path), 'record sorts before the first key of its leaf (not supported)'
        recs.insert(pos, rec)
        self.Hset('leafrecs', self.H('leafrecs') + 1)
        self._store(path, n, node, recs, -1, 1)

    def _store(self, path, n, node, recs, kind, height):
        """Write recs as node n; split when they do not fit (and push an index record to the parent)."""
        flink, blink = u32(node, 0), u32(node, 4)
        if self.fits(recs):
            self.node_write(n, self.build(flink, blink, kind, height, recs))
            return
        # split near half by bytes
        total = sum(len(r) for r in recs)
        acc, k = 0, 0
        while k < len(recs) - 1 and acc + len(recs[k]) < total // 2:
            acc += len(recs[k])
            k += 1
        k = max(1, min(k, len(recs) - 1))
        left, right = recs[:k], recs[k:]
        new = self.alloc_node()
        self.node_write(n, self.build(new, blink, kind, height, left))
        self.node_write(new, self.build(flink, n, kind, height, right))
        if flink:
            nn = self.node_read(flink)
            struct.pack_into('>I', nn, 4, new)
            self.node_write(flink, nn)
        elif kind == -1:
            self.Hset('last', new)
        # index record for the new node: its first key + pointer
        first = right[0]
        klen = key_len(first)
        idx_rec = first[:klen] + struct.pack('>I', new)
        if not path:
            # n was the root: make a new root over n and new
            root = self.alloc_node()
            lk = recs[0]
            lrec = lk[:key_len(lk)] + struct.pack('>I', n)
            self.node_write(root, self.build(0, 0, 0, height + 1, [lrec, idx_rec]))
            self.Hset('root', root)
            self.Hset('depth', self.H('depth') + 1)
            return
        pn, pidx = path[-1]
        pnode = self.node_read(pn)
        precs = self.parse(pnode)
        precs.insert(pidx + 1, idx_rec)
        self._store(path[:-1], pn, pnode, precs, 0, height + 1)

    def replace_rec(self, leaf_n, idx, newrec):
        node = self.node_read(leaf_n)
        recs = self.parse(node)
        assert len(recs[idx]) == len(newrec)
        recs[idx] = newrec
        self.node_write(leaf_n, self.build(u32(node, 0), u32(node, 4), -1, 1, recs))

    # ----- allocation bitmap
    def load_bitmap(self):
        if self.bitmap is None:
            self.bitmap = bytearray(self._fork_io(self.alloc, 0, self.alloc[0]))

    def is_free(self, b):
        return not self.bitmap[b >> 3] & (0x80 >> (b & 7))

    def alloc_run(self, nblocks):
        """Free blocks for a file: one run if there is one, else up to 8 runs. Returns [(start, count)] and marks them used."""
        self.load_bitmap()
        start_at = u32(self.vh, 52)
        runs = []
        need = nblocks
        # first look for one contiguous run
        for begin in (start_at, 0):
            b = begin
            while b < self.total:
                if self.is_free(b):
                    e = b
                    while e < self.total and self.is_free(e) and e - b < need:
                        e += 1
                    if e - b >= need:
                        runs = [(b, need)]
                        break
                    b = e
                else:
                    b += 1
            if runs:
                break
        if not runs:
            b = 0
            while b < self.total and need and len(runs) < 8:
                if self.is_free(b):
                    e = b
                    while e < self.total and self.is_free(e) and e - b < need:
                        e += 1
                    runs.append((b, e - b))
                    need -= e - b
                    b = e
                else:
                    b += 1
            if need:
                raise SystemExit('not enough free blocks (or too fragmented)')
        for s, c in runs:
            for b in range(s, s + c):
                self.bitmap[b >> 3] |= 0x80 >> (b & 7)
                self.dirty_bits.add(b >> 3)
        free = u32(self.vh, 48) - nblocks
        struct.pack_into('>I', self.vh, 48, free)
        struct.pack_into('>I', self.vh, 52, runs[-1][0] + runs[-1][1])
        return runs

    # ----- catalog objects
    def resolve(self, path):
        cur = 2
        rec = None
        for comp in [c for c in path.split('/') if c]:
            hit = self.lookup(cur, comp)
            if not hit:
                return None
            rec = hit
            body = hit[2][key_len(hit[2]):]
            if u16(body, 0) != 1:
                return hit[2], None
            cur = u32(body, 8)
        return (rec[2] if rec else None), cur

    def _bump_parent(self, parent, folders=0):
        """valence += 1 (and the sub-folder count) in the parent's folder record, content mod date now."""
        th = self.lookup(parent, '')
        if not th:
            raise SystemExit(f'no thread record for folder {parent}')
        body = th[2][key_len(th[2]):]
        pp, nlen = u32(body, 4), u16(body, 8)
        name = body[10:10 + 2 * nlen].decode('utf-16-be')
        hit = self.lookup(pp, name)
        n, i, rec = hit
        r = bytearray(rec)
        b = key_len(rec)
        struct.pack_into('>I', r, b + 4, u32(r, b + 4) + 1)
        struct.pack_into('>I', r, b + 16, self.now)
        if folders:
            fc = u32(r, b + 84)
            struct.pack_into('>I', r, b + 84, fc + folders)
        self.replace_rec(n, i, bytes(r))

    def next_cnid(self):
        c = u32(self.vh, 64)
        struct.pack_into('>I', self.vh, 64, c + 1)
        return c

    def _thread(self, kind, cnid, parent, name):
        nb = name.encode('utf-16-be')
        return mk_key(cnid, '') + struct.pack('>hHIH', kind, 0, parent, len(nb) // 2) + nb

    def mkdir(self, path, mode=0o755):
        parent_path, name = path.rstrip('/').rsplit('/', 1)
        res = self.resolve(parent_path or '/')
        if res is None or res[1] is None:
            raise SystemExit(f'no such folder: {parent_path}')
        parent = res[1] if parent_path else 2
        if self.lookup(parent, name):
            return u32(self.lookup(parent, name)[2][key_len(self.lookup(parent, name)[2]):], 8)
        cnid = self.next_cnid()
        t = self.now
        body = struct.pack('>hHII', 1, 0x0010, 0, cnid) + struct.pack('>IIIII', t, t, t, t, 0)
        body += struct.pack('>IIBBHI', 0, 0, 0, 0, 0o40000 | mode, 0) + b'\0' * 16 + b'\0' * 16 + struct.pack('>II', 0, 0)
        assert len(body) == 88, len(body)
        self.insert(mk_key(parent, name) + body)
        self.insert(self._thread(3, cnid, parent, name))
        self._bump_parent(parent, folders=1)
        struct.pack_into('>I', self.vh, 36, u32(self.vh, 36) + 1)
        return cnid

    def put(self, data, path, mode=0o644):
        parent_path, name = path.rsplit('/', 1)
        res = self.resolve(parent_path or '/')
        if res is None or res[1] is None:
            raise SystemExit(f'no such folder: {parent_path}')
        parent = res[1] if parent_path else 2
        if self.lookup(parent, name):
            raise SystemExit(f'exists: {path}')
        nblocks = (len(data) + self.bs - 1) // self.bs
        runs = self.alloc_run(nblocks) if nblocks else []
        pos = 0
        for s, c in runs:
            chunk = data[pos:pos + c * self.bs]
            self.img.write_raw(s * self.bs, chunk + b'\0' * (c * self.bs - len(chunk)) if len(chunk) % self.bs else chunk)
            pos += c * self.bs
        cnid = self.next_cnid()
        t = self.now
        ext = b''.join(struct.pack('>II', s, c) for s, c in runs) + b'\0' * 8 * (8 - len(runs))
        fork = struct.pack('>QII', len(data), 0, nblocks) + ext
        body = struct.pack('>hHII', 2, 0x0002, 0, cnid) + struct.pack('>IIIII', t, t, t, t, 0)
        body += struct.pack('>IIBBHI', 0, 0, 0, 0, 0o100000 | mode, 1) + b'????????' + b'\0' * 8 + b'\0' * 16 + struct.pack('>II', 0, 0)
        body += fork + b'\0' * 80
        assert len(body) == 248, len(body)
        self.insert(mk_key(parent, name) + body)
        self.insert(self._thread(4, cnid, parent, name))
        self._bump_parent(parent)
        struct.pack_into('>I', self.vh, 32, u32(self.vh, 32) + 1)
        return cnid

    def replace(self, data, path):
        """Give an existing file new contents (same CNID and record): new blocks first, then the old ones are freed."""
        parent_path, name = path.rsplit('/', 1)
        res = self.resolve(parent_path or '/')
        if res is None or res[1] is None:
            raise SystemExit(f'no such folder: {parent_path}')
        hit = self.lookup(res[1] if parent_path else 2, name)
        if not hit:
            raise SystemExit(f'no such file: {path}')
        n, i, rec = hit
        b = key_len(rec)
        if u16(rec, b) != 2:
            raise SystemExit(f'not a file: {path}')
        r = bytearray(rec)
        fo = b + 88
        old_blocks = u32(r, fo + 12)
        old_ext = [(u32(r, fo + 16 + 8 * k), u32(r, fo + 20 + 8 * k)) for k in range(8)]
        old_ext = [e for e in old_ext if e[1]]
        if sum(c for _, c in old_ext) != old_blocks:
            raise SystemExit('file has overflow extents: not supported')
        nblocks = (len(data) + self.bs - 1) // self.bs
        runs = self.alloc_run(nblocks) if nblocks else []
        pos = 0
        for s_, c in runs:
            chunk = data[pos:pos + c * self.bs]
            self.img.write_raw(s_ * self.bs, chunk + b'\0' * (c * self.bs - len(chunk)))
            pos += c * self.bs
        self.load_bitmap()
        for s_, c in old_ext:
            for blk in range(s_, s_ + c):
                self.bitmap[blk >> 3] &= ~(0x80 >> (blk & 7))
                self.dirty_bits.add(blk >> 3)
        struct.pack_into('>I', self.vh, 48, u32(self.vh, 48) + old_blocks)
        ext = b''.join(struct.pack('>II', s_, c) for s_, c in runs) + b'\0' * 8 * (8 - len(runs))
        r[fo:fo + 80] = struct.pack('>QII', len(data), 0, nblocks) + ext
        struct.pack_into('>I', r, b + 16, self.now)
        self.replace_rec(n, i, bytes(r))

    # ----- finish
    def commit(self):
        # allocation bitmap: only the touched 512-byte granules
        if self.bitmap is not None and self.dirty_bits:
            chunks = sorted({b // CHUNK for b in self.dirty_bits})
            for c in chunks:
                self._fork_io(self.alloc, c * CHUNK, min(CHUNK, len(self.bitmap) - c * CHUNK), bytes(self.bitmap[c * CHUNK:c * CHUNK + CHUNK]))
        self.node_write(0, self.hdr)
        struct.pack_into('>I', self.vh, 20, self.now)               # modifyDate
        struct.pack_into('>I', self.vh, 68, u32(self.vh, 68) + 1)   # writeCount
        self.img.write(1024, bytes(self.vh))
        self.img.write(self.img.size - 1024, bytes(self.vh))        # the backup volume header
        self.img.sync()


def install_tree(w, local, target):
    w.mkdir(target)
    for root, dirs, files in os.walk(local):
        rel = os.path.relpath(root, local)
        base = target if rel == '.' else target.rstrip('/') + '/' + rel
        for d in sorted(dirs):
            w.mkdir(base + '/' + d)
        for fn in sorted(files):
            p = os.path.join(root, fn)
            mode = 0o755 if os.access(p, os.X_OK) else 0o644
            with open(p, 'rb') as f:
                data = f.read()
            print(f'  {base}/{fn} ({len(data)} bytes, {oct(mode)})')
            w.put(data, base + '/' + fn, mode)


def main():
    a = sys.argv[1:]
    undo = touched = None
    if '--undo' in a:
        i = a.index('--undo')
        undo = a[i + 1]
        del a[i:i + 2]
    if '--touched' in a:
        i = a.index('--touched')
        touched = a[i + 1]
        del a[i:i + 2]
    if len(a) < 2:
        raise SystemExit(__doc__)
    image, cmd, args = a[0], a[1], a[2:]
    if cmd == 'undo':
        return undo_all(image, args[0])
    img = Img(image, undo, touched)
    w = HFSW(img)
    if cmd == 'mkdir':
        w.mkdir(args[0], int(args[1], 8) if len(args) > 1 else 0o755)
    elif cmd == 'put':
        with open(args[0], 'rb') as f:
            data = f.read()
        w.put(data, args[1], int(args[2], 8) if len(args) > 2 else 0o644)
    elif cmd == 'replace':
        with open(args[0], 'rb') as f:
            data = f.read()
        w.replace(data, args[1])
    elif cmd == 'install-tree':
        install_tree(w, args[0], args[1])
    else:
        raise SystemExit(__doc__)
    w.commit()
    print('done')


if __name__ == '__main__':
    main()
