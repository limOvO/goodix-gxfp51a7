#!/usr/bin/env python3
"""Determines whether 0xF2 actually reads memory on this firmware
(GF_ST411SEC_APP_14115), by varying the requested length."""
import sys, os, time
from importlib.machinery import SourceFileLoader
here = os.path.dirname(os.path.abspath(__file__))
d = SourceFileLoader("diag", os.path.join(here, "diag-sensor.py")).load_module()

s = d.Sensor('/dev/spidev1.0')

def f2(addr, length):
    a = (addr - 0x08000000) & 0xFFFFFFFF
    pkt = bytes([0xF2, 0x09, 0x00]) + a.to_bytes(4, 'little') + length.to_bytes(4, 'little')
    pkt += bytes([d.cksum(pkt)])
    s.plain_raw(pkt)
    time.sleep(0.010)
    s.read_frame()
    time.sleep(0.008)
    for _ in range(10):
        t, b = s.read_frame(cap=4096)
        if b is not None:
            return b
        time.sleep(0.005)
    return None

print("fw:", s.fw_version())
print("\n len | reply body length | data-after-8 | first bytes")
for ln in (8, 16, 24, 40, 48, 64, 96, 128):
    b = f2(0x20007f14, ln)
    if b is None:
        print(f" {ln:3d} | no reply")
        continue
    data = b[11:-1] if len(b) > 12 else b''
    print(f" {ln:3d} | {len(b):3d} | {len(data):3d} | {data[:16].hex()}")

print("\n--- same address twice at len=128 (deterministic?) ---")
x = f2(0x20007f14, 128)
y = f2(0x20007f14, 128)
print("  equal:", x == y)
print("  data len:", len(x[11:-1]) if x else None)

print("\n--- two very different addresses at len=128 ---")
p = f2(0x20000000, 128)
q = f2(0x20010000, 128)
if p and q:
    pd, qd = p[11:-1], q[11:-1]
    print("  len p/q:", len(pd), len(qd))
    print("  first 32 p:", pd[:32].hex())
    print("  first 32 q:", qd[:32].hex())
    print("  payloads identical:", pd == qd)

os.close(s.fd)
