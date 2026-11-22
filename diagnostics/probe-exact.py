#!/usr/bin/env python3
"""Deterministic GXFP51A7 probe with exact byte indexing.

Context: this unit reports firmware GF_ST411SEC_APP_14115, newer than the
APP_14003 the Sigfrodr driver targets. On 14115 the 0xF2 memory-read reply
appears to carry an address/length echo that the driver does not expect, which
shifts the 48-byte PSK it copies out and makes the TLS handshake fail with
"cipher operation failed".

This tool prints indexed replies for each relevant command so the framing can
be read off directly instead of guessed.
"""
import ctypes, fcntl, struct, os, sys, time

SPI_IOC_MESSAGE_1 = 0x40206B00
SPI_IOC_WR_MODE = 0x40016B01
SPI_IOC_WR_BITS_PER_WORD = 0x40016B03
SPI_IOC_WR_MAX_SPEED_HZ = 0x40046B04

AMORCE = bytes([0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x88])


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

    def wf(self, typ, body):
        h = bytes([typ, len(body) & 0xFF, (len(body) >> 8) & 0xFF])
        h += bytes([(h[0] + h[1] + h[2]) & 0xFF])
        self._x(h, 4)
        time.sleep(0.002)
        self._x(body, len(body))

    def plain(self, body):
        self.wf(0xA0, AMORCE)
        time.sleep(0.008)
        self.wf(0xA0, body)

    def read_frame(self, cap=8192):
        h = self._x(self.zero[:4], 4)
        if h[0] not in (0xA0, 0xB0):
            return None, None
        n = h[1] | (h[2] << 8)
        if not (0 < n <= cap):
            return None, None
        time.sleep(0.0003)
        return h[0], self._x(self.zero[:n], n)

    def collect(self, seconds=0.35):
        """Gathers every frame within a window; frame boundaries are explicit,
        so no silence heuristic is needed to stop."""
        end = time.time() + seconds
        out = []
        while time.time() < end:
            t, b = self.read_frame()
            if b is not None:
                out.append((t, b))
                time.sleep(0.004)
        return out


def idx(b):
    return " ".join(f"{i:02d}:{v:02x}" for i, v in enumerate(b))


def cmd(s, label, body, wait=0.35):
    print(f"\n=== {label} ===")
    print(f"  TX [{len(body)}] {body.hex()}")
    s.plain(body)
    for t, b in s.collect(wait):
        print(f"  RX type=0x{t:02x} len={len(b)}")
        print(f"     {b.hex()}")
        print(f"     {idx(b)}")


def main():
    s = Sensor()
    print("flushing:", len(s.collect(0.25)), "stale frame(s)")

    cmd(s, "0xA8 firmware", bytes([0xA8, 0x03, 0x00, 0x70, 0xDC, 0xB3]))
    cmd(s, "0x82 chip id", bytes([0x82, 0x06, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x1E]))
    cmd(s, "0xA6 read OTP", bytes([0xA6, 0x03, 0x00, 0x00, 0x00, 0x01]))
    cmd(s, "0xE4 factory hash read",
        bytes([0xE4, 0x09, 0x00, 0x03, 0x00, 0x02, 0xBB, 0x00, 0x00, 0x00, 0x00, 0xFD]))

    for addr, ln in ((0x20007F14, 56), (0x08020000, 64)):
        a = (addr - 0x08000000) & 0xFFFFFFFF
        body = bytes([0xF2, 0x09, 0x00]) + a.to_bytes(4, 'little') + ln.to_bytes(4, 'little')
        body += bytes([cksum(body)])
        cmd(s, f"0xF2 mem read 0x{addr:08x} len={ln}", body)

    os.close(s.fd)


main()
