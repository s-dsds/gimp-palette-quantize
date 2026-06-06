#!/usr/bin/env bash
#
# build-install-flatpak.sh
#
# Build and install the palette-quantize GEGL op + GIMP plug-in against a
# Flatpak install of GIMP (org.gimp.GIMP). This is the supported path on
# systems where GIMP 3 is only available as a Flatpak (no host -dev packages).
#
# Why this exists instead of install-linux-user.sh:
#   - The Flatpak GIMP bundles GEGL/babl/GIMP dev files in its /app prefix, so
#     the build must happen inside the Flatpak's --devel environment (which
#     mounts the matching org.gnome.Sdk toolchain at /usr and /app on top).
#   - The Flatpak sandbox redirects XDG dirs, so plug-ins do NOT go in the
#     native ~/.local/share and ~/.config/GIMP/3.0 locations. They go in:
#         GEGL op   -> ~/.var/app/org.gimp.GIMP/data/gegl-0.4/plug-ins/
#         GIMP plug -> ~/.config/GIMP/<MAJOR.MINOR>/plug-ins/<name>/
#     (config is bind-mounted to the real ~/.config/GIMP via the
#      xdg-config/GIMP flatpak permission; data is per-app.)
#
# Usage:
#   ./build-install-flatpak.sh            # deps check + build + install + verify
#   ./build-install-flatpak.sh deps       # ensure the matching GNOME SDK is installed
#   ./build-install-flatpak.sh build      # meson setup (if needed) + compile
#   ./build-install-flatpak.sh install    # copy artifacts into the Flatpak dirs
#   ./build-install-flatpak.sh verify     # confirm GEGL op + plug-in register
#   ./build-install-flatpak.sh clean      # remove the build directory
#
set -euo pipefail

APP_ID=org.gimp.GIMP
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
PKGCONF_PATH=/app/lib/pkgconfig:/app/lib/x86_64-linux-gnu/pkgconfig

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

require_gimp() {
  flatpak info "$APP_ID" >/dev/null 2>&1 \
    || die "Flatpak app $APP_ID is not installed. Install it with: flatpak install flathub $APP_ID"
}

# Branch of the GNOME SDK that matches GIMP's runtime, e.g. "50".
sdk_branch() {
  flatpak info "$APP_ID" 2>/dev/null \
    | sed -n 's@.*Sdk:[[:space:]]*org\.gnome\.Sdk/[^/]*/\([0-9]\+\).*@\1@p' \
    | head -1
}

# GIMP user-config version dir, derived from the app version, e.g. "3.2".
gimp_config_version() {
  flatpak info "$APP_ID" 2>/dev/null \
    | sed -n 's@.*Version:[[:space:]]*\([0-9]\+\.[0-9]\+\).*@\1@p' \
    | head -1
}

cmd_deps() {
  require_gimp
  local branch; branch="$(sdk_branch)"
  [ -n "$branch" ] || die "Could not determine GIMP's SDK branch from 'flatpak info $APP_ID'."
  if flatpak info "org.gnome.Sdk//$branch" >/dev/null 2>&1; then
    log "GNOME SDK $branch already installed."
  else
    log "Installing GNOME SDK $branch (matches GIMP runtime)..."
    flatpak remote-add --if-not-exists --user flathub \
      https://dl.flathub.org/repo/flathub.flatpakrepo
    flatpak install -y --user flathub "org.gnome.Sdk//$branch"
  fi
}

# Run a command inside the GIMP Flatpak build environment (SDK toolchain + /app libs).
in_build_env() {
  flatpak run --devel --filesystem=host --share=network \
    --command=bash "$APP_ID" -c "export PKG_CONFIG_PATH=$PKGCONF_PATH; $*"
}

cmd_build() {
  require_gimp
  if [ ! -f "$BUILD_DIR/build.ninja" ]; then
    log "Configuring build (meson setup)..."
    in_build_env "cd '$PROJECT_DIR' && meson setup build"
  fi
  log "Compiling (meson compile)..."
  in_build_env "cd '$PROJECT_DIR' && meson compile -C build"
  [ -f "$BUILD_DIR/src/gegl-palette-quantize.so" ] \
    || die "Build did not produce gegl-palette-quantize.so"
}

cmd_install() {
  require_gimp
  local ver; ver="$(gimp_config_version)"
  [ -n "$ver" ] || die "Could not determine GIMP config version."

  local gegl_so="$BUILD_DIR/src/gegl-palette-quantize.so"
  local gimp_bin="$BUILD_DIR/src/gimp-palette-quantize"
  [ -f "$gegl_so" ] || die "Missing $gegl_so. Run: $0 build"

  local gegl_dst="$HOME/.var/app/$APP_ID/data/gegl-0.4/plug-ins"
  local gimp_dst="$HOME/.config/GIMP/$ver/plug-ins/gimp-palette-quantize"

  log "Installing GEGL op -> $gegl_dst"
  mkdir -p "$gegl_dst"
  cp "$gegl_so" "$gegl_dst/"

  if [ -f "$gimp_bin" ]; then
    log "Installing GIMP plug-in -> $gimp_dst"
    mkdir -p "$gimp_dst"
    cp "$gimp_bin" "$gimp_dst/"
    chmod +x "$gimp_dst/gimp-palette-quantize"
  else
    warn "GIMP plug-in binary not built; installed GEGL op only."
  fi
  log "Done. Restart GIMP if it is running."
}

cmd_verify() {
  require_gimp
  log "Checking GEGL operation registration..."
  if flatpak run --command=sh "$APP_ID" -c 'gegl --list-all 2>/dev/null' \
       | grep -q 'custom:palette-quantize'; then
    log "  OK: custom:palette-quantize is registered."
  else
    warn "  custom:palette-quantize NOT found by gegl."
  fi

  local ver; ver="$(gimp_config_version)"
  local prc="$HOME/.config/GIMP/$ver/pluginrc"
  log "Triggering GIMP plug-in query (headless)..."
  timeout 180 flatpak run "$APP_ID" -i -d -f \
    --batch-interpreter=plug-in-script-fu-eval -b '(gimp-quit 0)' >/dev/null 2>&1 || true
  if grep -aq 'plug-in-palette-quantize-group' "$prc" 2>/dev/null; then
    log "  OK: plug-in-palette-quantize-group registered in pluginrc."
    log "  Menu: Filters > Color > Quantize to Palette..."
  else
    warn "  plug-in not found in $prc (it may still load; check GIMP's Filters menu)."
  fi
}

cmd_clean() {
  log "Removing $BUILD_DIR"
  rm -rf "$BUILD_DIR"
}

cmd_all() {
  cmd_deps
  cmd_build
  cmd_install
  cmd_verify
}

case "${1:-all}" in
  deps)    cmd_deps ;;
  build)   cmd_build ;;
  install) cmd_install ;;
  verify)  cmd_verify ;;
  clean)   cmd_clean ;;
  all)     cmd_all ;;
  *)       die "Unknown command: $1 (use: deps|build|install|verify|clean|all)" ;;
esac
