# Building & Installing on Windows (MSYS2 / MinGW-w64)

GIMP plug-ins on Windows are built with **MSYS2 / MinGW-w64** — the same
toolchain the official GIMP Windows builds use. The source is portable; you do
**not** need to change any code. You build a `.dll` (the GEGL op) and a `.exe`
(the GIMP plug-in).

## TL;DR

1. Install MSYS2, open the **UCRT64** shell.
2. Copy this project folder onto the Windows box (or `git clone` it).
3. In the UCRT64 shell, `cd` into it and run:
   ```bash
   ./build-install-msys2.sh
   ```
4. Fully restart GIMP → **Filters → Color → Quantize to Palette…**

---

## 1. Install MSYS2

Download and run the installer from <https://www.msys2.org/> (accept defaults).
From the Start menu open **“MSYS2 UCRT64”** (blue icon).

> Use the **UCRT64** shell, not the plain “MSYS” shell. UCRT64 is MSYS2's
> modern default and matches recent GIMP packaging. (If you specifically need
> to match an older GIMP, the **MINGW64** shell works too — the script adapts
> to whichever environment shell you launch.)

Update the package database once (it may ask you to close and reopen the shell):
```bash
pacman -Syu
```

## 2. Get the project onto Windows

Copy the whole `gimp-palette-quantize` folder to the Windows machine, e.g. to
`C:\Users\<you>\gimp-palette-quantize`. In the UCRT64 shell that path is
`/c/Users/<you>/gimp-palette-quantize`:
```bash
cd /c/Users/<you>/gimp-palette-quantize
```

## 3. Install build dependencies

```bash
./build-install-msys2.sh deps
```
This runs `pacman -S` for the toolchain and libraries, using the prefix of your
current shell (`$MINGW_PACKAGE_PREFIX`, e.g. `mingw-w64-ucrt-x86_64`):
gcc, meson, ninja, pkgconf, **gimp**, **gegl**, **babl**.

Sanity check that the dev files are visible:
```bash
pkg-config --modversion gimp-3.0 gegl-0.4 babl-0.1
```

## 4. Build

```bash
./build-install-msys2.sh build
```
Produces:
- `build/src/gegl-palette-quantize.dll` — the GEGL operation
- `build/src/gimp-palette-quantize.exe` — the GIMP plug-in

(Or run the whole thing manually: `meson setup build && meson compile -C build`.)

## 5. Install

```bash
./build-install-msys2.sh install
```
This copies the artifacts to the per-user Windows locations:

| Component   | Destination                                                       |
|-------------|-------------------------------------------------------------------|
| GEGL op     | `%LOCALAPPDATA%\gegl-0.4\plug-ins\gegl-palette-quantize.dll`       |
| GIMP plug-in| `%APPDATA%\GIMP\<VER>\plug-ins\gimp-palette-quantize\gimp-palette-quantize.exe` |

`<VER>` defaults to the build GIMP's `major.minor`. **If the GIMP you actually
run is a different version**, override it:
```bash
GIMP_VER=3.2 ./build-install-msys2.sh install
```
The authoritative plug-in folder is shown in GIMP under
**Edit → Preferences → Folders → Plug-ins** — you can always drop the
`gimp-palette-quantize\gimp-palette-quantize.exe` (in its own subfolder) into
any folder listed there.

Then **fully quit and restart GIMP**.

## Verifying

- **Filters → Color → Quantize to Palette…** should appear, with the Metric
  dropdown (sRGB / Linear / CIE Lab / OKLab).
- If the menu item is missing, GIMP didn't load the plug-in `.exe` — check
  Preferences → Folders → Plug-ins and that the `.exe` is in its own subfolder.
- If the filter runs but errors that the operation `custom:palette-quantize`
  is unknown, GEGL didn't find the op `.dll`. Make sure it's in
  `%LOCALAPPDATA%\gegl-0.4\plug-ins\`, or set `GEGL_PATH` to that folder via
  GIMP's environment (see below).

### Forcing the GEGL op path (fallback)

If `%LOCALAPPDATA%\gegl-0.4\plug-ins` isn't picked up, create/edit
`%APPDATA%\GIMP\<VER>\environ\palette-quantize.env` with:
```
GEGL_PATH=${GEGL_PATH}:C:\Users\<you>\AppData\Local\gegl-0.4\plug-ins
```
and restart GIMP.

## Important: match the GIMP you run

The cleanest setup is to **build against the same GIMP you'll run**. Two cases:

- **You'll use MSYS2's GIMP** (`pacman -S $MINGW_PACKAGE_PREFIX-gimp`, launched
  from the UCRT64 shell): everything matches automatically. Simplest for
  testing.
- **You'll use the official GIMP installer build**: the MSYS2-built plug-in
  normally works because both are MinGW-w64 builds, **but only if the library
  versions line up** (same GIMP 3.x major.minor, compatible GEGL/babl/GLib).
  If the official GIMP is much newer/older than MSYS2's packages, the plug-in
  may fail to load. In that case, either update MSYS2 (`pacman -Syu`) so its
  GIMP matches, or build against the installer's own dev files if available.

Plug-in/op DLL dependencies (GEGL, babl, GLib, …) are resolved at runtime from
GIMP's own `bin` directory, which GIMP puts on the search path when it launches
the plug-in — so you don't need to copy those DLLs yourself, as long as the
versions are compatible.

## Notes

- No source changes are needed vs. the Linux/Flatpak build — same `meson.build`.
  Meson emits a `.dll` for the `shared_module` and an `.exe` for the executable
  automatically on Windows.
- See `BUILDING.md` for the Linux/Flatpak path and `README.md` for usage.
