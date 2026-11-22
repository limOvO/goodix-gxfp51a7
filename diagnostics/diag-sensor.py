#!/usr/bin/env python3
"""Direct GXFP51A7 SPI diagnostic.

Replicates the driver's MilanL transport (split writes, full-duplex chunked
reads) to answer one question: where is the 48-byte PSK actually sitting, and
what firmware is this unit running?

Read-only apart from the initial AMORCE preamble: it issues 0xA8 (firmware
version) and 0xF2 (memory read), never a flash or config command.
"""
import ctypes, fcntl, struct, sys, time, os

SPI_IOC_MESSAGE_1 = 0x40206B00
SPI_IOC_WR_MODE = 0x40016B01
SPI_IOC_WR_BITS_PER_WORD = 0x40016B03
SPI_IOC_WR_MAX_SPEED_HZ = 0x40046B04
SPI_MODE_0 = 0

AMORCE = bytes([0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x88])
PKT_PLAIN = 0xA0

class Xfer(ctypes.Structure):
    _fields_ = [("tx_buf", ctypes.c_uint64), ("rx_buf", ctypes.c_uint64),
                ("len", ctypes.c_uint32), ("speed_hz", ctypes.c_uint32),
                ("delay_usecs", ctypes.c_uint16), ("bits_per_word", ctypes.c_uint8),
                ("cs_change", ctypes.c_uint8), ("pad", ctypes.c_uint32)]

def cksum(b):
    return (0xAA - (sum(b) & 0xFF)) & 0xFF

class Sensor:
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR)
        fcntl.ioctl(self.fd, SPI_IOC_WR_MODE, struct.pack('B', SPI_MODE_0))
        fcntl.ioctl(self.fd, SPI_IOC_WR_BITS_PER_WORD, struct.pack('B', 8))
        fcntl.ioctl(self.fd, SPI_IOC_WR_MAX_SPEED_HZ, struct.pack('I', 10_000_000))
        self.zero = bytes(4096)

    def xfer(self, tx, rxlen):
        rx = ctypes.create_string_buffer(rxlen)
        t = Xfer()
        t.tx_buf = ctypes.cast(ctypes.c_char_p(tx), ctypes.c_void_p).value
        t.rx_buf = ctypes.addressof(rx)
        t.len = rxlen
        t.speed_hz = 10_000_000
        t.bits_per_word = 8
        fcntl.ioctl(self.fd, SPI_IOC_MESSAGE_1, bytes(t))
        return rx.raw

    def write_frame(self, typ, body):
        hdr = bytes([typ, len(body) & 0xFF, (len(body) >> 8) & 0xFF])
        hdr += bytes([(hdr[0] + hdr[1] + hdr[2]) & 0xFF])
        self.xfer(hdr, 4)            # split write: header first
        time.sleep(0.002)            # 2 ms, as the vendor wrapper paces it
        self.xfer(body, len(body))

    def read_frame(self, cap=512):
        hdr = self.xfer(self.zero[:4], 4)
        if hdr[0] not in (0xA0, 0xB0):
            return None, None
        n = hdr[1] | (hdr[2] << 8)
        if n == 0 or n > cap:
            return None, None
        time.sleep(0.0002)
        body = self.xfer(self.zero[:n], n)
        return hdr[0], body

    def plain_raw(self, body):
        self.write_frame(PKT_PLAIN, AMORCE)
        time.sleep(0.008)
        self.write_frame(PKT_PLAIN, body)

    def drain(self, tries=40):
        for _ in range(tries):
            t, b = self.read_frame()
            if t is None:
                return
            yield t, b

    def fw_version(self):
        body = bytes([0xA8, 0x03, 0x00, 0x70, 0xDC, 0xB3])
        self.plain_raw(body)
        time.sleep(0.010)
        self.read_frame()                    # ACK
        time.sleep(0.005)
        t, b = self.read_frame()
        if b is None or len(b) <= 3:
            return None
        return b[3:].split(b'\0')[0].decode('ascii', 'replace')

    def mem_read(self, addr, length):
        """Reads @length bytes from @addr. Responses over the frame cap are
        split into 96-byte requests: the body plus a 3-byte header still has
        to fit one frame."""
        out = bytearray()
        while len(out) < length:
            n = min(length - len(out), 96)
            chunk = self._mem_read_once(addr + len(out), n)
            if chunk is None:
                return None if not out else bytes(out)
            out += chunk
        return bytes(out)

    def _mem_read_once(self, addr, length):
        a = (addr - 0x08000000) & 0xFFFFFFFF
        pkt = bytes([0xF2, 0x09, 0x00]) + struct.pack('<I', a) + struct.pack('<I', length)
        pkt += bytes([cksum(pkt)])
        self.plain_raw(pkt)
        time.sleep(0.010)
        self.read_frame()                    # ACK
        time.sleep(0.008)
        t, b = self.read_frame(cap=2048)
        if b is None:
            return None
        hdr = 3 if (b[0] >> 4) == 0xF else 0
        return b[hdr:hdr + length]


def entropy_score(block):
    """Crude 'looks like a key' test: byte diversity plus no long zero runs."""
    uniq = len(set(block))
    zeros = block.count(0)
    return uniq, zeros


def main():
    dev = sys.argv[1] if len(sys.argv) > 1 else '/dev/spidev1.0'
    s = Sensor(dev)

    print("=== flushing pending frames ===")
    for i, (t, b) in enumerate(s.drain()):
        print(f"  stale frame type=0x{t:02x} len={len(b)}")
        if i > 8:
            break

    print("\n=== firmware version (0xA8) ===")
    fw = s.fw_version()
    print(f"  {fw!r}")

    print("\n=== PSK window scan: 0x20007f00..0x20008100 ===")
    base = 0x20007f00
    blob = s.mem_read(base, 512)
    if blob is None:
        print("  memory read FAILED")
    else:
        print(f"  got {len(blob)} bytes")
        for off in range(0, len(blob) - 48, 8):
            block = blob[off:off + 48]
            uniq, zeros = entropy_score(block)
            if uniq >= 20 and zeros < 8:
                addr = base + off
                print(f"  candidate +0x{off:03x} addr=0x{addr:08x} "
                      f"uniq={uniq} zeros={zeros}")
                print(f"    hex={block.hex()}")

    print("\n=== also check the GXFP5187 address 0x20012d68 ===")
    b2 = s.mem_read(0x20012d00, 192)
    if b2 is None:
        print("  FAILED")
    else:
        for off in range(0, len(b2) - 48, 8):
            block = b2[off:off + 48]
            uniq, zeros = entropy_score(block)
            if uniq >= 20 and zeros < 8:
                print(f"  candidate addr=0x{0x20012d00+off:08x} uniq={uniq} zeros={zeros}")
                print(f"    hex={block.hex()}")

    os.close(s.fd)

if __name__ == '__main__':
    main()
