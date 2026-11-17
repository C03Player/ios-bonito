#!/usr/bin/env python3
"""Repack a LineageOS bonito boot.img (a recovery ramdisk) so `fastboot boot` gives an unauthenticated root adb shell.

  mk-linux-recovery.py LINEAGE_BOOT.img OUT.img

The ramdisk (a gzip'd newc cpio) is rewritten entry by entry with a patched /prop.default:
ro.secure=0 (adbd stays root), ro.adb.secure=0, and (optional) /adb_keys = the host's adb public key (no RSA prompt). Boot image header v2: ramdisk size and
the absolute recovery_dtbo offset are updated; sections are page aligned.
"""
import gzip, hashlib, struct, sys

def cpio_newc(entries):
    out = b''
    ino = 1
    for name, data, mode in entries:
        nm = name.encode() + b'\0'
        hdr = '070701' + ''.join('%08X' % v for v in (ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(nm), 0)).encode().decode()
        out += hdr.encode() + nm
        out += b'\0' * ((4 - len(out) % 4) % 4)
        out += data
        out += b'\0' * ((4 - len(out) % 4) % 4)
        ino += 1
    tr = b'TRAILER!!!\0'
    out += ('070701' + ''.join('%08X' % v for v in (0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, len(tr), 0))).encode() + tr
    out += b'\0' * ((4 - len(out) % 4) % 4)
    return out

import os
WINDOW = int(os.environ.get('SWEET_WINDOW', '240'))

def patch_rc(text):
    """init.recovery.sdm670.rc: build the whole USB gadget config in `on init` (before anything can bind the UDC) with
    adb AND a read-only mass-storage LUN over the whole eMMC. Mass storage needs no adb authorisation, which matters
    because nobody can approve a prompt on the phone. The product id marker is set through
    ro.recovery.usb.adb.pid (see patch_props), because init.rc rewrites idProduct from that property."""
    anchor = '    mkdir /config/usb_gadget/g1/functions/ffs.adb\n'
    assert anchor in text, 'ffs.adb line not found'
    g = '/config/usb_gadget/g1'
    block = (f'    mkdir {g}/configs/b.1 0777 shell shell\n'
             f'    mkdir {g}/configs/b.1/strings/0x409 0770 shell shell\n'
             f'    write {g}/configs/b.1/strings/0x409/configuration "adb+ums"\n'
             f'    symlink {g}/functions/ffs.adb {g}/configs/b.1/f1\n'
             f'    mkdir {g}/functions/mass_storage.0\n'
             f'    write {g}/functions/mass_storage.0/lun.0/ro 1\n'
             f'    write {g}/functions/mass_storage.0/lun.0/removable 0\n'
             f'    write {g}/functions/mass_storage.0/lun.0/file /dev/block/mmcblk0\n'
             f'    symlink {g}/functions/mass_storage.0 {g}/configs/b.1/f2\n')
    text = text.replace(anchor, anchor + block)
    # Hand the phone back to the bootloader by itself after SWEET_WINDOW seconds: adb is not authorised, so nobody can
    # do it remotely, and the recovery screen is not something to rely on.
    text += (f'\nservice sweetreboot /system/bin/sh -c "sleep {WINDOW}; /system/bin/reboot bootloader"\n'
             '    user root\n    seclabel u:r:recovery:s0\n    oneshot\n    disabled\n'
             '\non boot\n    start sweetreboot\n')
    return text

def patch_props(lines):
    out = []
    for l in lines:
        if l.startswith('ro.recovery.usb.adb.pid='):
            l = 'ro.recovery.usb.adb.pid=D0B1'
        out.append(l)
    return out


def main(src, dst, prop_in, keys_in=None):
    d = bytearray(open(src, 'rb').read())
    assert d[:8] == b'ANDROID!'
    ksz, kaddr, rsz, raddr, s2sz, s2addr, tags, page, hver = struct.unpack('<9I', d[8:44])
    assert hver == 2, hver
    up = lambda x: (x + page - 1) // page * page
    off_k = page
    off_r = off_k + up(ksz)
    off_s = off_r + up(rsz)
    dtbo_sz, dtbo_off = struct.unpack('<IQ', d[1632:1644])
    hdr_sz, dtb_sz = struct.unpack('<II', d[1644:1652])
    off_dtbo = off_s + up(s2sz)
    assert dtbo_off == off_dtbo or dtbo_sz == 0, (dtbo_off, off_dtbo)
    off_dtb = off_dtbo + up(dtbo_sz)
    kernel = bytes(d[off_k:off_k + ksz]); ramdisk = bytes(d[off_r:off_r + rsz])
    second = bytes(d[off_s:off_s + s2sz]); dtbo = bytes(d[off_dtbo:off_dtbo + dtbo_sz]); dtb = bytes(d[off_dtb:off_dtb + dtb_sz])
    prop = open(prop_in, 'rb').read().decode()
    lines = []
    seen = set()
    for l in prop.split('\n'):
        if l.startswith('ro.secure='): l = 'ro.secure=0'
        if l.startswith('ro.adb.secure='): l = 'ro.adb.secure=0'
        seen.add(l.split('=')[0])
        lines.append(l)
    lines = patch_props(lines)
    for k, v in (('ro.secure', '0'), ('ro.adb.secure', '0')):
        if k not in seen:
            lines.append(f'{k}={v}')
    files = [('prop.default', ('\n'.join(lines)).encode(), 0o100644)]
    if keys_in:
        # adbd in recovery trusts the host keys in /adb_keys without asking on the phone
        files.append(('adb_keys', open(keys_in, 'rb').read().strip() + b'\n', 0o100644))
    # Rewrite the (decompressed) newc archive entry by entry, replacing prop.default / default.prop contents
    # and /adb_keys; everything else (modes, owners, devices, links, order) is copied unchanged.
    cp = gzip.decompress(ramdisk)
    repl = {'prop.default': ('\n'.join(lines)).encode()}
    if keys_in:
        repl['adb_keys'] = open(keys_in, 'rb').read().strip() + b'\n'
    out = bytearray(); pos = 0; done = set()
    while True:
        assert cp[pos:pos + 6] == b'070701', 'not a newc archive'
        f = [int(cp[pos + 6 + 8 * i:pos + 14 + 8 * i], 16) for i in range(13)]
        mode, fsz, nsz = f[1], f[6], f[11]
        name = cp[pos + 110:pos + 110 + nsz - 1].decode('latin1')
        ds = (pos + 110 + nsz + 3) & ~3
        data = cp[ds:ds + fsz]
        nxt = (ds + fsz + 3) & ~3
        if name == 'TRAILER!!!':
            out += cp[pos:nxt]
            break
        if name == 'init.recovery.sdm670.rc':
            data = patch_rc(data.decode()).encode()
            f[6] = len(data)
            done.add(name)
        if name in repl or (name == 'default.prop' and mode & 0o170000 == 0o100000 and b'ro.adb.secure' in data):
            data = repl.get(name, ('\n'.join(lines)).encode())
            mode = (mode & ~0o170000) | 0o100000
            f[1], f[6], f[4] = mode, len(data), 1
            done.add(name)
        hdr = b'070701' + b''.join(b'%08X' % v for v in f)
        ent = hdr + cp[pos + 110:pos + 110 + nsz]
        ent += b'\0' * ((4 - len(ent) % 4) % 4)
        ent += data
        ent += b'\0' * ((4 - len(ent) % 4) % 4)
        out += ent
        pos = nxt
    print('replaced:', sorted(done))
    new_rd = gzip.compress(bytes(out), compresslevel=9, mtime=0)
    new_rd_unused = None
    pad = lambda b: b + b'\0' * (up(len(b)) - len(b))
    body = pad(kernel) + pad(new_rd) + pad(second) + pad(dtbo) + pad(dtb)
    hdr = bytearray(d[:page])
    struct.pack_into('<I', hdr, 16, len(new_rd))
    struct.pack_into('<Q', hdr, 1636, page + len(pad(kernel)) + len(pad(new_rd)) + len(pad(second)))
    # id field = sha1 over kernel, ramdisk, second, dtbo(+size), dtb
    h = hashlib.sha1()
    for blob in (kernel, new_rd, second):
        h.update(blob); h.update(struct.pack('<I', len(blob)))
    h.update(dtbo); h.update(struct.pack('<I', len(dtbo)))
    h.update(dtb)
    hdr[576:608] = h.digest().ljust(32, b'\0')
    open(dst, 'wb').write(bytes(hdr) + body)
    print(f'{dst}: kernel {ksz} ramdisk {rsz}->{len(new_rd)} dtbo {dtbo_sz} dtb {dtb_sz}, total {len(hdr)+len(body)} bytes')

if __name__ == '__main__':
    if len(sys.argv) not in (4, 5):
        sys.exit('usage: mk-linux-recovery.py LINEAGE_BOOT.img OUT.img PROP_DEFAULT_FILE [ADBKEY.pub]')
    main(*sys.argv[1:])
