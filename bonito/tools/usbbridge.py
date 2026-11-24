#!/usr/bin/env python3
"""Userspace network bridge to the phone's USB Ethernet gadget (CDC-ECM, VID 1209 PID 0001), without root, tun/tap or a kernel driver.

  usbbridge.py ping [COUNT]               ARP + ICMP echo to the phone (192.168.7.2): proves the link
  usbbridge.py serve [LPORT:RPORT ...]    relay local TCP ports to the phone; default 2222:22  (then: ssh -p 2222 -i KEY root@127.0.0.1)
  usbbridge.py dump [SECONDS]             print every frame the phone sends (ARP, mDNS, ...) for debugging

Why: the dev container has its own network namespace, no CAP_NET_ADMIN and no /dev/net/tun, so the host kernel's cdc_ether interface is not usable here.
Instead libusb (pyusb, apt: python3-usb) claims the ECM data interface and this program is the host's whole IP stack: ARP, ICMP echo, and a small
TCP client (in-order only, retransmits after 1 s, no congestion control: the link is a USB cable). Same idea as the Pixel 4 port's FlameBridge.
The gadget (xnu patch 0075): phone ue0 = 192.168.7.2/24, MAC 02:00:5e:77:ee:02; this side = 192.168.7.1, MAC 02:00:5e:77:ee:01.
"""
import os, queue, select, socket, struct, sys, threading, time

try:
    import usb.core, usb.util
except ImportError:
    sys.exit('needs pyusb: apt-get install python3-usb')

VID, PID = 0x1209, 0x0001
HOST_IP, PHONE_IP = bytes([192, 168, 7, 1]), bytes([192, 168, 7, 2])
HOST_MAC = bytes.fromhex('02005e77ee01')
PHONE_MAC = bytes.fromhex('02005e77ee02')
BCAST = b'\xff' * 6
MSS = 1400
VERBOSE = bool(os.environ.get('BRIDGE_VERBOSE'))


def csum(data):
    if len(data) & 1:
        data += b'\0'
    s = sum(struct.unpack('>%dH' % (len(data) // 2), data))
    while s >> 16:
        s = (s & 0xffff) + (s >> 16)
    return (~s) & 0xffff


class Link:
    """The USB side: bulk IN/OUT of the ECM data interface, frames in and out."""

    def __init__(self):
        self.dev = usb.core.find(idVendor=VID, idProduct=PID)
        if self.dev is None:
            sys.exit('no USB device %04x:%04x: the phone\'s Ethernet gadget is not enumerated' % (VID, PID))
        cfg = self.dev.get_active_configuration() if self._configured() else None
        if cfg is None:
            self.dev.set_configuration()
            cfg = self.dev.get_active_configuration()
        data_if = None
        for intf in cfg:
            if intf.bInterfaceClass == 0x0a and intf.bAlternateSetting == 1:
                data_if = intf
        if data_if is None:
            sys.exit('no CDC data interface with alternate setting 1 in the descriptors')
        for n in {i.bInterfaceNumber for i in cfg}:
            try:
                if self.dev.is_kernel_driver_active(n):
                    self.dev.detach_kernel_driver(n)
            except (NotImplementedError, usb.core.USBError):
                pass
        usb.util.claim_interface(self.dev, data_if.bInterfaceNumber)
        self.dev.set_interface_altsetting(data_if.bInterfaceNumber, 1)
        self.ep_in = usb.util.find_descriptor(data_if, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN)
        self.ep_out = usb.util.find_descriptor(data_if, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT)
        try:   # SET_ETHERNET_PACKET_FILTER: directed + broadcast + all multicast (the gadget may ignore it)
            self.dev.ctrl_transfer(0x21, 0x43, 0x001e, 0, None, timeout=1000)
        except usb.core.USBError:
            pass
        self.rx = queue.Queue()
        self.lock = threading.Lock()
        threading.Thread(target=self._reader, daemon=True).start()

    def _configured(self):
        try:
            self.dev.get_active_configuration()
            return True
        except usb.core.USBError:
            return False

    def _reader(self):
        while True:
            try:
                d = bytes(self.ep_in.read(2048, timeout=500))
                if d:
                    self.rx.put(d)
            except usb.core.USBTimeoutError:
                continue
            except usb.core.USBError as e:
                print('usb read error:', e, file=sys.stderr)
                time.sleep(0.5)

    def send(self, frame):
        if len(frame) < 60:
            frame += b'\0' * (60 - len(frame))
        with self.lock:
            self.ep_out.write(frame, timeout=2000)


def eth(dst, etype, payload):
    return dst + HOST_MAC + struct.pack('>H', etype) + payload


def arp_frame(op, tmac, tip):
    return eth(BCAST if op == 1 else tmac, 0x0806, struct.pack('>HHBBH', 1, 0x0800, 6, 4, op) + HOST_MAC + HOST_IP + (b'\0' * 6 if op == 1 else tmac) + tip)


def ip_packet(proto, dst, payload, ident=[0]):
    ident[0] = (ident[0] + 1) & 0xffff
    hdr = struct.pack('>BBHHHBBH', 0x45, 0, 20 + len(payload), ident[0], 0x4000, 64, proto, 0) + HOST_IP + dst
    hdr = hdr[:10] + struct.pack('>H', csum(hdr)) + hdr[12:]
    return hdr + payload


class Conn:
    """One TCP connection from this side to PHONE_IP:rport, relayed to a local socket."""

    def __init__(self, stack, lsock, sport, rport):
        self.stack, self.lsock, self.sport, self.rport = stack, lsock, sport, rport
        self.snd_nxt = self.snd_una = int.from_bytes(os.urandom(4), 'big')
        self.iss = self.snd_nxt
        self.rcv_nxt = 0
        self.state = 'SYN_SENT'
        self.unacked = []          # (seq, flags, data, time)
        self.pending = b''         # bytes from the local socket not yet sent
        self.local_closed = False
        self.fin_sent = False
        self.last = time.time()
        self._seg(0x02, b'')       # SYN

    def _seg(self, flags, data):
        seq = self.snd_nxt
        self._tx(seq, flags, data)
        adv = len(data) + (1 if flags & 0x03 else 0)       # SYN and FIN take a sequence number
        if adv:
            self.unacked.append([seq, flags, data, time.time()])
            self.snd_nxt = (self.snd_nxt + adv) & 0xffffffff

    def _tx(self, seq, flags, data):
        opts = struct.pack('>BBH', 2, 4, MSS) if flags & 0x02 else b''
        hl = (20 + len(opts)) // 4
        ack = self.rcv_nxt if flags & 0x10 or self.state != 'SYN_SENT' else 0
        flags2 = flags | (0x10 if self.state != 'SYN_SENT' else 0)
        hdr = struct.pack('>HHIIBBHHH', self.sport, self.rport, seq, ack, hl << 4, flags2, 65535, 0, 0) + opts
        pseudo = HOST_IP + PHONE_IP + struct.pack('>BBH', 0, 6, len(hdr) + len(data))
        c = csum(pseudo + hdr + data)
        hdr = hdr[:16] + struct.pack('>H', c) + hdr[18:]
        self.stack.send_ip(6, hdr + data)

    def ack_only(self):
        self._tx(self.snd_nxt, 0x10, b'')

    def on_segment(self, seq, ack, flags, data):
        self.last = time.time()
        if flags & 0x04:                                   # RST
            self.state = 'CLOSED'
            return
        if self.state == 'SYN_SENT':
            if (flags & 0x12) == 0x12 and ack == self.snd_nxt:
                self.rcv_nxt = (seq + 1) & 0xffffffff
                self.snd_una = ack
                self.unacked = []
                self.state = 'ESTABLISHED'
                self.ack_only()
            return
        if flags & 0x10:                                   # ACK: drop what is acknowledged
            self.snd_una = ack
            def still_unacked(u):      # keep a segment while its end lies beyond ack (mod 2**32)
                d = (u[0] + len(u[2]) + (1 if u[1] & 3 else 0) - ack) & 0xffffffff
                return 0 < d < 0x80000000
            self.unacked = [u for u in self.unacked if still_unacked(u)]
        if data:
            if seq == self.rcv_nxt:
                self.rcv_nxt = (self.rcv_nxt + len(data)) & 0xffffffff
                try:
                    self.lsock.sendall(data)
                except OSError:
                    self.state = 'CLOSED'
            self.ack_only()                                # in order or not: tell the phone where we are
        if flags & 0x01 and seq + len(data) == self.rcv_nxt:   # FIN in order
            self.rcv_nxt = (self.rcv_nxt + 1) & 0xffffffff
            self.ack_only()
            try:
                self.lsock.shutdown(socket.SHUT_WR)
            except OSError:
                pass
            self.state = 'CLOSE_WAIT' if not self.fin_sent else 'CLOSED'

    def pump(self):
        """Move bytes local -> phone and retransmit."""
        now = time.time()
        for u in self.unacked:
            if now - u[3] > 1.0:
                self._tx(u[0], u[1], u[2])
                u[3] = now
        if self.state not in ('ESTABLISHED', 'CLOSE_WAIT'):
            return
        inflight = sum(len(u[2]) for u in self.unacked)
        if not self.local_closed and inflight < 32768:
            r, _, _ = select.select([self.lsock], [], [], 0)
            if r:
                try:
                    d = self.lsock.recv(MSS)
                except OSError:
                    d = b''
                if d:
                    self._seg(0x18, d)                     # PSH+ACK
                else:
                    self.local_closed = True
        if self.local_closed and not self.fin_sent:
            self._seg(0x11, b'')                           # FIN+ACK
            self.fin_sent = True


class Stack:
    def __init__(self, link):
        self.link = link
        self.phone_mac = PHONE_MAC
        self.conns = {}            # sport -> Conn
        self.next_port = 40000
        self.icmp_replies = queue.Queue()

    def send_ip(self, proto, payload):
        self.link.send(eth(self.phone_mac, 0x0800, ip_packet(proto, PHONE_IP, payload)))

    def poll(self, timeout=0.05):
        try:
            f = self.link.rx.get(timeout=timeout)
        except queue.Empty:
            return False
        self.handle(f)
        return True

    def handle(self, f):
        if len(f) < 14:
            return
        et = struct.unpack('>H', f[12:14])[0]
        src = f[6:12]
        if VERBOSE:
            print('rx', src.hex(), 'type %04x' % et, len(f), file=sys.stderr)
        if et == 0x0806 and len(f) >= 42:                  # ARP
            op = struct.unpack('>H', f[20:22])[0]
            smac, sip, tip = f[22:28], f[28:32], f[38:42]
            if sip == PHONE_IP:
                self.phone_mac = smac
            if op == 1 and tip == HOST_IP:
                self.link.send(arp_frame(2, smac, sip))
        elif et == 0x0800 and len(f) >= 34:
            ihl = (f[14] & 0x0f) * 4
            proto = f[23]
            if f[26:30] != PHONE_IP:
                return
            self.phone_mac = src
            body = f[14 + ihl:14 + struct.unpack('>H', f[16:18])[0]]
            if proto == 1 and body and body[0] == 0:       # echo reply
                self.icmp_replies.put(body)
            elif proto == 6 and len(body) >= 20:
                sport, dport, seq, ack, off, flags = struct.unpack('>HHIIBB', body[:14])
                c = self.conns.get(dport)
                if c and sport == c.rport:
                    c.on_segment(seq, ack, flags, body[(off >> 4) * 4:])

    def ping(self, n=4):
        self.link.send(arp_frame(1, BCAST, PHONE_IP))
        ok = 0
        for i in range(n):
            ident = struct.pack('>HH', 0x4242, i)
            msg = bytes([8, 0, 0, 0]) + ident + b'bonito'
            msg = msg[:2] + struct.pack('>H', csum(msg)) + msg[4:]
            t0 = time.time()
            self.send_ip(1, msg)
            deadline = t0 + 2
            got = False
            while time.time() < deadline and not got:
                self.poll(0.05)
                try:
                    self.icmp_replies.get_nowait()
                    got = True
                except queue.Empty:
                    pass
            print('reply from 192.168.7.2: time=%.1f ms' % ((time.time() - t0) * 1000) if got else 'no reply (seq %d)' % i)
            ok += got
            time.sleep(0.5)
        return ok

    def serve(self, maps):
        listeners = {}
        for lp, rp in maps:
            s = socket.socket()
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind(('127.0.0.1', lp))
            s.listen(4)
            listeners[s] = rp
            print('relaying 127.0.0.1:%d -> 192.168.7.2:%d' % (lp, rp), flush=True)
        self.link.send(arp_frame(1, BCAST, PHONE_IP))
        while True:
            r, _, _ = select.select(list(listeners), [], [], 0.02)
            for s in r:
                ls, _ = s.accept()
                sp = self.next_port
                self.next_port += 1
                self.conns[sp] = Conn(self, ls, sp, listeners[s])
            while self.poll(0.0):
                pass
            for sp, c in list(self.conns.items()):
                c.pump()
                if c.state == 'CLOSED' or (c.state == 'SYN_SENT' and time.time() - c.last > 10):
                    try:
                        c.lsock.close()
                    except OSError:
                        pass
                    del self.conns[sp]


def main():
    a = sys.argv[1:] or ['ping']
    link = Link()
    st = Stack(link)
    if a[0] == 'ping':
        ok = st.ping(int(a[1]) if len(a) > 1 else 4)
        sys.exit(0 if ok else 1)
    if a[0] == 'dump':
        global VERBOSE
        VERBOSE = True
        end = time.time() + (float(a[1]) if len(a) > 1 else 10)
        while time.time() < end:
            st.poll(0.1)
        return
    if a[0] == 'serve':
        maps = [tuple(int(x) for x in m.split(':')) for m in a[1:]] or [(2222, 22)]
        st.serve(maps)
        return
    sys.exit(__doc__)


main()
