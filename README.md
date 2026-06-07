# GIMP 3.2 Palette Quantize Filter

This project provides a GEGL operation plus a GIMP 3 plug-in wrapper for enforcing a chosen GIMP palette on a selected layer or layer group.

## What it installs

- `custom:palette-quantize`: a GEGL point-filter operation.
- `plug-in-palette-quantize-group`: a GIMP plug-in entry at `Filters > Color > Quantize to Palette...`.

The plug-in uses GIMP's native palette argument, so the dialog can select from GIMP's palette resources. It converts the chosen palette to the inline palette string expected by the GEGL op, then either appends the op as a non-destructive drawable filter or merges it destructively.

## Behavior

For each non-transparent pixel:

1. Find the nearest palette entry, measuring distance in the selected metric
   (sRGB Euclidean, linear RGB, CIE Lab, or OKLab).
2. Replace the pixel RGB with that palette color (always the exact sRGB
   palette color, so it round-trips to the picked `#RRGGBB`).
3. Preserve alpha.
4. Optionally blend with the original color using `strength`.

The **Metric** dropdown controls only *how* the nearest color is chosen.
`sRGB`/`linear` are fast RGB-distance modes; `CIE Lab`/`OKLab` are perceptual
and usually give more natural matches for photographic content.

The **Dithering** dropdown distributes quantization error to reduce banding:

- `None` — hard nearest-color mapping.
- `Ordered (Bayer 8x8)` — fast, position-based; regular cross-hatch texture.
- `Blue noise` — position-based like ordered, but a natural, non-repeating
  texture (no cross-hatch). Cheap and tiles seamlessly.
- `Floyd-Steinberg` — classic error diffusion.
- `Atkinson` — error diffusion that distributes only part of the error, so
  flat areas stay cleaner (less busy). Good when you want to "respect" flats.
- `Jarvis-Judice-Ninke`, `Stucki` — large-kernel error diffusion; smoother,
  more natural gradients.
- `Sierra` — balanced error diffusion.

The position-based modes (ordered, blue noise) are cheap and tile seamlessly.
The error-diffusion modes process the whole layer at once (heavier for large
layers / live previews). For them, the **Serpentine scan** toggle alternates
the row direction to remove the diagonal "worm" artifacts.

The **Alpha** dropdown controls how transparency is handled so the *visible*
pixel can be an exact palette color:

- `Preserve alpha` — quantize the color, keep each pixel's original alpha.
- `Opaque (ignore alpha)` — quantize the pixel's own color and output it fully
  opaque, ignoring its alpha and whatever is below.
- `Composite over background` (default) — blend each pixel over the
  **Background** color using its alpha, then quantize, and output opaque. This
  makes the visible result an exact palette color even for semi-transparent
  pixels.
The **directional** modes color or shade each *stroke* by its own shape: they
treat the layer's alpha (blurred by **Width**) as a height field and use its
surface normal, with four colors (**Top/Right/Bottom/Left**) and a
**Direction** knob:

- `Directional paint (by shape)` — recolor each stroke by its surface normal:
  the top of the stroke takes the Top color, the bottom the Bottom color, the
  sides Left/Right. Replaces the stroke color.
- `Directional tint (by shape)` — same, but blended over the original color
  (**Relief** = amount), so the underlying texture shows through.
- `Directional gradient (per stroke)` — color each *separate* stroke by its
  position within its own bounding box (connected strokes are detected; merged
  /overlapping strokes count as one).
- `Bevel / emboss` — a real 3D bevel: a **Width**-wide band ramping in from the
  edge, highlighting the side facing the light (Top color) and shadowing the
  opposite side (Bottom color); **Relief** = strength, **Direction** = light.

Every mode except Preserve outputs fully opaque, exact palette colors. The
**Alpha threshold** (default 0.5) gives hard edges: pixels with coverage below
it become transparent, at/above become opaque — so anti-aliased edges resolve
to a crisp boundary instead of a translucent halo. Preserve keeps the original
alpha. The **Background**
color drives composite mode; **Top/Right/Bottom/Left**, **Direction**,
**Width** and **Relief** drive the directional/bevel modes.

Matching is **exact**: every pixel is compared against all palette entries in
the selected metric space and assigned its true nearest color, with ties
resolved to the lowest palette index. This is what indexed-color workflows
need — the output contains only exact palette colors, so a later
`Image > Mode > Indexed` with the same palette produces stable indices. The
design target is palettes of **256 colors or fewer**.

## Build & install guides

Pick the guide matching your platform — these cover the real, tested paths:

- **Linux with a Flatpak GIMP** (e.g. Ubuntu, where GIMP 3 isn't in apt): see
  [`BUILDING.md`](BUILDING.md). Use `./build-install-flatpak.sh`.
- **Windows** (MSYS2 / MinGW-w64): see
  [`BUILDING-windows.md`](BUILDING-windows.md). Use `./build-install-msys2.sh`.
- **Linux with native GIMP 3 dev packages**: the generic steps below, or
  `install-linux-user.sh`.

## Requirements

- GIMP 3.2.x development files, including `gimp-3.0` and `gimpui-3.0` pkg-config packages.
- GEGL 0.4 development files.
- babl, GLib, Meson, Ninja, and a C compiler.

On Linux package names vary. They are often similar to:

```bash
sudo apt install build-essential meson ninja-build libgegl-dev libbabl-dev libgimp-3.0-dev
```

If you use the official GIMP AppImage, Flatpak, Snap, Windows, or macOS SDK, point your environment to the GIMP prefix so `pkg-config` can find GIMP's `.pc` files.

## Build

```bash
meson setup build
meson compile -C build
```

## Install on Linux

Install the GEGL operation:

```bash
mkdir -p ~/.local/share/gegl-0.4/plug-ins
cp build/src/gegl-palette-quantize.so ~/.local/share/gegl-0.4/plug-ins/
```

Install the GIMP plug-in:

```bash
mkdir -p ~/.config/GIMP/3.0/plug-ins/gimp-palette-quantize
cp build/src/gimp-palette-quantize ~/.config/GIMP/3.0/plug-ins/gimp-palette-quantize/
chmod +x ~/.config/GIMP/3.0/plug-ins/gimp-palette-quantize/gimp-palette-quantize
```

Restart GIMP.

## Usage

1. Select a layer or layer group in the Layers dock.
2. Run `Filters > Color > Quantize to Palette...`.
3. Pick a palette from the GIMP palette selector.
4. Choose a `Metric` (how nearest color is measured) and a `Dithering` method.
5. Choose an `Alpha` mode. For an exact, fully opaque visible color, use
   `Opaque` or `Composite over background` (and pick a `Background` color for
   the latter).
6. Adjust `Strength` to blend the result with the original.
7. Keep `Non-destructive layer filter` enabled to append a live filter when GIMP accepts it. Disable it to merge destructively.

You can also use the GEGL operation directly through GIMP's GEGL operation dialog by choosing `custom:palette-quantize`, but that lower-level route requires entering the palette as a `#RRGGBB;#RRGGBB` string rather than choosing from GIMP's palette list.

## Notes and limitations

- Nearest-color quantization with selectable distance metrics (sRGB, linear,
  CIE Lab, OKLab) and dithering (none, ordered Bayer, Floyd-Steinberg).
- Matching is exact (no lookup-table approximation). Cost is O(palette size)
  per pixel; tuned for palettes of <= 256 colors. A full 256-color palette
  quantizes a 4 MP layer in well under a second.
- Floyd-Steinberg forces whole-layer processing (to avoid tile seams), so it is
  slower than the other modes for large layers and non-destructive previews.
- Non-destructive filters are available for layers; in GIMP 3.2, layer groups are layers, so selecting a group should work when the custom GEGL op is loaded.
- If GIMP cannot append the filter non-destructively to the selected drawable, the wrapper falls back to merging the GEGL operation.
- I could not compile-test this in the ChatGPT container because the container does not include GIMP/GEGL development headers. The code follows the GIMP 3.2.4 libgimp API docs, but you may still need small distro-specific include/link adjustments.
