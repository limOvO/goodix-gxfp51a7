#!/usr/bin/env bash
# Records raw captures only. Writes no images.
#
#   ./record-raw.sh <output-dir> <count>
#
# Deliberately does NOT stitch, render or otherwise turn the captures into a
# picture. Raw sensor samples are needed to measure matching, and keeping the
# recording step free of rendering means a session can never accidentally leave
# a fingerprint image behind. record-captures.sh is the coverage/stitching
# counterpart and does produce images; use it only when you want that.
#
# Set GX_RAW_FINGER to label the set. Nothing here inspects the label; it is so
# you can name the directories "genuine" and "impostor" yourself.
set -euo pipefail

OUT="${1:?usage: record-raw.sh <output-dir> <count>}"
COUNT="${2:?usage: record-raw.sh <output-dir> <count>}"
DUMP=/run/goodixtls/dump
HERE="$(cd "$(dirname "$0")" && pwd)"

[ -x "$HERE/test-verify" ] || { echo "build test-verify first" >&2; exit 1; }
[ -f /tmp/tmpl.bin ] || { echo "no /tmp/tmpl.bin" >&2; exit 1; }

echo "== preparing =="
sudo systemctl stop fprintd 2>/dev/null || true
sudo rm -rf "$DUMP"; sudo mkdir -p "$DUMP"
sudo rm -f /var/lib/fprint/.goodixtls-bg /var/lib/fprint/.goodixtls-timing
mkdir -p "$OUT"

echo
echo "  $COUNT presses please. Vary the angle a little between them - about"
echo "  10-20 degrees - and shift position slightly. Same finger throughout."
echo

sudo env GX_TEMPLATE=/tmp/tmpl.bin \
    LD_LIBRARY_PATH=/opt/libfprint-tod/lib \
    FP_TOD_DRIVERS_DIR=/opt/libfprint-tod/lib/libfprint-2/tod-1 \
    G_MESSAGES_DEBUG=goodixtls \
    timeout $((COUNT * 40)) "$HERE/test-verify" "$COUNT" \
  | tee "$OUT/session.log" || true

echo
echo "== collecting raw captures =="
n=0
for f in "$DUMP"/p*.bin; do
  case "$f" in *.bg) continue ;; esac
  [ -e "$f" ] || continue
  cp "$f" "$OUT/" && cp "$f.bg" "$OUT/" && n=$((n + 1))
done
chown -R "$(id -un):$(id -gn)" "$OUT" 2>/dev/null || true
sudo systemctl start fprintd 2>/dev/null || true

echo "copied $n capture(s) to $OUT"
echo "  (raw .bin/.bg only - no images rendered)"
