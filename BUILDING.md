# Building & Installing (Flatpak GIMP)

This documents the build/install path for systems where **GIMP 3 is only
available as a Flatpak** (`org.gimp.GIMP`) — i.e. there are no host
`gimp-3.0` / `gegl-0.4` `-dev` packages. This is the case on Ubuntu/Debian,
where `apt` only ships GIMP **2** development files.

If you instead have a native GIMP 3 install with `pkg-config` files on the
host, use the simpler `install-linux-user.sh` and the instructions in
`README.md`.

## TL;DR

```bash
./build-install-flatpak.sh        # deps + build + install + verify
```

Then restart GIMP and open **Filters → Color → Quantize to Palette…**

## Why the Flatpak path is different

Two things make a Flatpak GIMP special:

1. **Dev files live inside the Flatpak.** The GIMP Flatpak bundles
   `gegl-0.4`, `babl-0.1`, `gimp-3.0`, and `gimpui-3.0` `.pc` files in its
   `/app` prefix. They are not visible to the host. The compiler, `meson`,
   `ninja`, and `json-glib` come from the matching **`org.gnome.Sdk`** that
   GIMP's runtime is built on. So the build must run *inside* the Flatpak's
   `--devel` environment, which overlays the SDK at `/usr` and exposes `/app`.

2. **The sandbox redirects install locations.** GIMP running under Flatpak
   does **not** read plug-ins from the native `~/.local/share/gegl-0.4` or
   `~/.config/GIMP/3.0` paths. Instead:

   | Component   | Install location                                                |
   |-------------|-----------------------------------------------------------------|
   | GEGL op     | `~/.var/app/org.gimp.GIMP/data/gegl-0.4/plug-ins/`              |
   | GIMP plug-in| `~/.config/GIMP/<MAJOR.MINOR>/plug-ins/gimp-palette-quantize/`  |

   - The GEGL op goes in the **per-app data** dir, because `XDG_DATA_HOME` is
     redirected into `~/.var/app/.../data`.
   - The GIMP plug-in goes in the **real** `~/.config/GIMP` tree, because the
     `xdg-config/GIMP` Flatpak permission bind-mounts it into the sandbox.
   - `<MAJOR.MINOR>` is the GIMP version's config dir — **`3.2`** for GIMP
     3.2.x (not `3.0`). The script derives this automatically.

## Prerequisites

- The GIMP Flatpak: `flatpak install flathub org.gimp.GIMP`
- The matching GNOME SDK (the script installs it for you, into the **user**
  flatpak installation). For GIMP 3.2.4 this is `org.gnome.Sdk//50`.

## The script (`build-install-flatpak.sh`)

It is idempotent and broken into subcommands so you can replay any step:

```bash
./build-install-flatpak.sh deps      # install the GNOME SDK that matches GIMP's runtime
./build-install-flatpak.sh build     # meson setup (first time) + meson compile
./build-install-flatpak.sh install   # copy artifacts to the Flatpak locations
./build-install-flatpak.sh verify    # confirm the GEGL op + plug-in registered
./build-install-flatpak.sh clean     # rm -rf build/
./build-install-flatpak.sh           # = deps + build + install + verify
```

The SDK branch and the GIMP config version are detected from
`flatpak info org.gimp.GIMP`, so the script keeps working across GIMP/runtime
upgrades without edits.

## Doing it manually

If you prefer to run the steps by hand:

```bash
# 1. Install the SDK matching GIMP's runtime (50 for GIMP 3.2.4).
flatpak install --user flathub org.gnome.Sdk//50

# 2. Build inside the Flatpak --devel environment.
flatpak run --devel --filesystem=host --command=bash org.gimp.GIMP -c '
  export PKG_CONFIG_PATH=/app/lib/pkgconfig
  cd '"$PWD"'
  meson setup build
  meson compile -C build
'

# 3. Install into the Flatpak-visible locations.
mkdir -p ~/.var/app/org.gimp.GIMP/data/gegl-0.4/plug-ins
cp build/src/gegl-palette-quantize.so ~/.var/app/org.gimp.GIMP/data/gegl-0.4/plug-ins/

mkdir -p ~/.config/GIMP/3.2/plug-ins/gimp-palette-quantize
cp build/src/gimp-palette-quantize ~/.config/GIMP/3.2/plug-ins/gimp-palette-quantize/
chmod +x ~/.config/GIMP/3.2/plug-ins/gimp-palette-quantize/gimp-palette-quantize
```

## Verifying the install

```bash
# GEGL operation registered?
flatpak run --command=sh org.gimp.GIMP -c 'gegl --list-all' | grep palette-quantize
# -> custom:palette-quantize

# GIMP plug-in discovered? (run GIMP once, then check pluginrc)
flatpak run org.gimp.GIMP -i -b '(gimp-quit 0)'
grep -a plug-in-palette-quantize-group ~/.config/GIMP/3.2/pluginrc
# -> (plug-in-def ".../gimp-palette-quantize" ... (proc-def "plug-in-palette-quantize-group" ...
```

## Notes for hacking

- **The two binaries are Flatpak-bound.** They link against the GEGL/babl/GIMP
  libraries inside `/app`, so they only run inside the GIMP Flatpak sandbox.
  Don't expect them to load in a host GIMP.
- **Source layout:** `src/gegl-palette-quantize.c` is the GEGL point-filter op
  (GIMP-independent); `src/gimp-palette-quantize.c` is the GIMP 3 plug-in
  wrapper that converts a chosen `GimpPalette` into the op's inline hex string.
- **Rebuild loop while iterating:**
  ```bash
  ./build-install-flatpak.sh build && ./build-install-flatpak.sh install
  ```
  Only changing the GEGL op? You can skip the GIMP plug-in copy. Note that the
  non-destructive layer filter caches the GEGL op graph, so fully restart GIMP
  after replacing the `.so`.
