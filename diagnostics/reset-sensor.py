#!/usr/bin/env python3
"""Resets the GXFP51A7 sensor and rebinds spidev. Run as root.

Needed because a wedged sensor answers nothing at all; the driver's own
recovery needs the reset line, and libgpiod's gpioset is not installed here.

Reset line on this board (MateBook 13 WRT-WX9): gpiochip0 line 264, ACTIVE HIGH
(drive 1 to assert, 0 to release), short pulse only.
"""
import ctypes, fcntl, os, struct, subprocess, sys, time

GPIO_V2_GET_LINE_IOCTL = 0xC250B40F
GPIO_V2_LINE_SET_VALUES_IOCTL = 0xC010B40E
GPIO_V2_LINE_FLAG_OUTPUT = 2
GPIO_V2_LINE_FLAG_ACTIVE_LOW = 4

SENSOR = "spi-GXFP51A7:00"
CHIP = "/dev/gpiochip0"
LINE = 264


class LineRequest(ctypes.Structure):
    _fields_ = [("flags", ctypes.c_uint32), ("num_lines", ctypes.c_uint32),
                ("consumer", ctypes.c_char * 32),
                ("offsets", ctypes.c_uint32 * 64),
                ("output_values", ctypes.c_uint64)]


class LineValues(ctypes.Structure):
    _fields_ = [("bits", ctypes.c_uint64), ("mask", ctypes.c_uint64)]


def write(path, val):
    try:
        with open(path, "w") as f:
            f.write(val)
    except OSError as e:
        print(f"  {path}: {e}")


def pulse(line, hold_ms=10):
    fd = os.open(CHIP, os.O_RDWR | os.O_CLOEXEC)
    req = LineRequest()
    req.flags = GPIO_V2_LINE_FLAG_OUTPUT
    req.num_lines = 1
    req.consumer = b"gx-reset"
    req.offsets[0] = line
    fcntl.ioctl(fd, GPIO_V2_GET_LINE_IOCTL, req)
    linereq_fd = req.offsets  # kernel fills this union; see note below

    # The kernel returns the line-request fd in the same struct slot the
    # offsets array starts at. Read it back from a fresh struct.
    buf = bytearray(ctypes.sizeof(LineRequest))
    req2 = LineRequest()
    req2.flags = GPIO_V2_LINE_FLAG_OUTPUT
    req2.num_lines = 1
    req2.consumer = b"gx-reset"
    req2.offsets[0] = line
    fcntl.ioctl(fd, GPIO_V2_GET_LINE_IOCTL, bytearray(bytes(req2)))

    # Simpler and reliable: use libgpiod v2 through the C API is unavailable,
    # so drive the line via the debugfs-free v2 interface using a small helper
    # is overkill. Instead toggle through the character device using the
    # kernel's returned fd, obtained with a correctly-sized struct.
    os.close(fd)


def pulse_v2(line, hold_ms=10):
    """Requests the line as output and pulses it, using the exact ioctl ABI."""
    import array
    fd = os.open(CHIP, os.O_RDWR | os.O_CLOEXEC)
    # struct gpio_v2_line_request: flags u32, num_lines u32, consumer[32],
    # offsets[u32], output_values u64, padding, fd s32 at the end.
    size = 4 + 4 + 32 + 64 * 4 + 8 + 4 + 4
    buf = bytearray(size)
    struct.pack_into("<II", buf, 0, GPIO_V2_LINE_FLAG_OUTPUT, 1)
    buf[8:8 + 9] = b"gx-reset\0"
    struct.pack_into("<I", buf, 40, line)
    try:
        fcntl.ioctl(fd, GPIO_V2_GET_LINE_IOCTL, buf)
    except OSError as e:
        print(f"  get_line failed: {e}")
        os.close(fd)
        return False
    lfd = struct.unpack_from("<i", buf, size - 4)[0]

    def setval(b):
        v = bytearray(16)
        struct.pack_into("<QQ", v, 0, b, 1)
        fcntl.ioctl(lfd, GPIO_V2_LINE_SET_VALUES_IOCTL, v)

    setval(1)                      # assert (active high)
    time.sleep(hold_ms / 1000.0)
    setval(0)                      # release
    time.sleep(0.12)
    os.close(lfd)
    os.close(fd)
    return True


def main():
    print("stopping fprintd")
    subprocess.run(["systemctl", "stop", "fprintd"], check=False)
    time.sleep(1)

    print(f"unbinding {SENSOR}")
    write("/sys/bus/spi/drivers/spidev/unbind", SENSOR + "\n")
    time.sleep(0.5)

    print(f"pulse {CHIP} line {LINE} (active high)")
    if pulse_v2(LINE):
        print("  pulse done")
    else:
        print("  pulse FAILED")

    print("rebinding spidev")
    subprocess.run(["modprobe", "spidev"], check=False)
    write(f"/sys/bus/spi/devices/{SENSOR}/driver_override", "spidev\n")
    write("/sys/bus/spi/drivers/spidev/bind", SENSOR + "\n")
    time.sleep(0.5)

    nodes = [n for n in os.listdir("/dev") if n.startswith("spidev")]
    print("  spidev nodes:", nodes)


main()
