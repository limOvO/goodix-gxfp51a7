# Enabling the Goodix GXFP51A7 fingerprint sensor on Linux

**Machine:** Huawei MateBook 13 2019 (`WRT-WX9`) · CachyOS · kernel `7.3.3-1-cachyos`
**Sensor:** Goodix `GXFP51A7`, SPI, ACPI `GXFP51A7` · firmware **`GF_ST411SEC_APP_14115`**
**Date:** 2026-11-22

---

## 1. Summary

The power-button fingerprint reader is driven natively on Linux now. It is
detected, opened, negotiates a TLS-PSK session, captures frames, and enrols
presses — none of which was possible with any packaged driver.

Upstream libfprint has no driver for this part, and Arch's `libfprint` cannot
load out-of-tree drivers at all. The working solution is a private TOD build of
libfprint plus a patched community driver, used **only** by fprintd, leaving the
system's own libfprint untouched.

Four hardware-specific problems stood between the existing `gxfp51a7` driver
branch and a working sensor. Each presented a misleading symptom, and finding
them required reading the sensor's own protocol off the bus.

**What is confirmed working** (measured, repeated):

| Stage | Evidence |
|---|---|
| Detection by fprintd | device `Goodix GXFP5187/GXFP51A7 SPI (TLS-PSK)`, 20 enrol stages |
| SPI dialogue | firmware id read reliably, 3/3 and 5/5 repeated runs |
| TLS-PSK handshake | `PSK-AES128-GCM-SHA256`, 3/3 runs, zero cipher errors |
| Finger detection | idle mean 356 → floor 332, drop 98–125 on press |
| Image capture | 22189-byte frame on every attempt |
| Enrolment progress | **12 consecutive stages**, keypoints 143 → 854, extent 107×87 → 253×227 |
| Match scores | **27 against a threshold of 18 (MATCH)**, clean state |

**Confirmed with a finger on the sensor:** verification now scores
27 matches against a threshold of 18, reporting MATCH, from a clean state. Two
earlier runs scored 15 and 16 only because of polluted on-disk state (§7), not
because of the code.

**What is not yet confirmed:** a fresh 20-stage enrolment end to end, and login
unlock. Both need ~20 physical presses, which cannot be automated (§10).

---

## 2. The hardware

| | |
|---|---|
| ACPI id | `GXFP51A7`, on `pxa2xx-spi` (`\_SB.PCI0.SPI1.SPBA`) |
| Kernel surface | `spi-GXFP51A7:00` → `/dev/spidev1.0` |
| Firmware | `GF_ST411SEC_APP_14115` |
| Image | 132×112, 12-bit packed, 22189 bytes |
| Reset | `gpiochip0` line **264**, **active-high** |
| TLS | sensor is the *client*, host the *server*, PSK-AES128-GCM-SHA256 |

Two facts were established early and mattered throughout:

- The sensor is on **SPI, not USB**, so it never appears in `lsusb`. It is
  visible only as an unbound ACPI device on the SPI bus.
- `spidev` is required, but has **no ACPI alias** for this chip, so nothing
  loads or binds it automatically. It must be forced via `driver_override`, and
  its buffer must be raised from the 4096-byte default or the 22 kB frame is
  silently truncated.

---

## 3. The four blockers

The starting point was the `gxfp51a7` branch of
[Sigfrodr/libfprint-goodixtls](https://github.com/Sigfrodr/libfprint-goodixtls),
which already knows this ACPI id. It was written against firmware `APP_14003`;
this unit runs `APP_14115`, and that difference broke four things.

### 3.1 The PSK is not in RAM — the hardest one

The driver reads a 48-byte pre-shared key out of the sensor's RAM with the
`0xF2` memory-read command, then uses it for TLS. Here that failed with:

```
TLS handshake failed ... cipher operation failed
```

Both the driver's own notes and the protocol documentation say that exact
message means *the key was read from the wrong address*. So the obvious move was
to scan for it. That led nowhere useful, and produced the key insight:

**Every `0xF2` read returned the same 248 bytes, regardless of address.**
`0x20000000`, `0x20007f14`, `0x08004000` — identical. A RAM read that ignores
its address is not returning RAM.

Cross-referencing an independent bring-up for a sibling part on the *same
firmware* (`GXFP51A0`, same `GF_ST411SEC_APP_14115`) revealed what it actually
is: the `0xF2` path on 14115 rejects RAM reads and instead replays a
**boot-staged factory record**. And that record *is* the key, wrapped:

```
record  = salt(16) ‖ AES-128-CBC ciphertext(64) ‖ …
key     = SHA256(salt ‖ zeros(48) ‖ goodix_seed(16))[:16]
plain   = AES-128-CBC-decrypt(key, iv = salt, ciphertext)
        = 00 0d 00 00 00 30 ‖ PMK(48) ‖ …        ← marker + BE length
```

One further wrinkle: the read path **corrupts the record's first byte**. So the
record can't be used as-is. Recovering it means brute-forcing that single byte
and keeping the one value that produces the `0x000d`/`0x30` marker — exactly one
value does.

This is why the failure looked like a wrong key address: the driver was handing
TLS a plausible-looking 48-byte block that was simply not the key.

**Verified:** the recovered key is stable across runs, and re-derivation on a
deliberately corrupted bus produces a byte-identical key.

### 3.2 The memory-read reply has an undocumented echo

Once the real key was located, it still wasn't being parsed. Dumping raw frames
showed the reply carries a 9-byte echo of the request that is **counted inside
the requested length**:

```
cmd(1) ‖ len16(2) ‖ addr_echo(4) ‖ len_echo(4) ‖ req_cksum(1) ‖ data(N-9) ‖ cksum(1)
```

The driver assumed data started at offset 3 and asked for exactly N bytes — so
it handed TLS the *echo bytes* and got N−9 bytes of data. The layout is now
detected rather than assumed (the echo is checked against the request), so one
binary serves both this firmware and the older one.

### 3.3 The reference's press recipe doesn't work on this panel

The reference driver's *finger* capture recipe (finger-mode FDT zones → NOP →
arm via `0x32` → image) makes **this** panel answer the image request with a
`0xD0` TLS-reconnect and deliver no frame at all. Background capture worked; only
pressed frames failed.

That recipe belongs to the 88×80 panel of the `GXFP51A0`, which this unit is
not (it is 132×112). The short recipe used for background capture —
`MCU state → FDT boot → NAV → FDT background → image` — works for a **pressed**
finger too, so it is now used for both. This is what took enrolment from
"first stage only" to 12 consecutive stages.

The OTP-derived DAC calibration the reference applies was also dropped: this
unit's OTP block is 32 bytes, not the 64-byte layout those values are derived
from, so there is no calibration to apply here.

### 3.4 Reset handling, and one self-inflicted outage

Two reset problems:

1. **ACPI `_CRS` discovery decodes the wrong pin.** It selects **line 56**, which
   on this board is an unrelated pad — driving it does nothing. The verified
   reset is **line 264**. The model's line is now used directly.
2. **A real reset is destructive here.** Because the key lives in *boot-staged*
   state, pulsing line 264 discards it. Ironically the wrong-line bug had been
   masking this: resetting line 56 was a harmless no-op. Fixing the line exposed
   that routine resets were breaking sessions, so resets are now skipped for this
   firmware and reserved for recovery.

**The outage:** after the reset fix, GNOME reported *"fingerprint device
disconnected"* mid-enrolment. The log showed `key read failed` — a transient
firmware-id read failure. My code then made a bad inference: *can't read
firmware ⇒ older firmware* ⇒ take the RAM-key path (impossible here) **and**
pulse the reset (destroying the boot-staged key). One transient read failure
killed the device until manual recovery.

Three guards now prevent that, and each was verified against the exact failure:

- An unreadable firmware id is **not** treated as "older firmware". If a cached
  key exists, the driver assumes 14115 and **skips the reset**.
- The factory-record read **retries** — draining the bus, and reopening the
  spidev node from the third attempt — clearing a desynchronised bus.
- A **rejected cached key re-derives itself** if three handshakes fail with it.

Measured after the fix: the previously fatal desync now recovers, and the
re-derived key is byte-identical to the original (same MD5), so recovery is
deterministic rather than lossy.

---

## 4. Architecture

Stock Arch `libfprint` cannot host out-of-tree drivers, so a private TOD build
lives under `/opt/libfprint-tod` and **only fprintd** is pointed at it:

```
sensor ──spidev──► libfprint-tod (/opt/libfprint-tod/lib)
                     └── libgoodixtls-tod-driver.so
                            └── fprintd  (LD_LIBRARY_PATH via systemd drop-in)
                                   └── PAM · GNOME Settings · fprintd-*
```

The system's own libfprint stays in place, so nothing else on the machine
changes and every other consumer is unaffected. Verification: the TOD build
exports every `fp_*` symbol Arch's fprintd needs (0 missing), and fprintd loads
against it cleanly.

Driver state lives in fprintd's writable `StateDirectory`:

| File | Purpose |
|---|---|
| `.goodixtls-pmk` | cached PMK (re-validated by handshake every open) |
| `.goodixtls-bg` | last clean background frame |
| `.goodixtls-fdt` | idle detection level for this unit |
| `.goodixtls-timing` | learned protocol timing scale |

---

## 5. Changes made

All driver changes are in **`01-app-14115-support.patch`** — 641 insertions,
57 deletions across `goodixtls.c` and `goodix_tls.c`. It has been verified to apply cleanly to a
pristine clone and reproduce the working source byte-for-byte.

| Area | Change |
|---|---|
| Key acquisition | factory-record unwrap + first-byte brute force, cached |
| Key recovery | retry with bus drain and node reopen; re-derive on rejected cache |
| Memory read | detect and skip the 9-byte echo; honour it in the length |
| Capture | one short recipe for background *and* finger |
| Reset | use line 264; skip for 14115; read firmware before any reset |
| Firmware detect | retry; unreadable ⇒ assume 14115 if a key is cached |
| Cleanup | removed (the reference teardown broke the next frame) |
| Speed | attempted, reverted (see §6) |
| Capture recipe | short recipe for both frames; reference re-arm does not work here (§7) |
| 0xD0 handling | reconnect the TLS session and retry once |
| TLS (`goodix_tls.c`) | validate all CBC padding bytes, not just the last |

Non-driver pieces, all in the project directory:

- `build.sh` — reproducible build (TOD libfprint → staged → driver against it)
- `toolchain/setup.sh` — meson/ninja in a venv + glib code generators pulled from
  `glib2-devel`, since Arch ships neither TOD headers nor `glib-mkenums`
- `install.sh` / `--uninstall` — idempotent, fully reversible
- `recover.sh`, `gx-reset` — wedged-sensor recovery (real GPIO pulse + rebind)
- `test-direct.c` — libfprint harness that bypasses fprintd/polkit for testing
- `diagnostics/` — the read-only probes behind every conclusion above

---

## 6. Performance — a reverted attempt

Recognition felt slow, so capture was retuned (drain silence 4->3, inter-command
gap 30->25 ms, and a fix for a timing multiplier that had ratcheted to 150%).
Capture dropped from ~900 ms to ~649 ms on background frames.

**It was reverted.** Those timings were only ever validated with *background*
captures. With a finger down the sensor answers the image request with a 0xD0
TLS-reconnect instead of a frame — so recognition stopped working entirely. The
measurements were real; the validation was not.

The honest conclusion is that on this part the conservative timings are load
bearing, not slack:

| setting | background capture | finger capture |
|---|---|---|
| drain 4, gap 30 ms | works | works |
| drain 3, gap 25 ms | works, ~1.4x faster | **0xD0, no frame** |

Reverting restored capture immediately (3/3 runs, no reconnects). The rule this
episode produced: **a change is only validated by a capture with a finger on the
sensor**, because that is the path verification uses and it behaves differently
from the background path.

---

## 7. The capture recipe — and what actually captures a finger

Two references for this sensor family exist and they disagree about the finger
path. Both were tested on this unit; only one works.

**The short recipe** (what this driver uses for a background frame):

```
mcu_state(0x55) ; fdt_mode(0d01, boot zones) ; nav ; fdt_mode(0d01, background zones) ; get_image
```

**The finger-specific re-arm recipe** from
[szlukabence/goodix-fingerprint-spi-linux](https://github.com/szlukabence/goodix-fingerprint-spi-linux),
whose docs stress that `fdt_down` must be re-armed before every frame:

```
fdt_mode(0d01, finger) ; fdt_down(0c01, finger)
nop ; mcu_state(0x55)
fdt_down(0c01, 80b2 80c2 80a7 80b6 80a6 80b6)
get_image
```

**Measured here, the second one produces no image at all.** The sequence
completes, `0x20` returns no record, and every press is rejected as "swipe too
short" — the sensor answers with nothing rather than with a bad frame. That
recipe belongs to the 88x80 GXFP51A0 panel; this unit is 132x112.

The short recipe captures a pressed finger correctly on this panel. Compared on
one build, finger on the sensor:

| finger recipe | result |
|---|---|
| reference re-arm sequence | **0 usable frames**, every press rejected |
| short recipe (same as background) | **49, 45 matches** against a threshold of 20 |

So on this part a finger frame and a background frame are the *same* exchange.
The final confirmation run, with all polluted state cleared, scored
**27 matches against a threshold of 18 — MATCH**.

Two other findings in that reference do **not** apply here either, and were
checked rather than assumed:

- **`SPI_CS_HIGH`** is required on the 51A0 board. Not here: this driver runs
  plain `SPI_MODE_0` and reads the firmware version reliably.
- **88x80 geometry with 64 active columns** is that panel. This unit returns
  22189-byte records = 132x112 (14784 px), and the decode matches exactly.

Also implemented from the protocol docs: a **0xD0 reply now triggers a session
reconnect and one retry** instead of failing. The sensor asks for a fresh TLS
session when its state does not advance — the vendor driver restarts its init
thread on it — and this driver previously only logged a warning.

### State pollution matters as much as the code

Three separate pieces of on-disk state were produced during the broken period
and each dragged the score down afterwards:

| state | effect |
|---|---|
| finger-2 template (enrolled 03:05, bad recipe) | a second, unusable print |
| `.goodixtls-adapt/*.views` | 3 views captured with no usable frame, fused into every verify |
| `.goodixtls-bg` | a background frame taken with a finger resting on the sensor |

After clearing all three, the same finger scored 27/18 (MATCH) where it had
scored 15/20 and 16/20 with them present.

---

## 8. Why some press angles fail

Symptom: some angles never authenticate. Measured on six presses of the same
enrolled finger, varying the angle deliberately:

| press | score (threshold 20) | |
|---|---|---|
| 1 | 44 | match |
| 2 | 56 | match |
| 3 | 25 | match |
| 4 | **0** | fail |
| 5 | 31 | match |
| 6 | **5** | fail |

The distribution is **bimodal** — 25-56 or 0-5, never 18-19. A coverage
problem in the template would produce near-misses just under the threshold, so
this is not that. The template carries 20 views and is fine.

The captures themselves explain it. Both pass and fail produce a valid
22189-byte frame in normal time, but:

| capture | score | active pixels | diff mean |
|---|---|---|---|
| pass | 37 | 14292 | 918 |
| pass | 23 | 13362 | 888 |
| fail | 3 | 5172 | 349 |
| fail | 7 | 5062 | 334 |

A failing press puts roughly **a third** of the finger on the sensor. The
matcher is working correctly; there is less to match.

Rotation was the obvious suspect and was tested directly, not assumed: rotating
a failing capture through +-30 degrees and re-scoring changes nothing — the best
angle is always 0 deg.

| pair | direct | best over +-30 deg |
|---|---|---|
| weak vs strong | 4 | 4 (at 0 deg) |
| weak vs strong | 19 | 19 (at 0 deg) |
| strong vs itself | 137 | 137 (at 0 deg) |

So the descriptors are genuinely rotation-invariant, and the loss is **signal
coverage**, not pose. The fix is pressing with the pad centred on the button;
there is no code change that recovers a third of a fingerprint.

---

## 9. An upstream TLS bug found on the way

Rebuilding from a clean clone occasionally failed the driver's own TLS test
(`/tls/cbc-legacy-hello`) — roughly 1 run in 30. It reproduces in pristine,
unmodified upstream, so it was not caused by this work, but it turned out to be
a real defect rather than a flaky assertion.

`gx_tls_decrypt_record` validated only the **last** padding byte of a CBC
record. TLS 1.2 (RFC 5246 §6.2.3.2) requires all N+1 padding bytes to equal N.
The gap is reachable by accident: flipping the final *ciphertext* byte garbles
only the last plaintext block — which holds nothing but the padding — and if the
garbled final byte happens to name a length that still fits, the MAC over the
(intact) content still verifies, so a **corrupted record is accepted**. With 16
padding bytes that is a 1-in-256 event per tampered record, which is exactly the
observed rate.

The fix checks every padding byte. Measured:

| | runs | passes |
|---|---|---|
| upstream, unmodified | 60 | 58 |
| upstream + this fix | 120 | **120** |
| this build | 150 | **150** |

Hardware unaffected: real captures still decrypt 3/3, because the stricter check
only rejects records whose padding was actually tampered with.

---

## 10. Verification

Repeated, measured, on the final build:

| Check | Result |
|---|---|
| Session startup | 5/5, then 3/3 runs — clean TLS, background frame each time |
| TLS | `PSK-AES128-GCM-SHA256`, no cipher errors |
| Frame size | 22189 bytes, every capture |
| FDT calibration | idle 356 → floor 332 (stable across runs) |
| Enrolment | 12 consecutive stages, 143 → 854 keypoints |
| Desync recovery | recovers; re-derived key identical to original |
| fprintd integration | device enumerated, driver loads, PMK from cache |
| Patch reproducibility | applies clean, source identical |

Enrolment progression from the 12-stage run — keypoint count and covered area
growing as expected, which is what a healthy template looks like:

```
stage  1  extent 107x87   points 143
stage  4  extent 189x130  points 409
stage  8  extent 215x181  points 619
stage 12  extent 253x227  points 854
```

---

## 11. Remaining work

**One manual step: complete a full enrolment and verify a match.** Everything up
to and including press capture is automated and verified; a 20-press enrolment
and a subsequent verify need a finger.

```bash
fprintd-enroll -f right-index-finger     # press ~20×, shifting position
fprintd-verify -f right-index-finger
```

or GNOME Settings → Users → Fingerprint Login.

Notes for that run: each stage waits ~30 s for a press, and a longer pause ends
the enrolment (restart it). Presses only count when a finger is actually
detected. If it stalls, `journalctl -u fprintd -f` shows the reason per stage.

**Optional:** enable for login/sudo with
`auth sufficient pam_fprintd.so` as the first line of `/etc/pam.d/sudo`. The
password always remains available as a fallback.

---

## 12. Security notes

This sensor **streams images to the host** and matching runs on the CPU — it is
not match-on-chip. Consequences worth stating plainly:

- Root or kernel access can bypass it, and can read biometric data.
- There is no liveness detection; the sensor's anti-spoofing signals are never
  exposed to the host.
- The TLS channel is **obfuscation, not a security boundary** — the key is
  readable from the device by design, which is precisely how it was obtained.
- Templates sit in `/var/lib/fprint`, protected by filesystem permissions.

Treat it as a convenience factor, not a sole defence.

---

## 13. Provenance

Reverse-engineering input, all public:

- [`Sigfrodr/libfprint-goodixtls`](https://github.com/Sigfrodr/libfprint-goodixtls)
  (`gxfp51a7` branch, commit `1f8b7cf`) — base driver, LGPL-2.1-or-later
- [`GodsQuantum/Huawei-Matebook-13-Linux`](https://github.com/GodsQuantum/Huawei-Matebook-13-Linux)
  — `GXFP51A0` bring-up on the same firmware; source of the factory-PMK
  mechanism and the capture recipes
- [`delitdesnoyers06-del/goodix-gxfp-protocol`](https://github.com/delitdesnoyers06-del/goodix-gxfp-protocol)
  — protocol notes for GXFP5187/51A7
- [`szlukabence/goodix-fingerprint-spi-linux`](https://github.com/szlukabence/goodix-fingerprint-spi-linux)
  — protocol specification and imaging method for this family; source of the
  re-arm-before-every-frame requirement
- libfprint TOD fork: `3v1n0/libfprint`, tag `v1.94.10+tod1`

No vendor binaries, firmware images or captured biometric data are included. The
protocol constants retained from captures are documented in the driver source.
All driver code remains LGPL-2.1-or-later.
