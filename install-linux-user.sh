#!/bin/sh
set -eu

BUILD_DIR=${1:-build}
GEGL_SO="$BUILD_DIR/src/gegl-palette-quantize.so"
GIMP_BIN="$BUILD_DIR/src/gimp-palette-quantize"

if [ ! -f "$GEGL_SO" ]; then
  echo "Missing $GEGL_SO. Run: meson setup build && meson compile -C build" >&2
  exit 1
fi

mkdir -p "$HOME/.local/share/gegl-0.4/plug-ins"
cp "$GEGL_SO" "$HOME/.local/share/gegl-0.4/plug-ins/"

if [ -f "$GIMP_BIN" ]; then
  mkdir -p "$HOME/.config/GIMP/3.0/plug-ins/gimp-palette-quantize"
  cp "$GIMP_BIN" "$HOME/.config/GIMP/3.0/plug-ins/gimp-palette-quantize/"
  chmod +x "$HOME/.config/GIMP/3.0/plug-ins/gimp-palette-quantize/gimp-palette-quantize"
  echo "Installed GEGL op and GIMP plug-in. Restart GIMP."
else
  echo "Installed GEGL op only. GIMP plug-in binary was not built."
fi
