#!/usr/bin/env python3
"""Read-only access to the phone's eMMC through the recovery's USB mass-storage LUN (see mk-linux-recovery.py).

  ums.py info                      capacity + GPT partitions
  ums.py read LBA COUNT OUTFILE    raw sectors (512 B)
Uses pyusb (libusb) directly: SCSI over the bulk-only transport, so no kernel block device is needed in the container.
"""
import struct, sys, time
import usb.core, usb.util

VID, PID = 0x18d1, 0xd0b1

class UMS:
    def __init__(self, lun=0):
        self.lun = lun
        self.dev = usb.core.find(idVendor=VID, idProduct=PID)
        if self.dev is None:
            raise SystemExit('no 18d1:d0b1 device (phone not in the log-reading recovery)')
        for cfg in self.dev:
            for i in cfg:
                if i.bInterfaceClass == 8:
                    self.iface = i.bInterfaceNumber
                    for e in i:
                        if e.bEndpointAddress & 0x80: self.ep_in = e.bEndpointAddress
                        else: self.ep_out = e.bEndpointAddress
        try:
            if self.dev.is_kernel_driver_active(self.iface):
                self.dev.detach_kernel_driver(self.iface)
        except Exception:
            pass
        usb.util.claim_interface(self.dev, self.iface)
        self.tag = 1
        try:    # bulk-only mass storage reset recovery: drop any half-finished command from an earlier run
            self.dev.ctrl_transfer(0x21, 0xFF, 0, self.iface, None, 2000)
            self.dev.clear_halt(self.ep_in); self.dev.clear_halt(self.ep_out)
        except Exception:
            pass

    def cmd(self, cb, length=0, data_in=True, timeout=20000):
        self.tag += 1
        cbw = struct.pack('<4sIIBBB', b'USBC', self.tag, length, 0x80 if data_in else 0, self.lun, len(cb)) + cb.ljust(16, b'\0')
        self.dev.write(self.ep_out, cbw, timeout)
        data = b''
        csw = None
        while len(data) < length:
            # ask for a full packet at least: a failed command answers with a 13-byte CSW instead of data
            chunk = bytes(self.dev.read(self.ep_in, max(512, min(length - len(data), 1 << 20)), timeout))
            if len(chunk) == 13 and chunk[:4] == b'USBS':
                csw = chunk
                break
            data += chunk
        if csw is None:
            csw = bytes(self.dev.read(self.ep_in, 512, timeout))
        sig, tag, residue, status = struct.unpack('<4sIIB', csw)
        if sig != b'USBS' or status != 0:
            raise IOError(f'SCSI status {status} (sig {sig}, tag {tag})')
        return data

    def capacity(self):
        d = self.cmd(bytes([0x25]) + bytes(9), 8)
        last, bs = struct.unpack('>II', d)
        return last + 1, bs

    def read(self, lba, count, bs=512):
        out = b''
        while count:
            n = min(count, 2048)
            out += self.cmd(struct.pack('>BBIBHB', 0x28, 0, lba, 0, n, 0), n * bs)
            lba += n; count -= n
        return out

def gpt(u):
    h = u.read(1, 1)
    assert h[:8] == b'EFI PART', h[:8]
    entries_lba, n, sz = struct.unpack('<QII', h[72:88])
    raw = u.read(entries_lba, (n * sz + 511) // 512)
    parts = []
    for i in range(n):
        e = raw[i * sz:(i + 1) * sz]
        if e[:16] == bytes(16): continue
        first, last = struct.unpack('<QQ', e[32:48])
        parts.append((e[56:128].decode('utf-16le').rstrip('\0'), first, last))
    return parts

if __name__ == '__main__':
    import os
    u = UMS(int(os.environ.get('LUN', '0')))
    if sys.argv[1] == 'info':
        n, bs = u.capacity()
        print(f'capacity {n} blocks x {bs} = {n * bs / 2**30:.1f} GiB')
        for name, a, b in gpt(u):
            print(f'  {name:20s} lba {a:>10d}-{b:<10d} {(b - a + 1) * 512 / 2**20:10.1f} MiB')
    elif sys.argv[1] == 'text':      # LUN 1 (diag): the text the recovery wrote, up to its end marker
        n, bs = u.capacity()
        raw = u.read(0, min(n, 6144))
        i = raw.find(b'====SWEET-END====')
        sys.stdout.buffer.write(raw if i < 0 else raw[:i])
        if i < 0: sys.stderr.write('(no end marker yet: the recovery has not finished writing)\n')
    elif sys.argv[1] == 'read':
        lba, cnt, out = int(sys.argv[2], 0), int(sys.argv[3], 0), sys.argv[4]
        open(out, 'wb').write(u.read(lba, cnt))
