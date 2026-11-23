#!/usr/bin/env bash
# Records captures AND judges, as it goes, whether they are stitchable.
#
#   ./record-captures.sh [count] [output-dir]
#
# The point of recording 50 presses is to answer one question honestly: do two
# presses of this sensor share enough content to be stitched? Rather than
# guess, this script records, then measures:
#
#   * pairwise normalised cross-correlation of the ridge images, searched over
#     shifts — this is content overlap, independent of any matcher
#   * the number of geometrically consistent descriptor correspondences
#     (rigid RANSAC inliers)
#
# A pair only stitches if BOTH are healthy (corr well above the ~0.2 noise
# floor, and enough inliers). The script prints the distribution so the
# conclusion rests on 50 presses rather than a handful.
set -euo pipefail

COUNT="${1:-50}"
OUT="${2:-$HOME/錄製/stitch-$(date +%Y%m%d-%H%M%S)}"
DUMP=/run/goodixtls/dump
HERE="$(cd "$(dirname "$0")" && pwd)"

[ -x "$HERE/test-verify" ] || { echo "build test-verify first" >&2; exit 1; }
[ -f /tmp/tmpl.bin ] || { echo "no /tmp/tmpl.bin (copy an enrolled print there)" >&2; exit 1; }

echo "== preparing =="
sudo systemctl stop fprintd 2>/dev/null || true
sudo rm -rf "$DUMP"; sudo mkdir -p "$DUMP"
sudo rm -f /var/lib/fprint/.goodixtls-bg
mkdir -p "$OUT"

cat <<'INSTRUCTIONS'

  HOW TO PRESS (this matters more than anything else)

  For a stitchable set: SAME finger, SAME place, and vary the ANGLE a little
  each time (rotate the fingertip 10-20 degrees, as if turning a dial).
  Do not lift and move to a different part of the fingertip — that is a
  different experiment, and it is the one that has already been shown not to
  produce overlapping content.

INSTRUCTIONS

echo "  Asking for $COUNT presses."
echo "  A missed press is retried and does not use up one of the $COUNT."
echo

sudo env GX_TEMPLATE=/tmp/tmpl.bin \
    LD_LIBRARY_PATH=/opt/libfprint-tod/lib \
    FP_TOD_DRIVERS_DIR=/opt/libfprint-tod/lib/libfprint-2/tod-1 \
    G_MESSAGES_DEBUG=goodixtls \
    timeout $((COUNT * 40)) "$HERE/test-verify" "$COUNT" \
  | tee "$OUT/session.log" || true

echo
echo "== collecting =="
n=0
for f in "$DUMP"/p*.bin; do
  case "$f" in *.bg) continue ;; esac
  [ -e "$f" ] || continue
  cp "$f" "$OUT/" && cp "$f.bg" "$OUT/" && n=$((n + 1))
done
chown -R "$(id -un):$(id -gn)" "$OUT" 2>/dev/null || true
sudo systemctl start fprintd 2>/dev/null || true
echo "copied $n capture(s) to $OUT"
[ "$n" -ge 2 ] || { echo "need at least 2 captures to compare" >&2; exit 1; }

echo
echo "== measuring pairwise content overlap =="
# Every pair, so the conclusion is not based on a handful of samples.
if [ -x "$HERE/overlap-check" ]; then
  "$HERE/overlap-check" "$OUT"/p*.bin | tee "$OUT/overlap.txt"
else
  echo "overlap-check not built; skipping" >&2
fi

echo
echo "== measuring rigid consistency =="
"$HERE/stitch-captures" --stats --csv "$OUT/pairs.csv" \
  --out "$OUT/stitched.png" --coverage "$OUT/coverage.png" \
  "$OUT"/p*.bin | tee "$OUT/stats.txt"

echo
echo "== done =="
echo "  $OUT/overlap.txt    content overlap per pair"
echo "  $OUT/stats.txt      inlier distribution"
echo "  $OUT/stitched.png   stitched result (if any pair was trusted)"
