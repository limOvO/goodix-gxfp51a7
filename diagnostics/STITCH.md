# stitch-captures — registering and stitching fingerprint captures

A standalone tool that recovers the **rigid transform (rotation + translation)**
between captures of the Goodix sensor and lays them out in one image.

## Why a separate tool

The driver's own enrolment registration (`gx_sift_island_add`) estimates
**translation only**. On a ~6×5 mm pad, a press is rarely upright, so a view
pressed at a different angle cannot be attached — the matcher's geometric model
simply has no rotation term. The descriptors themselves *are* rotation-invariant
(they carry an orientation), so this is a modelling gap, not a descriptor
problem. This tool closes it: RANSAC over two-point **rigid** hypotheses, which
two correspondences fully determine.

## Build

```bash
cc -O2 stitch-captures.c goodix_sift.c -o stitch-captures \
   $(pkg-config --cflags --libs glib-2.0) -lz -lm -I../../sources/goodixtls
```

Requires the driver's `goodix_sift.c/.h` (fetched by `bootstrap.sh`) and zlib.

## Use

Captures must be dumped first — create the dump directory and the driver writes
one `.bin` plus one `.bin.bg` per capture:

```bash
sudo mkdir -p /run/goodixtls/dump
# ... run an enrolment / verification so captures accumulate ...
sudo ./stitch-captures --out stitched.png --coverage coverage.png /run/goodixtls/dump/p*.bin
```

`--coverage` writes a second image whose brightness is how many views contributed
to each pixel — useful for judging whether an enrolment actually covered area.

## Method

1. **Per capture:** `diff = background − frame`, then remove the sensor's
   row/column banding (median per row and column, as the driver does).
2. **Keypoints:** the driver's SIFT-like detector and RootSIFT descriptors.
3. **Pairwise matching:** nearest neighbour with Lowe's ratio test, **loosened
   to 0.95**. Measured: 0.90 keeps 13 correspondences between two presses, 0.95
   keeps 81. The ratio's job (outlier rejection) is done by RANSAC here, and
   RANSAC needs input to find a pose at all.
4. **RANSAC over rigid hypotheses:** 2 correspondences → rotation + translation.
   Least-squares refine on the inlier set, then tighten the gate.
5. **Trust a pair** only at ≥10 inliers. Measured on ground truth, translation
   scores 86, +12° scores 12, −15° scores 17; random pairs sit at 2–5.
6. **Layout:** breadth-first accumulation of transforms from the best-connected
   view, so a chain reaches views that do not overlap the seed directly.
7. **Blend:** feather-weighted average by distance to each view's own border,
   which hides seams and the vignetting at the pad's edge.

## Validation

`stitch-selftest.py` generates views of a real capture at **known** poses and
checks what comes back:

| applied | recovered | inliers |
|---|---|---|
| +0°, +26 px | +0.2°, +26.2 px | 86 |
| +12° | +12.1° | 12 |
| −15° | −14.9° | 17 |

All four views placed; rotation error under 0.5°.

## Results on real presses

Earlier in this work I concluded real presses could not be stitched, because a
handful of captures measured only 2–5 inliers. **That conclusion was wrong**, and
the fault was in the sample, not the physics: those captures were taken of
*different parts* of the fingertip (measured inter-image correlation 0.10–0.42),
so they genuinely shared no content.

A proper session — 15 presses of **one spot**, rotating the fingertip 10–20° each
time — gives entirely different numbers:

| | value |
|---|---|
| pairs with any overlap | 46 / 153 |
| r ≥ 0.60 (content genuinely aligns) | 3 pairs, best **r = 0.993** |
| pairs clearing the inlier gate | **23** (strongest: **92 inliers**) |
| recovered rotations | consistent 0…−26° across the set |

The stitched output shows continuous ridges and a coherent whorl. So rigid
stitching **does** work on this sensor, provided the presses cover the same spot.

## Two bugs this found

1. **Transform composition was inverted.** It used `gth_A + th` and mapped only
   the anchor through `t`. Small genuine rotations (−3°…−21°) therefore
   *accumulated* along the placement chain instead of cancelling, producing a
   radially smeared image that looked like a deformation problem. The correct
   composition is `gth_B = gth_A − th` with
   `G_B = G_A + R(gth_A − th)(q − t) − R(gth_A) q`, derived on paper and checked
   numerically before use. The self-test could not catch this on its own: with
   the seed view at rotation 0, every composed rotation stays small enough to
   look plausible.
2. **A physically impossible pair was trusted.** A −91.7° / +146 px pair cleared
   the inlier gate (20 inliers) purely from repetitive ridge structure and
   wrecked the layout. A plausibility gate now rejects anything beyond 60° or
   80 px, and edges are consumed strongest-first so a 92-inlier anchor is not
   displaced by a 10-inlier one.

## Remaining limits

- Only views connected to the anchor through a trusted pair are placed
  (5/18 in the session above). Coverage, not correctness, is what limits the
  output size; more presses in a tighter rotation range would connect more.
- Strong elastic deformation is still not modelled. It is second-order here —
  the rigid result aligns well — but a non-rigid stage (thin-plate spline) would
  sharpen overlaps further.

## Recording a good set

`../record-captures.sh [count]` drives the recording, then reports both the
cross-correlation and the inlier distribution, so a session can be judged
immediately rather than assumed:

```bash
sudo ./record-captures.sh 15
```

## Do not read the output as a fingerprint record

The stitched image is a diagnostic view of sensor data. It is biometric data —
treat any file produced from real presses as sensitive.
