#!/usr/bin/env python3
"""Replicates the driver's full init sequence (config upload -> enable ->
request TLS) and then tests whether 0xF2 reads real memory. The 0xF2 behaviour
may depend on the command gate being open, which the config upload unlocks."""
import sys, os, time, struct
from importlib.machinery import SourceFileLoader
here = os.path.dirname(os.path.abspath(__file__))
d = SourceFileLoader("diag", os.path.join(here, "diag-sensor.py")).load_module()

CFG = open(os.path.join(here, "config_pcap.bin"), "rb").read()

GX_ENABLE = bytes([0x96, 0x03, 0x00, 0x01, 0x00, 0x10])
GX_REQTLS = bytes([0xd0, 0x03, 0x00, 0x00, 0x00, 0xd7])

s = d.Sensor('/dev/spidev1.0')

def read_body(timeout_reads=12, cap=4096):
    time.sleep(0.010)
    for _ in range(timeout_reads):
        t, b = s.read_frame(cap=cap)
        if b is not None:
            return t, b
        time.sleep(0.005)
    return None, None

def f2(addr, length):
    a = (addr - 0x08000000) & 0xFFFFFFFF
    pkt = bytes([0xF2, 0x09, 0x00]) + a.to_bytes(4, 'little') + length.to_bytes(4, 'little')
    pkt += bytes([d.cksum(pkt)])
    s.plain_raw(pkt)
    time.sleep(0.010)
    s.read_frame()                       # ACK
    return read_body()[1]

print("fw:", s.fw_version())

print("\n=== BEFORE config upload: does 0xF2 vary with address? ===")
a1 = f2(0x20000000, 48)
a2 = f2(0x20007f14, 48)
print("  0x20000000:", a1.hex() if a1 else None)
print("  0x20007f14:", a2.hex() if a2 else None)

# --- config upload, as gx_upload_config_and_reqtls does ------------------
print("\n=== uploading config (0x90, 256 bytes) ===")
cn = len(CFG) + 1
frame = bytes([0x90, cn & 0xFF, (cn >> 8) & 0xFF]) + CFG
frame += bytes([d.cksum(frame)])
s.write_frame(0xA0, d.AMORCE)
time.sleep(0.015)
s.write_frame(0xA0, frame)
time.sleep(0.080)
t, b = read_body()
print("  config ACK:", b.hex() if b else None)

print("\n=== enable (0x96) ===")
s.plain_raw(GX_ENABLE)
time.sleep(0.020)
t, b = read_body()
print("  enable ACK:", b.hex() if b else None)

print("\n=== request TLS (0xD0) ===")
s.plain_raw(GX_REQTLS)
time.sleep(0.020)
t, b = read_body()
print("  reqtls ACK:", b.hex() if b else None)

print("\n=== AFTER config upload: does 0xF2 vary with address? ===")
b1 = f2(0x20000000, 48)
b2 = f2(0x20007f14, 48)
b3 = f2(0x20010000, 48)
for lbl, v in (("0x20000000", b1), ("0x20007f14", b2), ("0x20010000", b3)):
    print(f"  {lbl}: {v.hex() if v else None}")
if b1 and b2 and b3:
    print("  payloads equal:", b1[11:-1] == b2[11:-1] == b3[11:-1])

os.close(s.fd)
