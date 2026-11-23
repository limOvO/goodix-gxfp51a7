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
| Duplicate presses | recorded but not counted during enrolment (§9) |
| New areas | pending until seen twice, then added (§9) |
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

A 0xD0 can arrive at two points, and both are now handled. The first version of
this only covered one, which is why the sensor could still wedge completely:

| where the 0xD0 arrives | handled |
|---|---|
| during the capture sequence (drain) | aborts the sequence, then retries |
| while waiting for the image record | **was missed** — set the flag but never acted on it |

The second path is the common one, so a wedge could leave the sensor answering
nothing at all until a manual recovery. Both now set `tls_reconn` and retry once
on a fresh session.

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

## 9. Learning new areas, without letting a bad press in

Two behaviours were added so the template can grow from ordinary use while
staying trustworthy.

### During enrolment: a duplicate is recorded, but does not count

Pressing the same spot again cannot improve coverage, so the driver measures how
much of each press the views collected so far already explain. Above
`GX_DUPLICATE_FRAC` the press is **kept** — its descriptors still contribute to
the fused template — but it does not consume a stage, and the user is asked to
shift to a new area.

The threshold is measured, not guessed, using the driver's own fusion matcher:

| pair | content correlation | fraction already covered |
|---|---|---|
| p015 / p016 | 0.993 | 0.73 |
| p002 / p003 | 0.676 | 0.57 |
| p012 / p014 | 0.584 | 0.45 |
| p008 / p009 | 0.513 | 0.39 |
| p005 / p006 | 0.629 | 0.38 |
| p001 / p002 | 0.275 | 0.24 |
| p001 / p008 | 0.106 | 0.00 |

Same-area pairs span 0.38–0.73, unrelated ones 0.00–0.24, so the boundary sits
at **0.33**. The first value tried was 0.85, which is above every real
same-area pair — it could never fire, and only measurement showed that.

### Outside enrolment: a new area needs a second sighting

A verify that fails is normally just a failed attempt. But a score in the band
between `0.45 × threshold` and the threshold means *the same finger, an area the
template does not cover* — the ridges agree, just not enough to authenticate.

Such a capture is **not** added immediately. It is written to a `.pending` store
next to the adaptive one. When the same area appears again — the new capture
matching the pending one — the pair is promoted into the template's store. One
unrecognised press could be a bad press, a smear or another finger; two
independent presses agreeing on ridges the template lacks is much stronger
evidence.

Re-enrolling clears the pending store: its entries were "areas the old template
does not cover", which is void once the template is replaced.

Verified on hardware with 8 presses (4 centre, 4 edge): the `.pending` file
appeared and the `.views` store was **not** written to, i.e. the first sighting
of each new area was recorded without being trusted. A separate check on
recorded pairs confirms the promotion threshold separates the populations — the
same-area pairs in the table above all clear it, the unrelated ones do not.

Safety note, unchanged by any of this: matching runs on the host, so these
stores are only as protected as the filesystem. They are written `0600`.

---

## 10. Press-to-verdict latency

Measured from the driver's own timestamps over 4 presses, not estimated
(`diagnostics/fingerprint-latency.py`):

| press | capture sequence | image reception | scoring | total | score |
|---|---|---|---|---|---|
| 1 | 604 ms | 125 ms | 86 ms | **815 ms** | 36 |
| 2 | 602 ms | 122 ms | 90 ms | **814 ms** | 30 |
| 3 | 610 ms | 124 ms | 90 ms | **824 ms** | 10 |
| 4 | 611 ms | 124 ms | 90 ms | **825 ms** | 17 |

Median **820 ms** from the moment the driver acts on the press to the match
decision, with a spread of only 11 ms across presses.

Where it goes:

| phase | time | share |
|---|---|---|
| capture sequence (5 protocol commands) | 607 ms | 74% |
| image reception and TLS decryption | 124 ms | 15% |
| descriptor matching and decision | 89 ms | 11% |

The matching itself — the part that would be worth optimising in software — is
only **89 ms**. Three quarters is the protocol exchange with the sensor.

**Add the detection poll.** The driver cannot see a press until its next finger
probe, and each probe is itself a protocol exchange, so probes land ~545 ms
apart (up to 1250 ms when the loop is between states). A press therefore waits
0-545 ms just to be noticed, averaging ~272 ms:

> **Typical touch-to-verdict: ~1.1 s** (820 ms path + ~270 ms poll wait)

The drains and gaps that dominate cannot simply be shortened: measured earlier,
drain 2 wedges the sensor outright, and drain 3 works for background frames but
not for a pressed finger. This is close to the floor for this part.

---

## 11. Optimising the dominant cost: the reply-ready interrupt

74% of the latency was the capture sequence, and the largest single component
was that the driver **guessed** when a reply had arrived: a fixed 15 ms sleep
after each command, then polling. The vendor driver does not guess — the sensor
raises GPIO 48 when a reply is staged, and the public protocol notes document
it.

`diagnostics/irq-probe.c` verified the line on this hardware before any driver
change: it asserted on **8/8** exchanges, and an IRQ-driven round trip took
**5.8 ms against 35 ms** for the timed approach.

The driver now waits on that line for reply-ready. Measured effect:

| | before | after |
|---|---|---|
| cmd 0xae | 82 ms | 67 ms |
| cmd 0x36 | 92 ms | 78 ms |
| cmd 0x50 | 106 ms | 92 ms |
| cmd 0x20 | 77 ms | 62 ms |
| **capture sequence** | **607 ms** | **524 ms** |
| press → verdict | 820 ms | 737 ms |
| touch → verdict (incl. poll) | ~1092 ms | ~1009 ms |

83 ms off every recognition, **14%** of the sequence, verified 5/5 captures
with no failures.

### What was tried and reverted

Also using the line to decide when the reply was *finished* — replacing the
drain's four 10 ms silence misses — broke capture outright: **0/5 frames**, and
the sequence grew past a second as the bus fell progressively further out of
step.

The reason is worth recording, because it looks like an obvious optimisation:
a 3 ms quiet window is far shorter than the sensor's own inter-frame gap, so
the drain returned while the sensor was still emitting, and the next command
collided with a reply still in flight. The timed misses are not a slower
substitute for the line — they measure **silence**, which is what the protocol
actually requires before the next command may be sent. The line answers "has the
reply started", not "is it over".

The reverted version was removed rather than left behind a flag, and the code
carries the reasoning so it is not retried.

### It stays optional

Some boards may not wire GPIO 48, and another process may hold it. If the line
cannot be claimed the driver says so and falls back to the timed path, which is
the behaviour everything was validated with. `GOODIXTLS_NO_IRQ=1` forces the
fallback for comparison; verified it still captures (599 ms, image received).

---

## 12. Cutting the sequence: configure once, not per press

The interrupt work (§11) shaved 83 ms, which was not enough. The problem was
structural, not tunable: **every press re-ran all five commands**, but only
`0x20` reads the sensor. The other four — MCU state, FDT boot zones, NAV,
background FDT baseline — configure state that persists for the TLS session.

Doing that before every press was 4/5 of the sequence spent on a job already
done. The background capture at session start is the pass that configures the
sensor; a press afterwards only needs the image.

| | before | after |
|---|---|---|
| sequence on a press | 376 ms | **60 ms** |
| press → verdict | 623 ms | **298 ms** |
| touch → verdict (incl. poll) | ~895 ms | ~570 ms |

Median **298 ms** over 5 presses (295–339), 0 failures, measured with the
compiled-in defaults rather than environment overrides. Match scores *improved*
— 51, 90, 84, 81, 76 against a threshold of 20 — because the background is now
captured once with the finger lifted instead of being re-settled under a
pressing finger.

The configuration is tied to the TLS session, so it is cleared whenever the
session is torn down. If a press ever arrives before the background pass has
run, the driver falls back to the full sequence rather than sending `0x20` to an
unconfigured sensor.

### Protocol timings re-measured

With the sequence split, the per-command delays were re-swept (8–10 captures
per point, clean timing scale):

| drain misses × spacing | reply read | sequence | reliability |
|---|---|---|---|
| 4 × 10 ms (previous) | 5 ms | 526 ms | 10/10 |
| **3 × 8 ms** | **4 ms** | **371 ms** | **18/18** |
| 3 × 7 ms | 4 ms | — | 0/4 |
| 3 × 6 ms | 4 ms | — | 0/3 |

The inter-command gap also went 30 ms → 15 ms. The floor is a hard cliff — 3 ×
7 ms fails outright rather than degrading — so these sit one step back from it.

---

## 13. A press at a different angle: the geometry, not the descriptor

The descriptor is rotation-normalised, so a rotated press should still match. It
did not: on real presses, the same finger area pressed askew was rejected.

Measuring it correctly mattered. An early attempt decoded the saved captures as
packed 12-bit when the recorder writes already-decoded 16-bit samples, and the
resulting features were self-consistent but meaningless — they "proved" the
descriptor was not rotation-invariant at all, which was wrong.

With the loader matching `stitch-captures.c`, rotating one capture against
itself:

| angle | correspondences | translation vote (old) | rigid fit (new) |
|---|---|---|---|
| 0° | 115 | 110 | 110 |
| 5° | 85 | 66 | 66 |
| 10° | 66 | **15** | **50** |
| 15° | 66 | **15** | **39** |
| 20° | 69 | **5** | **34** |
| 30° | 45 | **3** | **20** |

The descriptors keep producing correspondences to about 20°. What broke was the
geometric validation: it voted on a **translation** only. Under rotation every
correct pair implies a *different* translation, so the vote counts them as
mutually inconsistent and discards them — the exact pairs that are right.

So the fix is a rigid hypothesis (rotation plus translation) fitted by RANSAC
over pairs of correspondences, refined once on the inlier set. No tolerance was
loosened; `GEO_TOL` stays at 6 px.

Against a real multi-view template, probe rotated, threshold 20:

| angle | old pass | new pass |
|---|---|---|
| 0° | 12/18 | **14/18** |
| 10° | 11/18 | **13/18** |
| 20° | 6/18 | **13/18** |
| 30° | 4/18 | **12/18** |
| 35° | 1/18 | **11/18** |

### Impostor check

A rigid fit has one more degree of freedom than a translation, so it can fit
noise. Measured on 234 impostor trials (18 captures of one finger against 13 of
another):

| | old | new |
|---|---|---|
| impostor max | 4 | **5** |
| impostor ≥ 5 | 0/234 | **1/234** (0.43 %) |
| genuine median | 1 | 2 |
| genuine p90 | 7 | 10 |

Against a threshold of 20 the margin stays a factor of four. No threshold change
was needed.

### What this does and does not solve

Rotation is covered by matching, so **one enrolment angle serves many press
angles** — no need to record the finger at several orientations:

```
   rotating one capture against a template built from the others:
     0° 14/18    10° 13/18    20° 13/18    30° 12/18    35° 11/18
```

What it does not fix is a press on an area the template never covered: the four
captures that score zero do so at *every* angle, because they share no content
at all. That is the coverage problem of section 12, not the geometry here.

### Result on hardware

Six normal presses, threshold 20: 33, 34, 20, 21, 24 — 5/5, no failures.

---

## 14. An upstream TLS bug found on the way

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

## 15. Verification

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

## 16. Remaining work

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

## 17. Security notes

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

## 18. Provenance

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
