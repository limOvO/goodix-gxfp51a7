#!/bin/bash
# Stands the project up from nothing: clones the two upstream sources at the
# exact revisions this work was built on, then restores the local driver patch.
#
#   ./bootstrap.sh
#
# Safe to re-run: existing checkouts are left alone.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/sources"
PATCH="$HERE/01-app-14115-support.patch"

mkdir -p "$SRC"

# --- libfprint TOD ----------------------------------------------------
TOD="$SRC/libfprint-tod"
if [ ! -d "$TOD/.git" ]; then
  echo "==> Cloning libfprint-tod (v1.94.10+tod1)"
  git clone --quiet --branch 'v1.94.10+tod1' \
    https://gitlab.freedesktop.org/3v1n0/libfprint.git "$TOD"
else
  echo "==> libfprint-tod already present"
fi

# --- Goodix driver ----------------------------------------------------
GX="$SRC/goodixtls"
if [ ! -d "$GX/.git" ]; then
  echo "==> Cloning libfprint-goodixtls (gxfp51a7)"
  git clone --quiet --branch gxfp51a7 \
    https://github.com/Sigfrodr/libfprint-goodixtls.git "$GX"
else
  echo "==> goodixtls already present"
fi

# --- local patch ------------------------------------------------------
if [ -f "$PATCH" ] && [ ! -f "$GX/.patched" ]; then
  echo "==> Applying $(basename "$PATCH")"
  git -C "$GX" apply --check "$PATCH"
  git -C "$GX" apply "$PATCH"
  touch "$GX/.patched"
else
  echo "==> Driver patch already applied (or absent)"
fi

echo
echo "Ready. Next:"
echo "    ./toolchain/setup.sh    # meson/ninja + glib code generators"
echo "    ./build.sh              # libfprint-tod + driver into ./stage"
echo "    sudo ./install.sh       # install"
