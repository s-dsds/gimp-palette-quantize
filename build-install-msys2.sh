#!/usr/bin/env bash
#
# build-install-msys2.sh
#
# Build and install the palette-quantize GEGL op + GIMP plug-in on Windows
# using MSYS2 / MinGW-w64. Run this from an MSYS2 *environment* shell
# (UCRT64 or MINGW64) — NOT the plain "MSYS" shell — so that $MINGW_PACKAGE_PREFIX
# and a native gcc/meson/pkg-config are on PATH.
#
# Usage (inside the UCRT64 or MINGW64 shell):
#   ./build-install-msys2.sh           # deps + build + install
#   ./build-install-msys2.sh deps      # pacman -S the toolchain + GIMP/GEGL/babl
#   ./build-install-msys2.sh build     # meson setup (if needed) + compile
#   ./build-install-msys2.sh install   # copy artifacts into the GIMP/GEGL dirs
#   ./build-install-msys2.sh clean     # remove the build directory
#
# Overrides (env vars):
#   GIMP_VER=3.2   # GIMP user-config version dir (the one your *running* GIMP uses)
#
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$PROJECT_DIR/build"

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

check_env() {
  [ -n "${MINGW_PACKAGE_PREFIX:-}" ] \
    || die "Not in an MSYS2 environment shell. Open 'MSYS2 UCRT64' (or MINGW64) and re-run."
}

cmd_deps() {
  check_env
  log "Installing toolchain + GIMP/GEGL/babl via pacman ($MINGW_PACKAGE_PREFIX-*)..."
  pacman -S --needed --noconfirm \
    "${MINGW_PACKAGE_PREFIX}-gcc" \
    "${MINGW_PACKAGE_PREFIX}-meson" \
    "${MINGW_PACKAGE_PREFIX}-ninja" \
    "${MINGW_PACKAGE_PREFIX}-pkgconf" \
    "${MINGW_PACKAGE_PREFIX}-gimp" \
    "${MINGW_PACKAGE_PREFIX}-gegl" \
    "${MINGW_PACKAGE_PREFIX}-babl"
}

cmd_build() {
  check_env
  command -v meson >/dev/null || die "meson not found. Run: $0 deps"
  pkg-config --exists gimp-3.0 \
    || die "gimp-3.0 dev files not found by pkg-config. Run: $0 deps"

  if [ ! -f "$BUILD_DIR/build.ninja" ]; then
    log "Configuring (meson setup)..."
    meson setup "$BUILD_DIR" "$PROJECT_DIR"
  fi
  log "Compiling (meson compile)..."
  meson compile -C "$BUILD_DIR"

  [ -f "$BUILD_DIR/src/gegl-palette-quantize.dll" ] \
    || die "Build did not produce gegl-palette-quantize.dll"
  [ -f "$BUILD_DIR/src/gimp-palette-quantize.exe" ] \
    || warn "GIMP plug-in .exe was not built (gimp-3.0/gimpui-3.0 missing?)."
}

cmd_install() {
  check_env
  # GIMP user version dir: defaults to the build GIMP's major.minor. If your
  # *running* GIMP differs, pass GIMP_VER=... (see Edit > Preferences > Folders).
  local ver="${GIMP_VER:-$(pkg-config --modversion gimp-3.0 | cut -d. -f1,2)}"
  [ -n "$ver" ] || die "Could not determine GIMP version; pass GIMP_VER=3.2"

  local appdata localappdata
  appdata="$(cygpath -u "${APPDATA}")"
  localappdata="$(cygpath -u "${LOCALAPPDATA}")"

  local gegl_dll="$BUILD_DIR/src/gegl-palette-quantize.dll"
  local gimp_exe="$BUILD_DIR/src/gimp-palette-quantize.exe"
  [ -f "$gegl_dll" ] || die "Missing $gegl_dll. Run: $0 build"

  local gegl_dst="$localappdata/gegl-0.4/plug-ins"
  local gimp_dst="$appdata/GIMP/$ver/plug-ins/gimp-palette-quantize"

  log "Installing GEGL op -> $gegl_dst"
  mkdir -p "$gegl_dst"
  cp "$gegl_dll" "$gegl_dst/"

  if [ -f "$gimp_exe" ]; then
    log "Installing GIMP plug-in -> $gimp_dst"
    mkdir -p "$gimp_dst"
    cp "$gimp_exe" "$gimp_dst/"
  else
    warn "GIMP plug-in .exe not built; installed GEGL op only."
  fi

  log "Done. Fully restart GIMP."
  log "If the GEGL op is not found, set GEGL_PATH to '$gegl_dst' in"
  log "  GIMP's environ file, or copy the .dll next to GIMP's other GEGL ops."
}

cmd_clean() { log "Removing $BUILD_DIR"; rm -rf "$BUILD_DIR"; }

case "${1:-all}" in
  deps)    cmd_deps ;;
  build)   cmd_build ;;
  install) cmd_install ;;
  clean)   cmd_clean ;;
  all)     cmd_deps; cmd_build; cmd_install ;;
  *)       die "Unknown command: $1 (use: deps|build|install|clean|all)" ;;
esac
