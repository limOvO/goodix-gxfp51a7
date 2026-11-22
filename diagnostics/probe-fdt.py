#!/usr/bin/env python3
"""Measures the FDT reply on firmware 14115 so the zone count and value layout
are known rather than assumed. The Sigfrodr driver expects 12 zones (reply
>= 31 bytes); the 14115 reference parses 6 zones from a 20-byte reply."""
import ctypes, fcntl, struct, os, time

SPI_IOC_MESSAGE_1 = 0x40206B00
SPI_IOC_WR_MODE = 0x40016B01
SPI_IOC_WR_BITS_PER_WORD = 0x40016B03
SPI_IOC_WR_MAX_SPEED_HZ = 0x40046B04
AMORCE = bytes([0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x88])


def cksum(b):
    return (0xAA - (sum(b) & 0xFF)) & 0xFF


class X(ctypes.Structure):
    _fields_ = [("tx", ctypes.c_uint64), ("rx", ctypes.c_uint64),
                ("len", ctypes.c_uint32), ("speed", ctypes.c_uint32),
                ("delay", ctypes.c_uint16), ("bits", ctypes.c_uint8),
                ("cs", ctypes.c_uint8), ("pad", ctypes.c_uint32)]


class S:
    def __init__(self, p='/dev/spidev1.0'):
        self.fd = os.open(p, os.O_RDWR)
        fcntl.ioctl(self.fd, SPI_IOC_WR_MODE, struct.pack('B', 0))
        fcntl.ioctl(self.fd, SPI_IOC_WR_BITS_PER_WORD, struct.pack('B', 8))
        fcntl.ioctl(self.fd, SPI_IOC_WR_MAX_SPEED_HZ, struct.pack('I', 10_000_000))
        self.z = bytes(8192)

    def x(self, tx, n):
        b = ctypes.create_string_buffer(n)
        t = X()
        t.tx = ctypes.cast(ctypes.c_char_p(tx), ctypes.c_void_p).value
        t.rx = ctypes.addressof(b)
        t.len = n
        t.speed = 10_000_000
        t.bits = 8
        fcntl.ioctl(self.fd, SPI_IOC_MESSAGE_1, bytes(t))
        return b.raw

    def wf(self, ty, body):
        h = bytes([ty, len(body) & 0xFF, (len(body) >> 8) & 0xFF])
        h += bytes([(h[0] + h[1] + h[2]) & 0xFF])
        self.x(h, 4)
        time.sleep(0.002)
        self.x(body, len(body))

    def plain(self, body):
        self.wf(0xA0, AMORCE)
        time.sleep(0.008)
        self.wf(0xA0, body)

    def rf(self, cap=8192):
        h = self.x(self.z[:4], 4)
        if h[0] not in (0xA0, 0xB0):
            return None, None
        n = h[1] | (h[2] << 8)
        if not (0 < n <= cap):
            return None, None
        time.sleep(0.0003)
        return h[0], self.x(self.z[:n], n)

    def drain(self, secs=0.35):
        end = time.time() + secs
        out = []
        while time.time() < end:
            t, b = self.rf()
            if b is not None:
                out.append((t, b))
                time.sleep(0.004)
        return out


BOOT = bytes([0xb2, 0xb2, 0xc2, 0xc2, 0xa7, 0xa7, 0xb6, 0xb6, 0xa6, 0xa6, 0xb6, 0xb6])
FINGER = bytes([0x80, 0xb1, 0x80, 0xc1, 0x80, 0xa6, 0x80, 0xb6, 0x80, 0xa5, 0x80, 0xb6])


def fdt(state, zones):
    cmd = 0x32 if state == 0x0c else 0x34 if state == 0x0e else 0x36
    payload = bytes([state, 0x01]) + zones
    b = bytes([cmd, len(payload) + 1, 0]) + payload
    b += bytes([cksum(b)])
    return b


s = S()
s.drain(0.2)

print("=== FDT mode 0x36 / 0x0d with boot zones ===")
s.plain(fdt(0x0d, BOOT))
for t, b in s.drain(0.35):
    print(f"  type=0x{t:02x} len={len(b)} {b.hex()}")
    if b and b[0] in (0x36, 0x32) and len(b) >= 20:
        wl = b[1] | (b[2] << 8)
        print(f"    wire_len={wl} cksum_ok={b[-1] == cksum(b[:-1])}")
        print(f"    touchflag(body[5])=0x{b[5]:02x}")
        z6 = [b[7 + 2 * i] | (b[8 + 2 * i] << 8) for i in range(6)]
        print(f"    6 zones: {z6}")
        if len(b) >= 31:
            z12 = [b[7 + 2 * i] | (b[8 + 2 * i] << 8) for i in range(12)]
            print(f"    12 zones: {z12}")

print("\n=== repeat x3 (stability) ===")
for i in range(3):
    s.plain(fdt(0x0d, BOOT))
    for t, b in s.drain(0.3):
        if b and b[0] in (0x36, 0x32):
            z6 = [b[7 + 2 * j] | (b[8 + 2 * j] << 8) for j in range(6)]
            print(f"  {i}: len={len(b)} zones6={z6}")

os.close(s.fd)
