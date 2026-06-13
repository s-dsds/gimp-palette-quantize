# Development notes / handoff

Context for continuing work on this plugin in a fresh session. (User: sylvodsds.)
For build/install see `BUILDING.md` (Linux/Flatpak) and `BUILDING-windows.md`.

## What this is

Two pieces, joined by an inline `#rrggbb;...` palette string:

- `src/gegl-palette-quantize.c` — a GEGL **filter** op (`custom:palette-quantize`),
  GIMP-independent. Maps each pixel to the nearest palette color.
- `src/gimp-palette-quantize.c` — the GIMP 3 plug-in wrapper
  (`plug-in-palette-quantize-group`, menu **Filters > Color > Quantize to
  Palette…**). Converts a chosen `GimpPalette` → hex string and appends/merges
  the op on the selected drawable.

## Feature set (current)

- **Metric** (nearest-color distance space): `srgb`, `linear`, `cie-lab`, `oklab`.
- **Dithering**: `none`, `ordered` (Bayer 8x8), `blue-noise` (interleaved
  gradient noise), `floyd-steinberg`, `atkinson`, `jarvis`, `stucki`, `sierra`,
  plus a **serpentine** toggle for the error-diffusion kernels.
- **Alpha**: `opaque` (default — ignore alpha, output opaque), `preserve`,
  `composite` (over a Background color), `dir-paint` / `dir-tint` (recolor each
  stroke by its alpha-height-field surface normal), `bevel` (3D bevel/emboss).
  Directional modes use four colors (`color-top/right/bottom/left`) +
  `direction` (deg) + `width` (px) + `relief` (depth); bevel uses Top=highlight,
  Bottom=shadow. A per-stroke bounding-box gradient mode (`dir-bbox`) existed in
  ≤0.8.x and was removed in 0.9.0.
- **Strength**: blend original ↔ quantized.

The interactive dialog hides mode-irrelevant controls: `show_dialog()` connects
to `notify::alpha`/`notify::dither` on the config and toggles widget visibility
(`gimp_procedure_dialog_get_widget` + `gtk_widget_set_no_show_all`/`set_visible`).

Matching is **exact** (O(n) scan in the metric space, ties → lowest index),
tuned for palettes ≤ 256 colors. Output is always an exact palette color, so a
later `Image > Mode > Indexed` with the same palette gives stable indices.

## Architecture notes

- `prepare()` builds a `PaletteCache` (palette in sRGB + metric space, dither
  amplitude, background + directional colors, direction/relief) once; it is the
  only place that touches `o->user_data` (process() runs multi-threaded for the
  tileable modes). Freed in `finalize`.
- `build_src()` produces the per-pixel match/blend **source** color for *every*
  alpha mode, so dithering composes with all alpha modes uniformly. The rest
  (match → dither → write, set output alpha) is mode-agnostic.
- `build_src_normal()` (dir-paint/dir-tint/bevel): the alpha is box-blurred
  **3×** (≈Gaussian) into a height field, and a **Sobel** 3×3 gradient gives the
  surface normal. Both matter for the bevel: a 2-pass box blur + 2-tap central
  difference left axis-aligned facets/"vertical cuts"; 3-pass + Sobel make the
  bevel a smooth ramp. Bevel shade gain ≈ `1.5 * (width+1)` so it ramps across
  the band instead of saturating at the edge.
- `convert_to_metric()` is the metric conversion: sRGB = identity, OKLab =
  **manual** (see gotcha), linear/CIE-Lab = babl fish.
- Whole-image processing is forced (`needs_whole_image`) for the error-diffusion
  dithers (avoid tile seams) and `directional-edge` (reads neighbor pixels);
  everything else tiles and runs in parallel.

## Gotchas (learned the hard way)

1. **GIMP mirrors a GEGL enum property into the drawable-filter config as a
   `GimpChoice` nick STRING.** Pass enum values to `gimp_drawable_*_new_filter`
   as `const gchar*` nicks, NOT the int id (int id → bogus pointer → segfault;
   value 0 silently no-ops). The plug-in's `GimpChoice` nicks deliberately match
   the GEGL enum nicks. Colors pass through as `GeglColor` (same type both sides).
2. **babl's `"Oklab float"` is broken** in this stack (GIMP 3.2.4 / babl 0.1.126):
   garbage for saturated colors, nonzero a/b for neutrals. We compute OKLab
   manually (`rgb_to_oklab`). CIE Lab via babl is fine.
3. **GEGL property enum names**: single-word names used to avoid `_`↔`-`
   canonicalization ambiguity across the GEGL/GIMP boundary.
4. **`enum_end()`** localizes labels via `dgettext(GETTEXT_PACKAGE, …)`, so the
   op `#define`s `GETTEXT_PACKAGE` and includes `<libintl.h>` before `gegl-op.h`.
5. **Always reinstall before testing** — standalone GEGL loads the *installed*
   `.so` from `~/.var/app/org.gimp.GIMP/data/gegl-0.4/plug-ins`, not `build/`.
   An out-of-range enum set silently falls back to 0 (read it back to verify).

## Testing approach

- GEGL op in isolation: tiny C program linking `gegl-0.4`/`babl-0.1` inside the
  Flatpak `--devel` env (`flatpak run --devel ... -lm`), build a
  buffer-source → `custom:palette-quantize` → write-buffer/png-save graph. Pass
  enums as ints, colors via `gegl_color_new`.
- Through GIMP: headless script-fu batch calling the PDB procedure
  (`plug-in-palette-quantize-group RUN-NONINTERACTIVE image (vector layer) …`).
  PDB arg order = registration order: palette, strength, non-destructive,
  metric, dither, serpentine, alpha, background, color-top/right/bottom/left,
  direction, relief.

## Possible next steps

- Interior/height-field relief (current emboss only keys off edges).
- True blue-noise tile (current is interleaved-gradient-noise, not spectral).
- Optional k-d tree for exact matching at large palette sizes / faster FS.
- Alpha threshold so near-transparent edge pixels can stay transparent.
- A `samples/` generator tool (the sheets were rendered ad-hoc in /tmp).
