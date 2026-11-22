#!/bin/bash
# Installs the Goodix GXFP51A7 (MilanL) fingerprint driver for libfprint/fprintd.
#
# Design: the TOD build of libfprint is kept OUT of the system library path
# (/opt/libfprint-tod) and used only by fprintd, through a systemd drop-in.
# Stock libfprint is left untouched, so nothing else on the system changes and
# the whole thing is undone by `install.sh --uninstall`.
#
#   sudo ./install.sh              install
#   sudo ./install.sh --uninstall  remove everything except enrolled prints
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
STAGE="$HERE/stage"
PREFIX=/opt/libfprint-tod
TOD_DIR="$PREFIX/lib/libfprint-2/tod-1"
DROPIN=/etc/systemd/system/fprintd.service.d/goodixtls.conf
UDEV_RULE=/etc/udev/rules.d/60-libfprint-2-goodixtls.rules

say()  { printf '\n\033[1m== %s\033[0m\n' "$*"; }
warn() { printf '\033[33m!! %s\033[0m\n' "$*"; }
die()  { printf '\033[31m!! %s\033[0m\n' "$*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "run this with sudo"

# --- uninstall --------------------------------------------------------
if [ "${1:-}" = "--uninstall" ]; then
  say "Removing driver and runtime wiring"
  systemctl stop fprintd 2>/dev/null || true
  rm -f "$DROPIN"
  systemctl daemon-reload
  rm -f "$UDEV_RULE"
  rm -f /etc/modprobe.d/goodixtls-spidev.conf
  rm -f /etc/modules-load.d/goodixtls-spidev.conf
  udevadm control --reload 2>/dev/null || true
  rm -rf "$PREFIX"
  echo
  echo "Done. Enrolled fingerprints were kept in /var/lib/fprint."
  echo "To erase them too:"
  echo "  rm -rf /var/lib/fprint/*/goodixtls /var/lib/fprint/.goodixtls-*"
  exit 0
fi

[ -f "$STAGE/drivers/libgoodixtls-tod-driver.so" ] || \
  die "build output missing; run ./build.sh first"

SENSOR=spi-GXFP51A7:00
[ -d "/sys/bus/spi/devices/$SENSOR" ] || \
  die "sensor $SENSOR not present (is this a MateBook 13 WRT-WX9?)"

# --- 1. packages ------------------------------------------------------
say "Packages"
for p in fprintd; do
  if pacman -Q "$p" >/dev/null 2>&1; then
    echo "$p already installed"
  else
    pacman -S --needed --noconfirm "$p"
  fi
done
# Stock libfprint is expected (fprintd depends on it); it stays in place and is
# simply shadowed for fprintd by the drop-in below.

# --- 2. TOD libfprint + driver module --------------------------------
say "Installing libfprint-tod and the driver into $PREFIX"
systemctl stop fprintd 2>/dev/null || true
rm -rf "$PREFIX"
mkdir -p "$PREFIX/lib" "$TOD_DIR"
# Library plus its symlink chain.
cp -a "$STAGE/usr/lib/"libfprint-2*.so* "$PREFIX/lib/"
# The driver module: libfprint looks here (FP_TOD_DRIVERS_DIR points at it).
install -m 0755 "$STAGE/drivers/libgoodixtls-tod-driver.so" "$TOD_DIR/"
# Reset helper: recovers a wedged sensor (real GPIO pulse + spidev rebind).
install -m 0755 "$HERE/gx-reset" "$PREFIX/gx-reset"

# --- 3. spidev: buffer size and autoload -----------------------------
# The sensor sends a single ~22 kB SPI frame; spidev's default 4096-byte
# buffer silently truncates it, which is the classic "capture fails" cause.
say "Configuring spidev"
cat > /etc/modprobe.d/goodixtls-spidev.conf <<'EOF'
# Goodix SPI fingerprint sensor: image frames are ~22 kB, above spidev's
# 4096-byte default. Must be raised or transfers are truncated.
options spidev bufsiz=65536
EOF
# spidev has no ACPI alias for this hardware, so nothing loads it on its own.
echo spidev > /etc/modules-load.d/goodixtls-spidev.conf

if [ -d /sys/module/spidev ]; then
  modprobe -r spidev 2>/dev/null || \
    warn "spidev is in use; the buffer size takes effect after a reboot"
fi
modprobe spidev bufsiz=65536 2>/dev/null || true

# --- 4. udev rule and immediate bind --------------------------------
say "Binding the sensor to spidev"
install -m 0644 "$STAGE/60-libfprint-2-goodixtls.rules" "$UDEV_RULE"
udevadm control --reload
udevadm trigger --subsystem-match=spi --action=add 2>/dev/null || true

echo spidev > "/sys/bus/spi/devices/$SENSOR/driver_override"
if [ -e "/sys/bus/spi/devices/$SENSOR/driver" ]; then
  echo "already bound to $(basename "$(readlink -f "/sys/bus/spi/devices/$SENSOR/driver")")"
elif ! echo "$SENSOR" > /sys/bus/spi/drivers/spidev/bind 2>/tmp/gx-bind.err; then
  warn "cannot bind $SENSOR to spidev: $(cat /tmp/gx-bind.err)"
fi
rm -f /tmp/gx-bind.err

if [ "$(cat /sys/module/spidev/parameters/bufsiz 2>/dev/null || echo 0)" -lt 65536 ]; then
  warn "spidev bufsiz is still below 65536; a reboot will apply it"
fi

NODE=$(ls /dev/spidev* 2>/dev/null | head -1 || true)
if [ -n "$NODE" ]; then
  echo "spidev node ready: $NODE"
else
  warn "no /dev/spidev* node appeared"
fi

# --- 5. fprintd drop-in ---------------------------------------------
# fprintd must load the TOD libfprint (it is the only thing that can host an
# out-of-tree driver) and needs GPIO access for the sensor's hardware reset.
say "Configuring fprintd"
mkdir -p "$(dirname "$DROPIN")"
cat > "$DROPIN" <<EOF
[Service]
# Run the daemon against the TOD libfprint in $PREFIX instead of the stock one.
Environment=LD_LIBRARY_PATH=$PREFIX/lib
Environment=FP_TOD_DRIVERS_DIR=$TOD_DIR
# Stay resident: otherwise a wake from sleep offers only the password while
# the service is being started.
ExecStart=
ExecStart=/usr/lib/fprintd --no-timeout
# The driver pulses the sensor's reset line; systemd's device policy denies
# /dev/gpiochip0 without this even to root.
DeviceAllow=/dev/gpiochip0 rw
RuntimeDirectory=goodixtls
RuntimeDirectoryMode=0755
RuntimeDirectoryPreserve=yes
EOF
systemctl daemon-reload

# --- 6. start and verify -------------------------------------------
say "Starting fprintd"
systemctl restart fprintd
sleep 2

echo "Devices reported by fprintd:"
dbus-send --system --print-reply --dest=net.reactivated.Fprint \
  /net/reactivated/Fprint/Manager net.reactivated.Fprint.Manager.GetDevices 2>/dev/null || \
  warn "could not query fprintd over D-Bus"

cat <<EOF

If the list above mentions GXFP51A7/goodixtls, the sensor is live.

Next steps (interactive, needs a finger on the sensor):
    fprintd-enroll -f right-index-finger     # or GNOME Settings -> Users

Enable fingerprint for login / sudo only if you want it (the password stays
available as a fallback):
    auth sufficient pam_fprintd.so    # first line of /etc/pam.d/sudo

If the sensor later stops responding:
    sudo $HERE/recover.sh
EOF
