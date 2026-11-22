#!/usr/bin/env python3
"""Robust GXFP51A7 probe: never loses SPI frame sync, and reports every
exchange so the reply layout is unambiguous.

Motivation: on this unit the firmware is GF_ST411SEC_APP_14115, not the
APP_14003 the driver targets. Its 0xF2 memory-read replies look canned (the
payload is identical at every address), so before concluding anything this
tool (1) validates framing, (2) opens the command gate exactly as the driver
does, and (3) re-tests 0xF2 with the gate open.
"""
import ctypes, fcntl, struct, sys, time, os

SPI_IOC_MESSAGE_1 = 0x40206B00
SPI_IOC_WR_MODE = 0x40016B01
SPI_IOC_WR_BITS_PER_WORD = 0x40016B03
SPI_IOC_WR_MAX_SPEED_HZ = 0x40046B04

AMORCE = bytes([0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x88])
CFG = open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "config_pcap.bin"), "rb").read()


def cksum(b):
    return (0xAA - (sum(b) & 0xFF)) & 0xFF


class Xfer(ctypes.Structure):
    _fields_ = [("tx_buf", ctypes.c_uint64), ("rx_buf", ctypes.c_uint64),
                ("len", ctypes.c_uint32), ("speed_hz", ctypes.c_uint32),
                ("delay_usecs", ctypes.c_uint16), ("bits_per_word", ctypes.c_uint8),
                ("cs_change", ctypes.c_uint8), ("pad", ctypes.c_uint32)]


class Sensor:
    def __init__(self, path='/dev/spidev1.0'):
        self.fd = os.open(path, os.O_RDWR)
        fcntl.ioctl(self.fd, SPI_IOC_WR_MODE, struct.pack('B', 0))
        fcntl.ioctl(self.fd, SPI_IOC_WR_BITS_PER_WORD, struct.pack('B', 8))
        fcntl.ioctl(self.fd, SPI_IOC_WR_MAX_SPEED_HZ, struct.pack('I', 10_000_000))
        self.zero = bytes(8192)

    def _x(self, tx, n):
        rx = ctypes.create_string_buffer(n)
        t = Xfer()
        t.tx_buf = ctypes.cast(ctypes.c_char_p(tx), ctypes.c_void_p).value
        t.rx_buf = ctypes.addressof(rx)
        t.len = n
        t.speed_hz = 10_000_000
        t.bits_per_word = 8
        fcntl.ioctl(self.fd, SPI_IOC_MESSAGE_1, bytes(t))
        return rx.raw

    def write_frame(self, typ, body):
        hdr = bytes([typ, len(body) & 0xFF, (len(body) >> 8) & 0xFF])
        hdr += bytes([(hdr[0] + hdr[1] + hdr[2]) & 0xFF])
        self._x(hdr, 4)
        time.sleep(0.002)
        self._x(body, len(body))

    def sync(self, index=None):
        """Returns body bytes of the next valid frame, or None. Reads header
        repeatedly, discarding garbage one byte at a time so sync cannot be
        lost permanently (which shifts every later read)."""
        for _attempt in range(40):
            time.sleep(0.004)
            for _shift in range(8):
                h = self._x(self.zero[:4], 4)
                if h[0] in (0xA0, 0xB0):
                    n = h[1] | (h[2] << 8)
                    if 0 < n <= 8192:
                        time.sleep(0.0003)
                        return h[0], self._x(self.zero[:n], n)
                # not a header: resync by reading 1 byte at a time
                self._x(self.zero[:1], 1)
        return None, None

    def plain(self, body):
        self.write_frame(0xA0, AMORCE)
        time.sleep(0.008)
        self.write_frame(0xA0, body)

    def drain(self, quiet=4, cap=30):
        out, misses = [], 0
        for _ in range(cap):
            t, b = self.sync()
            if t is None:
                misses += 1
                if out and misses >= quiet:
                    break
                continue
            out.append((t, b))
            misses = 0
        return out


def show(frames):
    for t, b in frames:
        print(f"    type=0x{t:02x} len={len(b):3d}  {b.hex()}")


def f2(s, addr, length):
    a = (addr - 0x08000000) & 0xFFFFFFFF
    pkt = bytes([0xF2, 0x09, 0x00]) + a.to_bytes(4, 'little') + length.to_bytes(4, 'little')
    pkt += bytes([cksum(pkt)])
    print(f"  -> 0xF2 addr=0x{addr:08x} len={length}  frame={pkt.hex()}")
    s.plain(pkt)
    frames = s.drain()
    show(frames)
    return frames


def main():
    s = Sensor()
    print("=== flush ===")
    show(s.drain(quiet=2))

    print("\n=== 0xA8 firmware ===")
    s.plain(bytes([0xA8, 0x03, 0x00, 0x70, 0xDC, 0xB3]))
    show(s.drain())

    print("\n=== 0xF2 BEFORE gate (3 addresses) ===")
    for a in (0x20000000, 0x20007f14, 0x20010000):
        f2(s, a, 48)

    print("\n=== open the gate: 0x90 config, 0x96 enable, 0xD0 reqtls ===")
    cn = len(CFG) + 1
    frame = bytes([0x90, cn & 0xFF, (cn >> 8) & 0xFF]) + CFG
    frame += bytes([cksum(frame)])
    s.write_frame(0xA0, AMORCE)
    time.sleep(0.015)
    s.write_frame(0xA0, frame)
    time.sleep(0.080)
    print("  0x90 ->"); show(s.drain())
    print("  0x96 ->")
    s.plain(bytes([0x96, 0x03, 0x00, 0x01, 0x00, 0x10]))
    time.sleep(0.020)
    show(s.drain())
    print("  0xD0 ->")
    s.plain(bytes([0xD0, 0x03, 0x00, 0x00, 0x00, 0xD7]))
    time.sleep(0.020)
    show(s.drain())

    print("\n=== 0xF2 AFTER gate (3 addresses) ===")
    for a in (0x20000000, 0x20007f14, 0x20010000):
        f2(s, a, 48)

    os.close(s.fd)


main()
