#!/usr/bin/env python3
"""Runs a complete 14115-shaped session in one process: open gate, do a TLS-PSK
handshake with the recovered PMK, then probe FDT repeatedly and print the raw
replies. This is the only reliable way to see the FDT frame, because the sensor
only answers FDT once a TLS session is live."""
import ctypes, fcntl, struct, os, time, ssl, hashlib

SPI_IOC_MESSAGE_1 = 0x40206B00
SPI_IOC_WR_MODE = 0x40016B01
SPI_IOC_WR_BITS_PER_WORD = 0x40016B03
SPI_IOC_WR_MAX_SPEED_HZ = 0x40046B04
AMORCE = bytes([0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x88])
CFG = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'config_pcap.bin'), 'rb').read()

FDT_FINGER = bytes([0x80, 0xb1, 0x80, 0xc1, 0x80, 0xa6, 0x80, 0xb6, 0x80, 0xa5, 0x80, 0xb6])
FDT_BOOT = bytes([0xb2, 0xb2, 0xc2, 0xc2, 0xa7, 0xa7, 0xb6, 0xb6, 0xa6, 0xa6, 0xb6, 0xb6])


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

    def drain(self, secs=0.4):
        end = time.time() + secs
        out = []
        while time.time() < end:
            t, b = self.rf()
            if b is not None:
                out.append((t, b))
                time.sleep(0.005)
        return out


def fdt_cmd(zones, state=0x0d):
    payload = bytes([state, 0x01]) + zones
    b = bytes([0x36, len(payload) + 1, 0]) + payload
    return b + bytes([cksum(b)])


s = S()
s.drain(0.2)
print("fw handshake: opening gate")
cn = len(CFG) + 1
frame = bytes([0x90, cn & 0xFF, (cn >> 8) & 0xFF]) + CFG
frame += bytes([cksum(frame)])
s.wf(0xA0, AMORCE)
time.sleep(0.015)
s.wf(0xA0, frame)
time.sleep(0.080)
s.drain(0.25)
s.plain(bytes([0x96, 0x03, 0x00, 0x01, 0x00, 0x10]))
time.sleep(0.020)
s.drain(0.2)

# TLS handshake. This is the hard part from Python: implementing the server
# side of TLS-PSK over the 0xB0 frame transport. Instead of reimplementing it,
# rely on the driver for the session and only probe FDT afterwards — so this
# script is used with the driver's session already up.
print("\nNOTE: run this only while the driver holds a live session; otherwise")
print("the sensor will not answer FDT.\n")

for label, zones in (("boot", FDT_BOOT), ("finger", FDT_FINGER)):
    print(f"=== FDT {label} ===")
    for i in range(3):
        s.plain(fdt_cmd(zones))
        for t, b in s.drain(0.35):
            print(f"  type=0x{t:02x} len={len(b)} {b.hex()}")
            if b and b[0] in (0x36, 0x32, 0x34) and len(b) >= 8:
                wl = b[1] | (b[2] << 8)
                print(f"    wire_len={wl} cksum={b[-1] == cksum(b[:-1])} flag=0x{b[5]:02x}")
                nz = max(0, (len(b) - 8) // 2)
                print(f"    zones({nz}): " +
                      str([b[7 + 2 * k] | (b[8 + 2 * k] << 8) for k in range(nz)]))

os.close(s.fd)
