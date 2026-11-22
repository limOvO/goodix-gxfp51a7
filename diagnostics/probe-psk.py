#!/usr/bin/env python3
"""Focused PSK probe: read 48-byte windows and check for the documented
"wrong address" signature (leading zeros = key misaligned or absent)."""
import sys, struct, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from importlib.machinery import SourceFileLoader
mod = SourceFileLoader("diag", os.path.join(os.path.dirname(os.path.abspath(__file__)), "diag-sensor.py"))
d = mod.load_module()

s = d.Sensor('/dev/spidev1.0')

print("fw:", s.fw_version())

# Determinism check: same address twice must match.
a = s.mem_read(0x20007f14, 48)
b = s.mem_read(0x20007f14, 48)
print("\n0x20007f14 read #1:", a.hex() if a else None)
print("0x20007f14 read #2:", b.hex() if b else None)
print("deterministic:", a == b)

# Documented correct address for 14003, and the 5187 ones.
for addr, label in [
    (0x20007f0c, "GXFP5187/11033"),
    (0x20007f14, "GXFP51A7/14003 (driver default)"),
    (0x20012d68, "GXFP5187/11034"),
]:
    v = s.mem_read(addr, 48)
    if v:
        print(f"\n{label} @0x{addr:08x}:")
        print("  hex :", v.hex())
        print("  uniq:", len(set(v)), "leading-zero bytes:", len(v) - len(v.lstrip(b'\0')))
    else:
        print(f"\n{label} @0x{addr:08x}: READ FAILED")

os.close(s.fd)
