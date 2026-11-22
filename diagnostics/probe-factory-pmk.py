#!/usr/bin/env python3
"""Attempts the firmware-14115 factory-PMK path on this unit.

Mechanism (from the GXFP51A0 bring-up for firmware GF_ST411SEC_APP_14115):
  * RAM reads via 0xF2 on 14115 are rejected/echo-only, so the PMK is not
    readable from RAM the way older firmware allows.
  * Instead the driver reads a 256-byte factory record staged at flash
    0x08004000 (the read returns a 'rejected staging' reply carrying 248 bytes).
  * That record is AES-128-CBC ciphertext: the first 16 bytes are the salt/IV,
    and the key is SHA-256(salt[16] ++ zeros[48] ++ seed[16])[:16] with a fixed
    Goodix fallback seed.
  * The plaintext starts  00 0d 00 00 00 30  followed by the 48-byte PMK.

This tool replicates that, printing every intermediate so the outcome is
verifiable. Read-only.
"""
import ctypes, fcntl, struct, os, sys, time, hashlib

SPI_IOC_MESSAGE_1 = 0x40206B00
SPI_IOC_WR_MODE = 0x40016B01
SPI_IOC_WR_BITS_PER_WORD = 0x40016B03
SPI_IOC_WR_MAX_SPEED_HZ = 0x40046B04

AMORCE = bytes([0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x88])
FALLBACK_SEED = bytes([0x5c, 0xba, 0x6e, 0x25, 0x81, 0x95, 0x18, 0xde,
                       0x2d, 0x53, 0xe9, 0x6d, 0xc0, 0x34, 0x7a, 0xb0])
SELECTOR = 0x08004000
BODY_LEN = 256
SALT_LEN = 16
PMK_LEN = 48


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

    def rf(self, cap=8192):
        h = self._x(self.zero[:4], 4)
        if h[0] not in (0xA0, 0xB0):
            return None, None
        n = h[1] | (h[2] << 8)
        if not (0 < n <= cap):
            return None, None
        time.sleep(0.0003)
        return h[0], self._x(self.zero[:n], n)

    def drain(self, secs=0.3):
        end = time.time() + secs
        out = []
        while time.time() < end:
            t, b = self.rf()
            if b is not None:
                out.append((t, b))
                time.sleep(0.004)
        return out


def derive_key(salt):
    inp = salt + bytes(64 - len(salt)) + FALLBACK_SEED
    assert len(inp) == 80, len(inp)
    return hashlib.sha256(inp).digest()[:16]


def unwrap(body):
    """Returns (pmk, plaintext) or (None, None)."""
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    if len(body) < 80 or (len(body) - SALT_LEN) % 16:
        return None, None
    key = derive_key(body[:SALT_LEN])
    dec = Cipher(algorithms.AES(key), modes.CBC(body[:SALT_LEN])).decryptor()
    pt = dec.update(body[SALT_LEN:]) + dec.finalize()
    if len(pt) < 6 + PMK_LEN:
        return None, pt
    marker = pt[0:2]
    blen = int.from_bytes(pt[2:6], 'big')
    if marker == b'\x00\x0d' and blen == PMK_LEN:
        return pt[6:6 + PMK_LEN], pt
    return None, pt


def main():
    s = Sensor()
    print("flushing", len(s.drain(0.2)), "frames")

    print("\n=== fw version ===")
    s.plain(bytes([0xA8, 0x03, 0x00, 0x70, 0xDC, 0xB3]))
    for t, b in s.drain():
        if len(b) > 4 and b[0] == 0xA8:
            print("  ", b[3:].split(b'\0')[0].decode('ascii', 'replace'))

    print(f"\n=== F2 staging read @0x{SELECTOR:08x} len={BODY_LEN} ===")
    a = (SELECTOR - 0x08000000) & 0xFFFFFFFF
    pkt = bytes([0xF2, 0x09, 0x00]) + a.to_bytes(4, 'little') + BODY_LEN.to_bytes(4, 'little')
    pkt += bytes([cksum(pkt)])
    print("  TX:", pkt.hex())
    s.plain(pkt)
    frames = s.drain(0.4)
    staging = None
    for t, b in frames:
        print(f"  RX type=0x{t:02x} len={len(b)} {b.hex()[:120]}")
        # 'rejected staging' form: cmd + len16 + addr(4) + len(4) + reqcksum + data + cksum
        if b[0] == 0xF2 and len(b) > 12:
            echo_ok = b[3:11] == a.to_bytes(4, 'little') + BODY_LEN.to_bytes(4, 'little')
            if echo_ok:
                data = b[11:-1]
                print(f"    echo matches request; stale-staging payload {len(data)}B")
                if len(data) >= 248:
                    staging = data[:248]

    if not staging:
        print("\n  no staging payload captured")
        return

    print(f"\n=== unwrap: salt={staging[:16].hex()} ===")
    print(f"  derived key = {derive_key(staging[:16]).hex()}")
    pmk, pt = unwrap(staging)
    if pmk:
        print(f"  SUCCESS  pmk={pmk.hex()}")
    else:
        print(f"  plaintext[0:16] = {pt[:16].hex() if pt else None}")
        print("  no valid marker/len; brute-forcing first byte...")
        hits = []
        for i in range(256):
            cand = bytes([i]) + staging[1:]
            p, _ = unwrap(cand)
            if p:
                hits.append((i, p))
        print(f"  candidates: {len(hits)}")
        for i, p in hits:
            print(f"    first_byte=0x{i:02x} pmk={p.hex()}")

    os.close(s.fd)


main()
