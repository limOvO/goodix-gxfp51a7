# Goodix GXFP51A7 fingerprint sensor on Linux (Huawei MateBook 13 WRT-WX9)

Native libfprint/fprintd support for the power-button fingerprint sensor in this
laptop, which upstream libfprint does not cover.

**Read [`REPORT.md`](REPORT.md) for the full write-up** — hardware, the four
blockers and how each was diagnosed, verification evidence, and remaining work.

## Quick start

```bash
./bootstrap.sh          # clone upstream sources at the right revisions + patch
./toolchain/setup.sh    # meson/ninja + glib code generators
./build.sh              # build into ./stage
sudo ./install.sh       # install
fprintd-enroll -f right-index-finger   # press ~20x
```

Status on this machine: the sensor is detected, opens, negotiates TLS, captures
frames and enrols presses. Enrolment needs a finger on the sensor, so the final
end-to-end confirmation is a manual step (see **Verify** below).

## Hardware

| | |
|---|---|
| Laptop | Huawei MateBook 13 2019, `WRT-WX9` |
| Sensor | Goodix `GXFP51A7` on SPI (ACPI `\_SB.PCI0.SPI1.SPBA`) |
| Firmware | `GF_ST411SEC_APP_14115` |
| Reset line | `gpiochip0` line **264**, active-high |
| Geometry | 132×112, 12-bit samples, ~22 kB per frame |

## How it fits together

Stock Arch `libfprint` cannot load out-of-tree drivers. So this installs a
private TOD build of libfprint under `/opt/libfprint-tod` and points **only
fprintd** at it, through a systemd drop-in. The system's own libfprint is left
untouched and every other consumer keeps using it.

```
sensor --spidev--> libfprint-tod (/opt/libfprint-tod)
                     └── libgoodixtls-tod-driver.so
                            └── fprintd (LD_LIBRARY_PATH via drop-in)
                                   └── PAM / GNOME Settings / fprintd-*
```

## What was needed beyond the upstream MilanL driver

The driver used here is the `gxfp51a7` branch of
[Sigfrodr/libfprint-goodixtls](https://github.com/Sigfrodr/libfprint-goodixtls),
which already knows this ACPI id. This revision of the hardware
(`APP_14115`) differs from what that branch expects, and four fixes were needed:

1. **The PSK is not readable from RAM.** Every `0xF2` RAM read returns the same
   boot-staged factory record, so the handshake died with
   `cipher operation failed` — the same symptom as a wrong key address. The key
   is that record, AES-128-CBC wrapped, and is unwrapped with
   `SHA256(salt ‖ zeros(48) ‖ goodix_seed)[:16]`, where the record's first byte
   is unreliable and is brute-forced against the `00 0d 00 00 00 30` marker.
   The result is cached in `/var/lib/fprint/.goodixtls-pmk` and re-validated by
   the TLS handshake on every open. (Derived from the GXFP51A0 bring-up, which
   targets the same firmware.)

2. **`0xF2` replies carry a 9-byte echo** (`addr32 ‖ len32 ‖ request cksum`)
   that is counted inside the requested length. Reading data at offset 3 handed
   TLS the echo instead of the key; the reply layout is now detected rather than
   assumed.

3. **Capture recipe.** The reference driver's *finger* recipe (finger-mode FDT
   zones, NOP, arm via `0x32`) makes this panel answer with a `0xD0` TLS
   reconnect and no frame. The short recipe (MCU state → FDT boot → NAV → FDT
   background → image) works for both background and a pressed finger, so it is
   used for both.

4. **No routine reset.** Pulsing line 264 discards the boot-staged state the
   factory PMK depends on, and the firmware version must be read *before* any
   reset. Resets are now skipped for this firmware; the reset helper is only for
   recovering a wedged sensor.

Also: ACPI `_CRS` discovery decodes **line 56** on this board, which is an
unrelated pad. The model's verified line (264) is used directly instead.

### Resilience

Because the key lives in boot-staged state, an unlucky sequence can lose it. Two
guards were added after that happened in practice:

- **A failed firmware read no longer means "older firmware".** It used to fall
  through to the RAM-key path, which cannot work here, *and* to the reset that
  discards the boot-staged key — so one transient read failure left the device
  unusable until manual recovery. If the version is unreadable but a cached key
  exists, the driver assumes 14115 and skips the reset.
- **The key recovers itself.** The factory-record read retries (draining the
  bus, and from the third attempt reopening the spidev node), and if a *cached*
  key is rejected by three handshakes it is re-derived from the factory record.
  Measured: recovery on a deliberately desynchronised bus produces a key
  byte-identical to the original.

### An upstream TLS bug this work also fixes

The patch additionally corrects a real defect in `goodix_tls.c`: CBC record
padding was validated only on its final byte, where TLS 1.2 requires all of it
to match. A record whose last ciphertext byte was flipped could therefore be
accepted — a 1-in-256 event, which is what made the driver's own TLS test flake
(~1 run in 30, reproducible on pristine upstream). With the check added, that
test passes 150/150.

## Speed — attempted, reverted

Capture was retuned from ~900 ms to ~649 ms (drain silence, inter-command gap, and
a timing multiplier stuck at 150%). **All of it is reverted.**

The retuned values were only ever checked against *background* captures. With a
finger down the sensor answers the image request with a 0xD0 TLS-reconnect
instead of a frame, so recognition stopped working entirely:

| setting | background | finger |
|---|---|---|
| drain 4, gap 30 ms | works | works |
| drain 3, gap 25 ms | works, ~1.4x faster | 0xD0, no frame |

On this part the conservative timings are load bearing. The rule this produced:
only a capture *with a finger on the sensor* validates a protocol change, since
that path behaves differently from the background path.

## Build

Arch does not ship the TOD headers, and `glib-mkenums` lives in `glib2-devel`,
so the toolchain is set up locally rather than installed system-wide:

```bash
./toolchain/setup.sh     # meson/ninja in a venv + glib code generators
./build.sh               # libfprint-tod into ./stage, then the driver
```

`build.sh` also runs the driver's TLS unit tests.

## Install

```bash
sudo ./install.sh
```

This installs `fprintd` if missing, the TOD libfprint and the driver into
`/opt/libfprint-tod`, the spidev buffer/autoload settings, the udev rule, and
the fprintd drop-in.

## Verify

Enrolment needs a finger on the sensor, so this is interactive:

```bash
fprintd-enroll -f right-index-finger
```

Press the sensor and lift, about 20 times, varying position slightly. Then:

```bash
fprintd-verify -f right-index-finger
```

GNOME Settings → Users → Fingerprint Login does the same through fprintd.

Note each stage waits ~30 s for a press; if you pause longer, enrolment stops
and can simply be restarted. Presses are only counted when a finger is actually
detected.

## Enable for login / sudo

Optional. The password always stays available as a fallback:

```bash
echo 'auth sufficient pam_fprintd.so' | sudo tee -a /etc/pam.d/sudo   # first line
```

## Recovery

If the sensor stops answering:

```bash
sudo /opt/libfprint-tod/gx-reset /dev/gpiochip0 264
sudo sh -c 'echo spi-GXFP51A7:00 > /sys/bus/spi/drivers/spidev/unbind
            sleep 1; modprobe spidev
            echo spidev > /sys/bus/spi/devices/spi-GXFP51A7:00/driver_override
            echo spi-GXFP51A7:00 > /sys/bus/spi/drivers/spidev/bind'
```

`recover.sh` does both.

## Uninstall

```bash
sudo ./install.sh --uninstall
```

Removes the driver, drop-in, udev rule and module settings. Enrolled prints are
kept; the script prints the paths to erase them.

## Security notes

This sensor streams images to the host and matching runs on the CPU, so it is
not match-on-chip: root on the machine can bypass it, and there is no liveness
detection. It is a convenience factor, not a sole defence. The TLS channel is
obfuscation, not a security boundary — the key is readable from the device.

## Diagnostics

`diagnostics/` holds the read-only probes used to reverse-engineer the
`APP_14115` differences (firmware id, PSK staging record, FDT replies, frame
layout). They are kept because they document how each conclusion above was
reached, and they are handy if a future firmware revision changes behaviour.
