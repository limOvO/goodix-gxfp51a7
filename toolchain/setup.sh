#!/bin/bash
# Prepares the local build toolchain for the Goodix GXFP51A7 driver:
#   - meson/ninja in a private venv (Arch does not need them installed system-wide)
#   - glib-mkenums/glib-genmarshal extracted from glib2-devel (needed by meson)
# Then writes a pkg-config override so meson finds them.
#
# Idempotent: safe to re-run. Never touches the system.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="$HERE/bin"
PC="$HERE/pc"
mkdir -p "$BIN" "$PC"

# --- meson + ninja -----------------------------------------------------
if [ ! -x "$HERE/venv/bin/meson" ]; then
  echo "Creating python venv with meson and ninja..."
  python3 -m venv "$HERE/venv"
  "$HERE/venv/bin/pip" install --quiet --upgrade pip
  "$HERE/venv/bin/pip" install --quiet meson ninja
fi

# --- glib code generators ---------------------------------------------
if [ ! -x "$BIN/glib-mkenums" ] || [ ! -x "$BIN/glib-genmarshal" ]; then
  echo "Extracting glib-mkenums and glib-genmarshal from glib2-devel..."
  TMP="$(mktemp -d)"
  trap 'rm -rf "$TMP"' EXIT

  # Match the installed glib2 version so the generated code matches the headers.
  GLIBVER="$(pacman -Q glib2 | awk '{print $2}' | sed 's/[-+].*//;s/\([0-9]*\.[0-9]*\.[0-9]*\).*/\1/')"
  URL="https://geo.mirror.pkgbuild.com/core/os/x86_64/glib2-devel-${GLIBVER}-1-x86_64.pkg.tar.zst"

  if ! curl -fsSL -o "$TMP/glib2-devel.pkg.tar.zst" "$URL"; then
    echo "Could not fetch $URL" >&2
    echo "Install glib2-devel with pacman and re-run, or set GLIBVER manually." >&2
    exit 1
  fi

  mkdir -p "$TMP/x"
  tar --zstd -xf "$TMP/glib2-devel.pkg.tar.zst" -C "$TMP/x"
  install -m 0755 "$TMP/x/usr/bin/glib-mkenums" "$BIN/glib-mkenums"
  install -m 0755 "$TMP/x/usr/bin/glib-genmarshal" "$BIN/glib-genmarshal"
fi

# --- pkg-config override ----------------------------------------------
# meson reads the tool paths from glib-2.0.pc, which points at /usr/bin on
# Arch even though the tools live in glib2-devel. Point it at ours instead.
sed -e "s#^glib_mkenums=.*#glib_mkenums=$BIN/glib-mkenums#" \
    -e "s#^glib_genmarshal=.*#glib_genmarshal=$BIN/glib-genmarshal#" \
    /usr/lib/pkgconfig/glib-2.0.pc > "$PC/glib-2.0.pc"

echo "Toolchain ready in $HERE"
echo "  meson:          $HERE/venv/bin/meson"
echo "  glib-mkenums:   $BIN/glib-mkenums"
echo "  pkgconfig pc:   $PC"
