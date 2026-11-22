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
| Match scores (fresh background) | 24-28 against a threshold of 20 |

**What is not yet confirmed:** a completed 20-stage enrolment and a matching
verify. Both need ~20 physical presses, which cannot be automated. The capture
path is proven; the end-to-end enrolment is a manual step (see §9).

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
| Speed | reuse cached background; fix timing-scale ratchet; retune delays |
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

## 6. Performance

Recognition felt slow, so each phase was measured rather than guessed at
(`fp_dbg` timestamps per protocol step).

| | before | after |
|---|---|---|
| Single capture | ~900 ms | **~649 ms** |
| Session start | 1581 ms | ~1260 ms |

**A caching idea that had to be reverted — and why it matters.** Reusing the
background frame across recognitions looked like a free ~700 ms: the frame holds
no fingerprint, because it is only ever taken with the finger lifted. It is not
free. The sensor's baseline drifts within tens of seconds, so an older
background yields a distorted difference image and the matcher collapses.

Measured on real verifications, the same enrolled template scored:

| background | scores | threshold 20 |
|---|---|---|
| fresh each time | 24, 26, 27, 28 | matches |
| cached (up to 2 h) | 0, 0, 2, 3, 7 | **never matches** |

That was a genuine regression I introduced, and it is why recognition stopped
working. The behaviour is now upstream's again: a fresh background before every
operation, with the cache used only when a finger is already down. The cache age
was also cut from 2 h to 20 s so a stale frame cannot be picked up elsewhere.
The upstream comment already said this ("a background captured too early ...
collapses the score"); I should have trusted it instead of measuring only
latency.

**1. The timing scale was stuck at 150%.** A ratchet bug: a capture returning
*zero* bytes was treated as "delays too tight" and raised the persisted
multiplier by 50% each time. No amount of extra delay can make a missing frame
appear — the real cause was the wrong capture recipe (§3.3), since fixed. Only a
frame that arrives but decrypts short now counts as a timing failure, and the
step-down is quick (3 clean sessions) so an unneeded scale cannot persist.

**2. Conservative protocol delays.** Re-measured per point, 5–6 captures each:

| drain silence | result |
|---|---|
| 4 → 3 | 724 ms → 615 ms, 6/6 reliable |
| 2 | **0/6** — no usable frame at all |

2 is a cliff rather than a degradation, so 3 is the floor. The inter-command gap
also went from 30 ms to 25 ms (measured 6/6 at that setting, 5/6 at 15 ms, which
is why 25 was kept rather than the fastest value).

Net effect on capture: **1.4x faster**, with matching left intact.

---

## 7. An upstream TLS bug found on the way

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

## 8. Verification

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

## 9. Remaining work

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

## 10. Security notes

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

## 11. Provenance

Reverse-engineering input, all public:

- [`Sigfrodr/libfprint-goodixtls`](https://github.com/Sigfrodr/libfprint-goodixtls)
  (`gxfp51a7` branch, commit `1f8b7cf`) — base driver, LGPL-2.1-or-later
- [`GodsQuantum/Huawei-Matebook-13-Linux`](https://github.com/GodsQuantum/Huawei-Matebook-13-Linux)
  — `GXFP51A0` bring-up on the same firmware; source of the factory-PMK
  mechanism and the capture recipes
- [`delitdesnoyers06-del/goodix-gxfp-protocol`](https://github.com/delitdesnoyers06-del/goodix-gxfp-protocol)
  — protocol notes for GXFP5187/51A7
- libfprint TOD fork: `3v1n0/libfprint`, tag `v1.94.10+tod1`

No vendor binaries, firmware images or captured biometric data are included. The
protocol constants retained from captures are documented in the driver source.
All driver code remains LGPL-2.1-or-later.
