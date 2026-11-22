#!/bin/bash
# Recovers a wedged GXFP51A7 sensor.
#
# A GPIO reset pulse does not flush the sensor's TX queue, so stale frames
# survive it. What actually clears a desynchronised state is rebinding the
# spidev driver, and for a deeper lock-up pulsing the reset line as well.
# Run as root.
set -euo pipefail

PREFIX=/opt/libfprint-tod
SENSOR=spi-GXFP51A7:00
CHIP=/dev/gpiochip0
# GXFP51A7 reset: gpiochip0 line 264, ACTIVE HIGH (assert 1, release 0).
# Line 58 is the GXFP5187's reset and an unrelated pad on this board.
RST_LINE=264

say() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
[ "$(id -u)" -eq 0 ] || { echo "run this with sudo" >&2; exit 1; }

say "Stopping fprintd"
systemctl stop fprintd 2>/dev/null || true
sleep 1

say "Unbinding spidev from $SENSOR"
echo "$SENSOR" > /sys/bus/spi/drivers/spidev/unbind 2>/dev/null || true
sleep 1

say "Reset pulse on $CHIP line $RST_LINE (short, not held)"
if command -v gpioset >/dev/null 2>&1; then
  if gpioset --version 2>/dev/null | grep -qE 'v?2\.'; then
    gpioset -c "$CHIP" -t 0 "$RST_LINE=1" || echo "  gpioset (assert) failed" >&2
    gpioset -c "$CHIP" -t 0 "$RST_LINE=0" || echo "  gpioset (release) failed" >&2
  else
    gpioset "$CHIP" "$RST_LINE=1" || echo "  gpioset (assert) failed" >&2
    gpioset "$CHIP" "$RST_LINE=0" || echo "  gpioset (release) failed" >&2
  fi
else
  echo "  gpioset not installed (libgpiod); the rebind below is what matters" >&2
fi
sleep 1

say "Rebinding spidev"
modprobe spidev
echo spidev > "/sys/bus/spi/devices/$SENSOR/driver_override"
echo "$SENSOR" > /sys/bus/spi/drivers/spidev/bind 2>/dev/null || true
sleep 1

# The cached PMK is only a guess until a handshake accepts it, and a reset
# discards the boot-staged record it was derived from. Dropping the cache makes
# the driver re-derive it from the factory record on the next open, which is the
# right move after a hard recovery.
if [ -f /var/lib/fprint/.goodixtls-pmk ]; then
  say "Dropping the cached PMK so it is re-derived"
  rm -f /var/lib/fprint/.goodixtls-pmk
fi

say "Restarting fprintd"
systemctl start fprintd 2>/dev/null || true
sleep 2

echo
echo "Devices:"
dbus-send --system --print-reply --dest=net.reactivated.Fprint \
  /net/reactivated/Fprint/Manager net.reactivated.Fprint.Manager.GetDevices 2>/dev/null || true
