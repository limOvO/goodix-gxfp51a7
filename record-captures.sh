#!/usr/bin/env bash
# Records captures, then reports whether they form ONE stitchable group.
#
#   ./record-captures.sh [count] [output-dir]
#
# The point is to widen coverage: presses of adjacent regions, overlapping like
# jigsaw pieces, so the stitched result grows. Recording unrelated regions does
# not work — measured on 15 presses, the trusted-pair graph came out as two
# disjoint groups of 7 and 6, and no amount of stitching can bridge groups that
# share no content.
#
# So the instructions below ask for a SWEEP pattern rather than scattered
# presses, and the script ends by reporting the connectivity so a bad session is
# caught immediately instead of after the fact.
set -euo pipefail

COUNT="${1:-24}"
OUT="${2:-$HOME/錄製/coverage-$(date +%Y%m%d-%H%M%S)}"
DUMP=/run/goodixtls/dump
HERE="$(cd "$(dirname "$0")" && pwd)"

[ -x "$HERE/test-verify" ] || { echo "build test-verify first (see README)" >&2; exit 1; }
[ -x "$HERE/stitch-captures" ] || { echo "build stitch-captures first" >&2; exit 1; }
[ -f /tmp/tmpl.bin ] || { echo "no /tmp/tmpl.bin - copy an enrolled print there first" >&2; exit 1; }

echo "== preparing =="
sudo systemctl stop fprintd 2>/dev/null || true
sudo rm -rf "$DUMP"; sudo mkdir -p "$DUMP"
sudo rm -f /var/lib/fprint/.goodixtls-bg
mkdir -p "$OUT"

cat <<'INSTRUCTIONS'

  HOW TO PRESS - this determines whether the result grows at all

  Think of it as walking across your fingertip in small steps, so that every
  press shares a band with the one before it. A pattern that works:

      centre,  a little UP,   centre,  a little DOWN,
      centre,  a little LEFT, centre,  a little RIGHT,
      then a ring: upper-left, upper-right, lower-right, lower-left

  Each step should be about a THIRD of the pad's width, not a full pad. If you
  jump to a region that does not touch the previous one, the stitch splits into
  groups that cannot be joined later.

  Rotating the fingertip slightly in place (10-20 degrees) also helps: it keeps
  overlap high while adding new ridge detail at the edges.

  Keep the same finger throughout.

INSTRUCTIONS

echo "  Asking for $COUNT presses. A missed press is retried and does not"
echo "  use up one of the $COUNT."
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
[ "$n" -ge 2 ] || { echo "need at least 2 captures" >&2; exit 1; }

echo
echo "== measuring content overlap =="
if [ -x "$HERE/overlap-check" ]; then
  "$HERE/overlap-check" "$OUT"/p*.bin > "$OUT/overlap.txt" 2>&1 || true
  tail -6 "$OUT/overlap.txt"
fi

echo
echo "== stitching =="
"$HERE/stitch-captures" --stats --csv "$OUT/pairs.csv" \
  --out "$OUT/stitched.png" --coverage "$OUT/coverage-map.png" \
  "$OUT"/p*.bin > "$OUT/stats.txt" 2>&1 || true
grep -E 'inliers :|trusted \(|placed|view\(s\) placed' "$OUT/stats.txt" | tail -6

echo
echo "== connectivity =="
python3 "$HERE/diagnostics/coverage-report.py" "$OUT" --csv "$OUT/pairs.csv" || true

echo
echo "== done =="
echo "  $OUT/stitched.png       the stitched image"
echo "  $OUT/coverage-map.png   how many views cover each pixel"
echo "  $OUT/overlap.txt        per-pair content correlation"
echo "  $OUT/stats.txt          inlier distribution and layout log"
