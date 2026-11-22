#!/usr/bin/env python3
"""Tests whether 0xF2 memory reads honour the address at all, across several
address encodings, so we know if a RAM scan for the PSK is even meaningful."""
import sys, os, time
from importlib.machinery import SourceFileLoader
here = os.path.dirname(os.path.abspath(__file__))
d = SourceFileLoader("diag", os.path.join(here, "diag-sensor.py")).load_module()

s = d.Sensor('/dev/spidev1.0')

def f2(addr_bytes, length=48, label=""):
    """Sends 0xF2 with raw address bytes, returns the payload after cmd/len."""
    pkt = bytes([0xF2, 0x09, 0x00]) + addr_bytes + length.to_bytes(4, 'little')
    pkt += bytes([d.cksum(pkt)])
    s.plain_raw(pkt)
    time.sleep(0.010)
    hdr = s.read_frame()          # ACK
    time.sleep(0.008)
    for _ in range(10):
        t, b = s.read_frame(cap=2048)
        if b is not None:
            print(f"  {label:<28} reply={b.hex()}")
            return b
        time.sleep(0.005)
    print(f"  {label:<28} (no reply)")
    return None

print("=== firmware:", s.fw_version(), "===")

print("\n--- encoding A: addr - 0x08000000 (driver's convention) ---")
for a in (0x20000000, 0x20007f14, 0x20005000, 0x20010000):
    f2(((a - 0x08000000) & 0xFFFFFFFF).to_bytes(4, 'little'), label=f"0x{a:08x}")

print("\n--- encoding B: addr as-is ---")
for a in (0x20000000, 0x20007f14):
    f2(a.to_bytes(4, 'little'), label=f"raw 0x{a:08x}")

print("\n--- encoding C: flash base 0x08000000 -> 0 ---")
f2((0).to_bytes(4, 'little'), label="0x08000000 (flash)")

print("\n--- encoding D: accept-outer addresses ---")
for a in (0x00000000, 0x10000000, 0x30000000):
    f2(((a - 0x08000000) & 0xFFFFFFFF).to_bytes(4, 'little'), label=f"0x{a:08x}")

os.close(s.fd)
