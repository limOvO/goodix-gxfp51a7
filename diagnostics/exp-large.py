#!/usr/bin/env python3
"""Decisive test: do large 0xF2 reads return real, address-dependent memory?

If they do, we can scan RAM for the 48-byte PSK (which this unit needs because
it runs firmware GF_ST411SEC_APP_14115, newer than the driver's APP_14003 and
therefore at an unknown PSK address). If they do not, 0xF2 is unusable here.
"""
import sys, os, time
from importlib.machinery import SourceFileLoader
here = os.path.dirname(os.path.abspath(__file__))
d = SourceFileLoader("diag", os.path.join(here, "diag-sensor.py")).load_module()


def parse(body):
    """Reply: [cmd][len16][addr4][len4][reqcksum][data...][cksum] -> data."""
    return body[12:-1]


s = d.Sensor('/dev/spidev1.0')
print("fw:", s.fw_version())

def read(addr, length):
    a = (addr - 0x08000000) & 0xFFFFFFFF
    pkt = bytes([0xF2, 0x09, 0x00]) + a.to_bytes(4, 'little') + length.to_bytes(4, 'little')
    pkt += bytes([d.cksum(pkt)])
    s.plain_raw(pkt)
    time.sleep(0.012)
    s.read_frame()                      # ACK frame
    time.sleep(0.010)
    t, b = s.read_frame(cap=8192)
    return parse(b) if b else None

print("\n--- 512 bytes at four addresses ---")
results = {}
for addr in (0x08000000, 0x08020000, 0x20000000, 0x20007f00):
    r = read(addr, 512)
    results[addr] = r
    print(f"  0x{addr:08x}: {'FAILED' if r is None else str(len(r)) + ' bytes'}")
    if r:
        print(f"    {r[:48].hex()}")

print("\n--- pairwise comparison ---")
addrs = list(results)
for i in range(len(addrs)):
    for j in range(i + 1, len(addrs)):
        a, b = results[addrs[i]], results[addrs[j]]
        if a and b:
            same = sum(1 for x, y in zip(a, b) if x == y)
            print(f"  0x{addrs[i]:08x} vs 0x{addrs[j]:08x}: "
                  f"{same}/{min(len(a),len(b))} bytes equal")

print("\n--- distinct byte values in each 512-byte read ---")
for addr, r in results.items():
    if r:
        print(f"  0x{addr:08x}: {len(set(r))} distinct of 256, "
              f"len={len(r)}")

os.close(s.fd)
