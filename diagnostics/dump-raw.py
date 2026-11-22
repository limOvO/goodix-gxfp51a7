#!/usr/bin/env python3
"""Dumps raw SPI reply frames so the exact reply layout is visible."""
import sys, os
from importlib.machinery import SourceFileLoader
here = os.path.dirname(os.path.abspath(__file__))
d = SourceFileLoader("diag", os.path.join(here, "diag-sensor.py")).load_module()

s = d.Sensor('/dev/spidev1.0')

def dump(label, fn):
    print(f"\n=== {label} ===")
    got = 0
    for t, b in fn():
        got += 1
        print(f"  type=0x{t:02x} len={len(b)}")
        print(f"    {b.hex()}")
    if not got:
        print("  (no frames)")

# 0xA8 firmware
def fw():
    s.plain_raw(bytes([0xA8, 0x03, 0x00, 0x70, 0xDC, 0xB3]))
    import time; time.sleep(0.010)
    for _ in range(30):
        t, b = s.read_frame()
        if t is None:
            return
        yield t, b
        import time; time.sleep(0.005)

dump("0xA8 firmware version", fw)

# 0xF2 mem read at the driver's 14003 address, 48 bytes
def mr(addr, ln):
    def g():
        import time
        a = (addr - 0x08000000) & 0xFFFFFFFF
        pkt = bytes([0xF2, 0x09, 0x00]) + (a).to_bytes(4, 'little') + ln.to_bytes(4, 'little')
        pkt += bytes([d.cksum(pkt)])
        s.plain_raw(pkt)
        time.sleep(0.010)
        for _ in range(30):
            t, b = s.read_frame(cap=2048)
            if t is None:
                return
            yield t, b
            time.sleep(0.005)
    return g

dump("0xF2 mem read @0x20007f14 len=48", mr(0x20007f14, 48))
dump("0xF2 mem read @0x20005000 len=48", mr(0x20005000, 48))
dump("0xF2 mem read @0x20000000 len=48", mr(0x20000000, 48))

os.close(s.fd)
