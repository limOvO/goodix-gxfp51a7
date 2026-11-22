#!/usr/bin/env python3
"""Firmware-14115 factory PMK extraction, matching the GXFP51A0 reference
implementation byte for byte.

Reference layout (gxfp_14115_parse_rejected_staging_response):
    body[0]     = 0xF2
    body[1:3]   = inner length (LE16)
    body[3:11]  = echo of the request: addr32 || len32
    body[11]    = request checksum
    body[11:]   = "stale staging" payload (the boot-staged factory record)
    body[-1]    = reply checksum

The read path corrupts the FIRST payload byte, so the record is recovered by
brute-forcing that single byte: exactly one value must decrypt to
marker 0x000d + BE length 0x30. Key derivation:
    key = SHA256(salt[16] || zeros[48] || fallback_seed[16])[:16]
and the record is AES-128-CBC with salt as IV.
"""
import ctypes, fcntl, struct, os, time, hashlib, sys

SPI_IOC_MESSAGE_1 = 0x40206B00
SPI_IOC_WR_MODE = 0x40016B01
SPI_IOC_WR_BITS_PER_WORD = 0x40016B03
SPI_IOC_WR_MAX_SPEED_HZ = 0x40046B04

AMORCE = bytes([0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x88])
SEED = bytes([0x5c, 0xba, 0x6e, 0x25, 0x81, 0x95, 0x18, 0xde,
              0x2d, 0x53, 0xe9, 0x6d, 0xc0, 0x34, 0x7a, 0xb0])
PMK_LEN = 48
SALT_LEN = 16
READ_LEN = 80          # reference uses the first 80 bytes of the record


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


def derive(salt):
    return hashlib.sha256(salt + bytes(48) + SEED).digest()[:16]


def decrypt_check(salt, ct):
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    if not ct or len(ct) % 16:
        return None, None
    d = Cipher(algorithms.AES(derive(salt)), modes.CBC(salt)).decryptor()
    pt = d.update(ct) + d.finalize()
    if len(pt) >= 6 + PMK_LEN:
        if pt[0] == 0x00 and pt[1] == 0x0D and int.from_bytes(pt[2:6], 'big') == PMK_LEN:
            return pt[6:6 + PMK_LEN], pt
    return None, pt


def try_record(rec, label):
    """rec = raw record bytes (first byte possibly corrupted)."""
    print(f"\n--- {label}: {len(rec)} bytes ---")
    print(f"  head: {rec[:32].hex()}")
    pmk, pt = decrypt_check(rec[:SALT_LEN], rec[SALT_LEN:SALT_LEN + 64])
    if pmk:
        print(f"  DIRECT HIT pmk={pmk.hex()}")
        return pmk
    hits = []
    for i in range(256):
        cand = bytes([i]) + rec[1:]
        p, _ = decrypt_check(cand[:SALT_LEN], cand[SALT_LEN:SALT_LEN + 64])
        if p:
            hits.append((i, p))
    print(f"  brute-force candidates: {len(hits)}")
    for i, p in hits:
        print(f"    first=0x{i:02x} pmk={p.hex()}")
    if len(hits) == 1:
        return hits[0][1]
    return None


def read_staging(s, selector, reqlen=256):
    a = (selector - 0x08000000) & 0xFFFFFFFF
    pkt = bytes([0xF2, 0x09, 0x00]) + a.to_bytes(4, 'little') + reqlen.to_bytes(4, 'little')
    pkt += bytes([cksum(pkt)])
    s.plain(pkt)
    frames = s.drain(0.4)
    for t, b in frames:
        if b and b[0] == 0xF2 and len(b) > 12:
            return b
    return None


def main():
    s = S()
    s.drain(0.2)

    s.plain(bytes([0xA8, 0x03, 0x00, 0x70, 0xDC, 0xB3]))
    for t, b in s.drain(0.3):
        if b and b[0] == 0xA8 and len(b) > 4:
            print("fw:", b[3:].split(b'\0')[0].decode('ascii', 'replace'))

    for sel in (0x08004000, 0x08000000, 0x08020000):
        b = read_staging(s, sel)
        if not b:
            print(f"\nselector 0x{sel:08x}: no reply")
            continue
        inner = b[1] | (b[2] << 8)
        print(f"\nselector 0x{sel:08x}: body={len(b)} inner_len={inner}")
        # reference: payload starts at body[11]; it is len-12 bytes
        pay11 = b[11:-1]
        pay12 = b[12:-1]
        print(f"  payload@11={len(pay11)}B  payload@12={len(pay12)}B")
        for name, pay in (("@11", pay11), ("@12", pay12)):
            if len(pay) >= READ_LEN:
                pmk = try_record(pay[:248] if len(pay) >= 248 else pay, f"sel 0x{sel:08x} {name}")
                if pmk:
                    print("\n*** PMK FOUND ***", pmk.hex())
                    os.close(s.fd)
                    return
    os.close(s.fd)


main()
