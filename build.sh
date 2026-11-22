#!/bin/bash
# Builds libfprint-tod (the TOD variant of libfprint that can load
# out-of-tree drivers) into a staging tree, plus the Goodix driver module
# built against it.
#
# Produces:
#   $HERE/stage/          -> TOD libfprint, staged with prefix /usr
#   $HERE/stage/drivers/  -> libgoodixtls-tod-driver.so
#
# Nothing is installed system-wide here. Run install.sh afterwards.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/sources"
TC="$HERE/toolchain"
STAGE="$HERE/stage"

export PATH="$TC/bin:$TC/venv/bin:$PATH"
export PKG_CONFIG_PATH="$TC/pc"

[ -x "$TC/venv/bin/meson" ] || { echo "Run toolchain/setup.sh first." >&2; exit 1; }

# The APP_14115 support lives as a patch rather than only in the working tree,
# so a fresh clone can be brought back to the exact known-good source.
PATCHES=("$HERE"/[0-9][0-9]-*.patch)
if [ -e "${PATCHES[0]}" ] && [ ! -f "$SRC/goodixtls/.patched" ]; then
  echo "==> Applying driver patches"
  for p in "${PATCHES[@]}"; do
    git -C "$SRC/goodixtls" apply --check "$p" 2>/dev/null && \
      git -C "$SRC/goodixtls" apply "$p" && echo "    $(basename "$p")"
  done
  touch "$SRC/goodixtls/.patched"
fi

# --- 1. libfprint TOD --------------------------------------------------
echo "==> Building libfprint-tod"
cd "$SRC/libfprint-tod"
rm -rf build
meson setup build \
  --prefix=/usr \
  -Ddrivers=default \
  -Dintrospection=false \
  -Dinstalled-tests=false \
  -Ddoc=false \
  -Dgtk-examples=false >/dev/null
ninja -C build >/dev/null

rm -rf "$STAGE"
DESTDIR="$STAGE" ninja -C build install >/dev/null
echo "    staged to $STAGE"

# --- 2. Goodix driver against it ---------------------------------------
# The staged .pc files point at /usr, which does not exist yet. Rewrite them
# to the staging prefix so the driver compiles and links without installing
# libfprint first.
mkdir -p "$HERE/pc-stage"
for f in libfprint-2.pc libfprint-2-tod-1.pc; do
  sed "s#^prefix=/usr\$#prefix=$STAGE/usr#" "$STAGE/usr/lib/pkgconfig/$f" \
    > "$HERE/pc-stage/$f"
done

echo "==> Building libgoodixtls driver"
cd "$SRC/goodixtls"
rm -rf build
PKG_CONFIG_PATH="$TC/pc:$HERE/pc-stage" meson setup build \
  --prefix=/usr --sysconfdir=/etc >/dev/null
PKG_CONFIG_PATH="$TC/pc:$HERE/pc-stage" ninja -C build >/dev/null

# --- 3. Collect --------------------------------------------------------
mkdir -p "$STAGE/drivers"
install -m 0755 build/libgoodixtls-tod-driver.so "$STAGE/drivers/"
install -m 0644 "$SRC/goodixtls/60-libfprint-2-goodixtls.rules" "$STAGE/"

echo "==> Tests"
PKG_CONFIG_PATH="$TC/pc:$HERE/pc-stage" meson test -C build --print-errorlogs | tail -5

echo
echo "Build complete."
echo "  libfprint-tod -> $STAGE/usr"
echo "  driver module -> $STAGE/drivers/libgoodixtls-tod-driver.so"
