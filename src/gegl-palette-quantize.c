/*
 * gegl-palette-quantize.c
 *
 * GEGL filter: map every pixel to the nearest color from an inline
 * semicolon/comma/whitespace separated palette string, e.g.
 *   #000000;#ffffff;#ff0044
 *
 * Features:
 *   - distance metric: sRGB / linear RGB / CIE Lab / OKLab
 *   - dithering:       none / ordered (Bayer) / blue noise / Floyd-Steinberg /
 *                      Atkinson / Jarvis / Stucki / Sierra (+ serpentine)
 *   - alpha handling:  preserve / opaque / composite over a background color /
 *                      directional paint / directional tint / per-stroke
 *                      gradient / bevel — the directional modes derive a
 *                      surface normal from the (blurred) alpha height field and
 *                      use 4 directional colors + direction + width + relief
 *   - strength:        blend between original and quantized result
 *
 * Matching is EXACT: each pixel is compared against every palette entry in the
 * selected metric space and assigned its true nearest color (ties resolved to
 * the lowest palette index). This is what indexed-color workflows need — the
 * result contains only exact palette colors, so a later "Image > Mode >
 * Indexed" with the same palette assigns stable indices. The design target is
 * palettes of <= 256 colors, so the per-pixel O(n) scan is cheap.
 *
 * Alpha handling controls what the *visible* pixel becomes:
 *   - preserve:  quantize the straight RGB, keep the original alpha.
 *   - opaque:    quantize the straight RGB, force the output fully opaque
 *                (ignores both the alpha and whatever is below).
 *   - composite: blend the pixel over the background color using its alpha,
 *                then quantize, and force the output opaque — so the visible
 *                color is an exact palette color.
 * In the opaque/composite modes, fully transparent pixels (alpha == 0) are
 * left transparent.
 *
 * This operation is deliberately GIMP-independent. The companion GIMP plug-in
 * converts a selected GimpPalette into this inline string and appends/merges
 * the operation as a layer or group filter.
 */

#include <glib.h>
#include <gegl.h>
#include <gegl-plugin.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef GEGL_PROPERTIES

enum_start (gegl_palette_quantize_metric)
  enum_value (GEGL_PQ_METRIC_SRGB,   "srgb",    "sRGB Euclidean")
  enum_value (GEGL_PQ_METRIC_LINEAR, "linear",  "Linear RGB")
  enum_value (GEGL_PQ_METRIC_LAB,    "cie-lab", "CIE Lab (perceptual)")
  enum_value (GEGL_PQ_METRIC_OKLAB,  "oklab",   "OKLab (perceptual)")
enum_end (GeglPaletteQuantizeMetric)

enum_start (gegl_palette_quantize_dither)
  enum_value (GEGL_PQ_DITHER_NONE,      "none",            "None")
  enum_value (GEGL_PQ_DITHER_ORDERED,   "ordered",         "Ordered (Bayer 8x8)")
  enum_value (GEGL_PQ_DITHER_BLUENOISE, "blue-noise",      "Blue noise")
  enum_value (GEGL_PQ_DITHER_FS,        "floyd-steinberg", "Floyd-Steinberg")
  enum_value (GEGL_PQ_DITHER_ATKINSON,  "atkinson",        "Atkinson")
  enum_value (GEGL_PQ_DITHER_JJN,       "jarvis",          "Jarvis-Judice-Ninke")
  enum_value (GEGL_PQ_DITHER_STUCKI,    "stucki",          "Stucki")
  enum_value (GEGL_PQ_DITHER_SIERRA,    "sierra",          "Sierra")
enum_end (GeglPaletteQuantizeDither)

enum_start (gegl_palette_quantize_alpha)
  enum_value (GEGL_PQ_ALPHA_PRESERVE,  "preserve",  "Preserve alpha")
  enum_value (GEGL_PQ_ALPHA_OPAQUE,    "opaque",    "Opaque (ignore alpha)")
  enum_value (GEGL_PQ_ALPHA_COMPOSITE, "composite", "Composite over background")
  enum_value (GEGL_PQ_ALPHA_DIR_PAINT, "dir-paint", "Directional paint (by shape)")
  enum_value (GEGL_PQ_ALPHA_DIR_TINT,  "dir-tint",  "Directional tint (by shape)")
  enum_value (GEGL_PQ_ALPHA_BEVEL,     "bevel",     "Bevel / emboss")
enum_end (GeglPaletteQuantizeAlpha)

property_string (palette, "Palette", "#000000;#ffffff")
  description ("Palette colors as separated #RRGGBB values")

property_enum (metric, "Metric",
               GeglPaletteQuantizeMetric, gegl_palette_quantize_metric,
               GEGL_PQ_METRIC_SRGB)
  description ("Color space in which nearest-color distance is measured")

property_enum (dither, "Dithering",
               GeglPaletteQuantizeDither, gegl_palette_quantize_dither,
               GEGL_PQ_DITHER_NONE)
  description ("Dithering method used to distribute quantization error")

property_boolean (serpentine, "Serpentine", FALSE)
  description ("Alternate row direction for the error-diffusion dithers (reduces directional 'worm' artifacts)")

property_enum (alpha, "Alpha",
               GeglPaletteQuantizeAlpha, gegl_palette_quantize_alpha,
               GEGL_PQ_ALPHA_OPAQUE)
  description ("How to treat alpha so the visible color is an exact palette color")

property_color (background, "Background", "white")
  description ("Backdrop color used by the 'composite over background' alpha mode")

property_color (color_top, "Top color", "white")
  description ("Directional color for the top / light direction")
property_color (color_right, "Right color", "gray")
  description ("Directional color for the right direction")
property_color (color_bottom, "Bottom color", "black")
  description ("Directional color for the bottom / shadow direction")
property_color (color_left, "Left color", "gray")
  description ("Directional color for the left direction")

property_double (direction, "Direction", 0.0)
  value_range (0.0, 360.0)
  ui_range    (0.0, 360.0)
  description ("Lighting/orientation direction (degrees) for the directional and bevel modes")

property_double (width, "Width", 4.0)
  value_range (0.0, 64.0)
  ui_range    (0.0, 32.0)
  description ("Spread/bevel width in pixels (directional 'by shape' and bevel modes)")

property_double (relief, "Relief", 0.5)
  value_range (0.0, 1.0)
  ui_range    (0.0, 1.0)
  description ("Strength of the directional tint / bevel shading")

property_double (threshold, "Alpha threshold", 0.5)
  value_range (0.0, 1.0)
  ui_range    (0.0, 1.0)
  description ("Hard-edge coverage cutoff: pixels below become transparent, at/above become opaque (all modes except Preserve)")

property_double (strength, "Strength", 1.0)
  value_range (0.0, 1.0)
  ui_range    (0.0, 1.0)
  ui_meta     ("unit", "ratio")
  description ("Blend between original and quantized color")

#else

#define GEGL_OP_FILTER
#define GEGL_OP_NAME     palette_quantize
#define GEGL_OP_C_SOURCE gegl-palette-quantize.c

/* The enum_end() registration macro localizes enum labels via
 * dgettext(GETTEXT_PACKAGE, ...), so both must be available here. */
#define GETTEXT_PACKAGE "gegl-palette-quantize"
#include <libintl.h>

#include "gegl-op.h"

typedef struct
{
  gfloat r;
  gfloat g;
  gfloat b;
} PaletteColor;

/* Parsed-and-prepared palette, cached on the operation. Built once in
 * prepare() (single-threaded); process() only reads it. */
typedef struct
{
  gchar      *palette;   /* copy of the source string, for change detection  */
  gint        metric;    /* metric the coords were built for                 */
  guint       n;         /* number of palette colors                         */
  gfloat     *srgb;      /* n * 3, sRGB 0..1 — written to the output         */
  gfloat     *coord;     /* n * 3, palette colors in the metric space        */
  const Babl *fish;      /* fish: "R'G'B' float" -> metric (linear/Lab only) */
  gfloat      amp;       /* ordered-dither amplitude (mean palette spacing)  */
  gfloat      bg[3];     /* background color (sRGB), for composite mode      */
  gfloat      dir_col[4][3]; /* directional colors sRGB: 0=top 1=right 2=bottom 3=left */
  gfloat      dir;       /* lighting/orientation direction, radians          */
  gfloat      relief;    /* tint/bevel strength                              */
  gfloat      width;     /* spread/bevel width in pixels                     */
  gfloat      threshold; /* hard-edge alpha cutoff (non-preserve modes)      */
} PaletteCache;

static const gint bayer8[8][8] = {
  {  0, 32,  8, 40,  2, 34, 10, 42 },
  { 48, 16, 56, 24, 50, 18, 58, 26 },
  { 12, 44,  4, 36, 14, 46,  6, 38 },
  { 60, 28, 52, 20, 62, 30, 54, 22 },
  {  3, 35, 11, 43,  1, 33,  9, 41 },
  { 51, 19, 59, 27, 49, 17, 57, 25 },
  { 15, 47,  7, 39, 13, 45,  5, 37 },
  { 63, 31, 55, 23, 61, 29, 53, 21 }
};

/* Interleaved gradient noise (Jorge Jimenez): a cheap, table-free dither with
 * blue-noise-like spectral character — far more natural than Bayer. */
static inline gfloat
ign (gint x, gint y)
{
  gfloat f = 0.06711056f * (gfloat) x + 0.00583715f * (gfloat) y;
  return fmodf (52.9829189f * fmodf (f, 1.0f), 1.0f);
}

/* A position-based dither offset in [-0.5, 0.5) * amp, added to the metric
 * coordinates before matching. pattern: 0 = none, 1 = Bayer, 2 = blue noise. */
static inline gfloat
ordered_offset (gint pattern, gint x, gint y, gfloat amp)
{
  switch (pattern)
    {
    case 1:  return (((bayer8[y & 7][x & 7] + 0.5f) / 64.0f) - 0.5f) * amp;
    case 2:  return (ign (x, y) - 0.5f) * amp;
    default: return 0.0f;
    }
}

/* Error-diffusion kernels: forward taps (dy > 0, or dy == 0 && dx > 0). */
typedef struct { gint dx, dy, w; } DiffTap;

static const DiffTap K_FS[]  = { {1,0,7},{-1,1,3},{0,1,5},{1,1,1} };
static const DiffTap K_ATK[] = { {1,0,1},{2,0,1},{-1,1,1},{0,1,1},{1,1,1},{0,2,1} };
static const DiffTap K_JJN[] = { {1,0,7},{2,0,5},{-2,1,3},{-1,1,5},{0,1,7},{1,1,5},
                                 {2,1,3},{-2,2,1},{-1,2,3},{0,2,5},{1,2,3},{2,2,1} };
static const DiffTap K_STK[] = { {1,0,8},{2,0,4},{-2,1,2},{-1,1,4},{0,1,8},{1,1,4},
                                 {2,1,2},{-2,2,1},{-1,2,2},{0,2,4},{1,2,2},{2,2,1} };
static const DiffTap K_SIE[] = { {1,0,5},{2,0,3},{-2,1,2},{-1,1,4},{0,1,5},{1,1,4},
                                 {2,1,3},{-1,2,2},{0,2,3},{1,2,2} };

/* Atkinson divides by 8 but distributes only 6/8 of the error, deliberately
 * dropping 1/4 — this is what keeps flat areas clean. */
static void
diffusion_kernel (gint dither, const DiffTap **taps, gint *n, gint *divisor)
{
  switch (dither)
    {
    case GEGL_PQ_DITHER_ATKINSON: *taps = K_ATK; *n = G_N_ELEMENTS (K_ATK); *divisor = 8;  break;
    case GEGL_PQ_DITHER_JJN:      *taps = K_JJN; *n = G_N_ELEMENTS (K_JJN); *divisor = 48; break;
    case GEGL_PQ_DITHER_STUCKI:   *taps = K_STK; *n = G_N_ELEMENTS (K_STK); *divisor = 42; break;
    case GEGL_PQ_DITHER_SIERRA:   *taps = K_SIE; *n = G_N_ELEMENTS (K_SIE); *divisor = 32; break;
    case GEGL_PQ_DITHER_FS:
    default:                      *taps = K_FS;  *n = G_N_ELEMENTS (K_FS);  *divisor = 16; break;
    }
}

static inline gboolean
is_error_diffusion (gint dither)
{
  return dither >= GEGL_PQ_DITHER_FS;
}

/* Modes that need the whole image in one process() call: error diffusion (to
 * avoid tile seams) and the directional modes (they read neighbors / blur the
 * alpha / label connected strokes). */
static inline gboolean
needs_whole_image (gint dither, gint alpha)
{
  return is_error_diffusion (dither) || alpha >= GEGL_PQ_ALPHA_DIR_PAINT;
}

static const gchar *
metric_format (gint metric)
{
  switch (metric)
    {
    case GEGL_PQ_METRIC_LINEAR: return "RGB float";
    case GEGL_PQ_METRIC_LAB:    return "CIE Lab float";
    case GEGL_PQ_METRIC_OKLAB:  return "Oklab float";
    case GEGL_PQ_METRIC_SRGB:
    default:                    return "R'G'B' float";
    }
}

static gboolean
parse_hex_color (const gchar *s, PaletteColor *out)
{
  gchar buf[7];
  guint v;

  while (g_ascii_isspace (*s))
    s++;

  if (*s == '#')
    s++;

  if (strlen (s) < 6)
    return FALSE;

  memcpy (buf, s, 6);
  buf[6] = '\0';

  for (gint i = 0; i < 6; i++)
    if (! g_ascii_isxdigit (buf[i]))
      return FALSE;

  if (sscanf (buf, "%06x", &v) != 1)
    return FALSE;

  out->r = ((v >> 16) & 0xff) / 255.0f;
  out->g = ((v >>  8) & 0xff) / 255.0f;
  out->b = ((v      ) & 0xff) / 255.0f;

  return TRUE;
}

static GArray *
parse_palette (const gchar *palette)
{
  GArray *colors;
  gchar **parts;

  colors = g_array_new (FALSE, FALSE, sizeof (PaletteColor));

  if (! palette || ! *palette)
    palette = "#000000;#ffffff";

  parts = g_strsplit_set (palette, ";,\n\r\t ", -1);

  for (gchar **p = parts; p && *p; p++)
    {
      PaletteColor c;

      if (**p == '\0')
        continue;

      if (parse_hex_color (*p, &c))
        g_array_append_val (colors, c);
    }

  g_strfreev (parts);

  if (colors->len == 0)
    {
      PaletteColor black = { 0.0f, 0.0f, 0.0f };
      PaletteColor white = { 1.0f, 1.0f, 1.0f };
      g_array_append_val (colors, black);
      g_array_append_val (colors, white);
    }

  return colors;
}

static void
palette_cache_free (PaletteCache *c)
{
  if (! c)
    return;
  g_free (c->palette);
  g_free (c->srgb);
  g_free (c->coord);
  g_free (c);
}

static inline gfloat
sqdist3 (const gfloat *a, const gfloat *b)
{
  gfloat d0 = a[0] - b[0];
  gfloat d1 = a[1] - b[1];
  gfloat d2 = a[2] - b[2];
  return d0 * d0 + d1 * d1 + d2 * d2;
}

/* Exact nearest palette color in the metric space. Ties resolve to the lowest
 * index (strict '<'), giving deterministic, index-stable results. */
static inline guint
nearest_exact (const gfloat *p, const PaletteCache *c)
{
  guint  best   = 0;
  gfloat best_d = G_MAXFLOAT;

  for (guint i = 0; i < c->n; i++)
    {
      gfloat d = sqdist3 (p, c->coord + i * 3);
      if (d < best_d)
        {
          best_d = d;
          best   = i;
        }
    }
  return best;
}

/* sRGB -> linear for one channel. */
static inline gfloat
srgb_to_linear (gfloat c)
{
  return (c <= 0.04045f) ? c / 12.92f : powf ((c + 0.055f) / 1.055f, 2.4f);
}

/* Manual OKLab from sRGB (Björn Ottosson). babl's "Oklab float" is broken in
 * this version (it returns garbage for saturated colors and nonzero a/b for
 * neutrals), so we do it ourselves. */
static void
rgb_to_oklab (const gfloat *rgb, gfloat *o)
{
  gfloat r = srgb_to_linear (rgb[0]);
  gfloat g = srgb_to_linear (rgb[1]);
  gfloat b = srgb_to_linear (rgb[2]);

  gfloat l = 0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b;
  gfloat m = 0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b;
  gfloat s = 0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b;

  l = cbrtf (l); m = cbrtf (m); s = cbrtf (s);

  o[0] = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
  o[1] = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
  o[2] = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
}

/* Convert an interleaved R'G'B' (sRGB, 3 ch) buffer into the metric space. */
static void
convert_to_metric (const PaletteCache *c, const gfloat *srgb, gfloat *out, glong n)
{
  switch (c->metric)
    {
    case GEGL_PQ_METRIC_OKLAB:
      for (glong i = 0; i < n; i++)
        rgb_to_oklab (srgb + i * 3, out + i * 3);
      break;
    case GEGL_PQ_METRIC_SRGB:
      memcpy (out, srgb, (gsize) n * 3 * sizeof (gfloat));
      break;
    default:                          /* linear RGB, CIE Lab: babl is correct */
      babl_process (c->fish, srgb, out, n);
      break;
    }
}

static PaletteCache *
palette_cache_build (const gchar *palette_str, gint metric)
{
  GArray       *colors = parse_palette (palette_str);
  guint         n      = colors->len;
  PaletteCache *c      = g_new0 (PaletteCache, 1);
  const Babl   *fmt    = babl_format (metric_format (metric));

  c->palette  = g_strdup (palette_str ? palette_str : "");
  c->metric   = metric;
  c->n        = n;
  c->srgb     = g_new (gfloat, (gsize) n * 3);
  c->coord    = g_new (gfloat, (gsize) n * 3);
  c->fish     = babl_fish (babl_format ("R'G'B' float"), fmt);
  c->bg[0] = c->bg[1] = c->bg[2] = 1.0f;   /* default white; set in prepare */

  /* Directional defaults (overwritten in prepare): top white, sides gray,
   * bottom black — a plausible top-lit emboss. */
  c->dir_col[0][0] = c->dir_col[0][1] = c->dir_col[0][2] = 1.0f;  /* top    */
  c->dir_col[1][0] = c->dir_col[1][1] = c->dir_col[1][2] = 0.5f;  /* right  */
  c->dir_col[2][0] = c->dir_col[2][1] = c->dir_col[2][2] = 0.0f;  /* bottom */
  c->dir_col[3][0] = c->dir_col[3][1] = c->dir_col[3][2] = 0.5f;  /* left   */
  c->dir       = 0.0f;
  c->relief    = 0.5f;
  c->width     = 4.0f;
  c->threshold = 0.5f;

  for (guint i = 0; i < n; i++)
    {
      PaletteColor pc = g_array_index (colors, PaletteColor, i);
      c->srgb[i * 3 + 0] = pc.r;
      c->srgb[i * 3 + 1] = pc.g;
      c->srgb[i * 3 + 2] = pc.b;
    }

  /* Palette in the metric space (used for both matching and dithering). */
  convert_to_metric (c, c->srgb, c->coord, n);

  /* Ordered-dither amplitude: mean distance (in the metric space) from each
   * palette color to its nearest neighbor — a reasonable quantization step. */
  c->amp = 0.0f;
  if (n > 1)
    {
      gfloat acc = 0.0f;
      for (guint i = 0; i < n; i++)
        {
          gfloat best = G_MAXFLOAT;
          for (guint j = 0; j < n; j++)
            if (j != i)
              {
                gfloat d = sqdist3 (c->coord + i * 3, c->coord + j * 3);
                if (d < best) best = d;
              }
          acc += sqrtf (best);
        }
      c->amp = acc / n;
    }

  g_array_unref (colors);
  return c;
}

/* -------- per-mode processing over an interleaved R'G'B'A float buffer ----- */

/* Box-blur the alpha channel (separable, sliding window, clamped edges) into
 * hgt with `iters` passes of radius r. Used as a height field whose gradient
 * gives a surface normal — this is what lets the directional / bevel modes
 * shade a band ramping inward from the edge instead of a 1px rim. */
static void
blur_alpha (const gfloat *buf, glong w, glong h, gint r, gint iters, gfloat *hgt)
{
  gfloat *tmp;
  gfloat  inv;

  for (gsize i = 0; i < (gsize) w * h; i++)
    hgt[i] = buf[i * 4 + 3];

  if (r < 1 || iters < 1)
    return;

  tmp = g_new (gfloat, (gsize) w * h);
  inv = 1.0f / (2.0f * r + 1.0f);

  for (gint it = 0; it < iters; it++)
    {
      for (glong y = 0; y < h; y++)            /* horizontal: hgt -> tmp */
        {
          const gfloat *row = hgt + (gsize) y * w;
          gfloat       *out = tmp + (gsize) y * w;
          gfloat        acc = 0.0f;
          for (glong k = -r; k <= r; k++)
            acc += row[CLAMP (k, 0, w - 1)];
          for (glong x = 0; x < w; x++)
            {
              out[x] = acc * inv;
              acc += row[CLAMP (x + r + 1, 0, w - 1)] - row[CLAMP (x - r, 0, w - 1)];
            }
        }
      for (glong x = 0; x < w; x++)            /* vertical: tmp -> hgt */
        {
          gfloat acc = 0.0f;
          for (glong k = -r; k <= r; k++)
            acc += tmp[(gsize) CLAMP (k, 0, h - 1) * w + x];
          for (glong y = 0; y < h; y++)
            {
              hgt[(gsize) y * w + x] = acc * inv;
              acc += tmp[(gsize) CLAMP (y + r + 1, 0, h - 1) * w + x]
                   - tmp[(gsize) CLAMP (y - r, 0, h - 1) * w + x];
            }
        }
    }
  g_free (tmp);
}

/* Blend the four directional colors by a (screen-space) unit vector, rotated
 * by the lighting direction. Screen y points down; top = -y. */
static void
blend_dir (const PaletteCache *c, gfloat vx, gfloat vy, gfloat out[3])
{
  gfloat ca = cosf (-c->dir), sa = sinf (-c->dir);
  gfloat rx = vx * ca - vy * sa;
  gfloat ry = vx * sa + vy * ca;
  gfloat wR = fmaxf (0.0f, rx), wL = fmaxf (0.0f, -rx);
  gfloat wT = fmaxf (0.0f, -ry), wB = fmaxf (0.0f, ry);
  gfloat sum = wR + wL + wT + wB;

  if (sum < 1e-6f)
    {
      for (gint k = 0; k < 3; k++)
        out[k] = 0.25f * (c->dir_col[0][k] + c->dir_col[1][k] +
                          c->dir_col[2][k] + c->dir_col[3][k]);
      return;
    }

  for (gint k = 0; k < 3; k++)
    out[k] = (c->dir_col[0][k] * wT + c->dir_col[1][k] * wR +
              c->dir_col[2][k] * wB + c->dir_col[3][k] * wL) / sum;
}

/* Normal-based modes derive a surface normal from the (blurred) alpha height
 * field, so each stroke is shaded/colored by its own shape. dir-paint replaces
 * the color by the directional color; dir-tint blends it in; bevel shades a
 * highlight (Top color) / shadow (Bottom color) band by the light direction. */
static void
build_src_normal (const PaletteCache *c, const gfloat *buf, gfloat *src,
                  glong w, glong h, gint amode)
{
  gint    r     = (gint) (c->width + 0.5f);
  gfloat *hgt   = g_new (gfloat, (gsize) w * h);
  /* With a smooth (Gaussian-ish) height field the edge slope is ~1/width, so
   * scale by ~width to make the bevel shade ramp across the whole band rather
   * than saturating in the first pixel (which read as hard "cuts"). */
  gfloat  bgain = 1.5f * (c->width + 1.0f);
  gfloat  Lx    = cosf (c->dir);
  gfloat  Ly    = -sinf (c->dir);

  /* Three box-blur passes approximate a Gaussian; this removes the faceting /
   * axis-aligned steps that a 2-pass box blur leaves in the height field. */
  blur_alpha (buf, w, h, r, 3, hgt);

  for (glong y = 0; y < h; y++)
    for (glong x = 0; x < w; x++)
      {
        gsize         p   = (gsize) y * w + x;
        const gfloat *px  = buf + p * 4;
        gfloat       *d   = src + p * 3;
        glong         xm  = CLAMP (x - 1, 0, w - 1);
        glong         xp  = CLAMP (x + 1, 0, w - 1);
        glong         ym  = CLAMP (y - 1, 0, h - 1) * w;
        glong         yp  = CLAMP (y + 1, 0, h - 1) * w;
        glong         yc  = y * w;
        /* Sobel gradient (3x3): includes the diagonal neighbours, so the normal
         * varies smoothly instead of the axis-aligned banding that a 2-tap
         * central difference produces on a blurred edge. */
        gfloat gx = (hgt[ym + xp] + 2.0f * hgt[yc + xp] + hgt[yp + xp] -
                     hgt[ym + xm] - 2.0f * hgt[yc + xm] - hgt[yp + xm]) * 0.125f;
        gfloat gy = (hgt[yp + xm] + 2.0f * hgt[yp + x]  + hgt[yp + xp] -
                     hgt[ym + xm] - 2.0f * hgt[ym + x]  - hgt[ym + xp]) * 0.125f;
        gfloat nx = -gx, ny = -gy;            /* outward normal = -gradient */
        gfloat mag = sqrtf (nx * nx + ny * ny);

        if (amode == GEGL_PQ_ALPHA_BEVEL)
          {
            gfloat s = (nx * Lx + ny * Ly) * bgain;   /* >0 lit, <0 shadow  */
            const gfloat *hl = c->dir_col[0];         /* Top    = highlight */
            const gfloat *sh = c->dir_col[2];         /* Bottom = shadow    */
            if (s >= 0.0f)
              {
                gfloat wgt = CLAMP (s, 0.0f, 1.0f) * c->relief;
                d[0] = px[0] + (hl[0] - px[0]) * wgt;
                d[1] = px[1] + (hl[1] - px[1]) * wgt;
                d[2] = px[2] + (hl[2] - px[2]) * wgt;
              }
            else
              {
                gfloat wgt = CLAMP (-s, 0.0f, 1.0f) * c->relief;
                d[0] = px[0] + (sh[0] - px[0]) * wgt;
                d[1] = px[1] + (sh[1] - px[1]) * wgt;
                d[2] = px[2] + (sh[2] - px[2]) * wgt;
              }
          }
        else
          {
            /* paint/tint: color by the normal DIRECTION (not its steepness),
             * full-strength for paint, scaled by relief for tint. v~0 (flat)
             * falls back to the average of the four colors via blend_dir. */
            gfloat col[3];
            gfloat wgt = (amode == GEGL_PQ_ALPHA_DIR_TINT) ? c->relief : 1.0f;
            if (mag > 1e-6f) blend_dir (c, nx / mag, ny / mag, col);
            else            blend_dir (c, 0.0f, 0.0f, col);
            d[0] = px[0] + (col[0] - px[0]) * wgt;
            d[1] = px[1] + (col[1] - px[1]) * wgt;
            d[2] = px[2] + (col[2] - px[2]) * wgt;
          }
      }
  g_free (hgt);
}

/* Build the straight-RGB color that will be matched and used as the blend
 * source, per alpha mode. */
static void
build_src (const PaletteCache *c, const gfloat *buf, gfloat *src,
           glong w, glong h, glong ox, glong oy, gint amode,
           gfloat bcx, gfloat bcy)
{
  (void) ox; (void) oy; (void) bcx; (void) bcy;

  if (amode == GEGL_PQ_ALPHA_DIR_PAINT ||
      amode == GEGL_PQ_ALPHA_DIR_TINT  ||
      amode == GEGL_PQ_ALPHA_BEVEL)
    {
      build_src_normal (c, buf, src, w, h, amode);
      return;
    }

  for (glong y = 0; y < h; y++)
    for (glong x = 0; x < w; x++)
      {
        gsize         p  = (gsize) y * w + x;
        const gfloat *px = buf + p * 4;
        gfloat        a  = px[3];
        gfloat       *d  = src + p * 3;

        if (amode == GEGL_PQ_ALPHA_COMPOSITE && a < 1.0f)
          {
            gfloat ia = 1.0f - a;
            d[0] = px[0] * a + c->bg[0] * ia;
            d[1] = px[1] * a + c->bg[1] * ia;
            d[2] = px[2] * a + c->bg[2] * ia;
          }
        else
          {
            d[0] = px[0]; d[1] = px[1]; d[2] = px[2];
          }
      }
}

/* Only 'preserve' keeps the original alpha. Every other mode outputs a fully
 * opaque exact palette color for visible pixels (alpha > 0); fully transparent
 * pixels are left transparent (handled by the caller skipping a <= 0). */
static inline gfloat
out_alpha (gint amode, gfloat a)
{
  return (amode == GEGL_PQ_ALPHA_PRESERVE) ? a : 1.0f;
}

/* Non-diffusing modes: none (pattern 0), ordered Bayer (1), blue noise (2). */
static void
process_ordered (const PaletteCache *c, gfloat *buf, glong w, glong h,
                 glong ox, glong oy, gfloat strength, gint pattern, gint amode,
                 gfloat bcx, gfloat bcy)
{
  glong   npix = w * h;
  gfloat *src  = g_new (gfloat, (gsize) npix * 3);   /* match & blend source  */
  gfloat *cm   = g_new (gfloat, (gsize) npix * 3);   /* src in metric space   */

  build_src (c, buf, src, w, h, ox, oy, amode, bcx, bcy);
  convert_to_metric (c, src, cm, npix);

  for (glong y = 0; y < h; y++)
    for (glong x = 0; x < w; x++)
      {
        gsize         p  = (gsize) y * w + x;
        gfloat       *px = buf + p * 4;
        const gfloat *s  = src + p * 3;
        const gfloat *m  = cm + p * 3;
        gfloat        a  = px[3];
        const gfloat *q;
        guint         idx;

        if (amode == GEGL_PQ_ALPHA_PRESERVE)
          {
            if (a <= 0.0f) continue;        /* keep the soft alpha */
          }
        else if (a < c->threshold)
          {
            px[3] = 0.0f;                    /* hard edge: below cutoff clears */
            continue;
          }

        if (pattern == 0)
          {
            idx = nearest_exact (m, c);
          }
        else
          {
            gfloat off = ordered_offset (pattern, (gint) (ox + x), (gint) (oy + y), c->amp);
            gfloat mm[3];
            mm[0] = m[0] + off; mm[1] = m[1] + off; mm[2] = m[2] + off;
            idx = nearest_exact (mm, c);
          }

        q = c->srgb + idx * 3;
        px[0] = s[0] + (q[0] - s[0]) * strength;
        px[1] = s[1] + (q[1] - s[1]) * strength;
        px[2] = s[2] + (q[2] - s[2]) * strength;
        px[3] = out_alpha (amode, a);
      }

  g_free (src);
  g_free (cm);
}

/* Error-diffusion modes, generic over the kernel, with optional serpentine
 * scanning. Diffuses in the metric space; matching stays exact. */
static void
process_diffuse (const PaletteCache *c, gfloat *buf, glong w, glong h,
                 glong ox, glong oy, gfloat strength, gint amode, gint dither,
                 gboolean serpentine, gfloat bcx, gfloat bcy)
{
  glong          npix = w * h;
  gfloat        *src  = g_new (gfloat, (gsize) npix * 3);   /* blend source     */
  gfloat        *wm   = g_new (gfloat, (gsize) npix * 3);   /* working metric   */
  const DiffTap *taps;
  gint           ntaps, divisor;

  diffusion_kernel (dither, &taps, &ntaps, &divisor);

  build_src (c, buf, src, w, h, ox, oy, amode, bcx, bcy);
  convert_to_metric (c, src, wm, npix);

  for (glong y = 0; y < h; y++)
    {
      gboolean l2r = ! (serpentine && (y & 1));

      for (glong k = 0; k < w; k++)
        {
          glong         x   = l2r ? k : (w - 1 - k);
          gsize         p   = (gsize) y * w + x;
          gfloat       *px  = buf + p * 4;
          const gfloat *s   = src + p * 3;
          gfloat       *m   = wm + p * 3;
          gfloat        a   = px[3];
          const gfloat *q;
          guint         idx;
          gfloat        e0, e1, e2;

          if (amode == GEGL_PQ_ALPHA_PRESERVE)
            {
              if (a <= 0.0f) continue;       /* keep the soft alpha */
            }
          else if (a < c->threshold)
            {
              px[3] = 0.0f;                   /* hard edge: below cutoff clears */
              continue;
            }

          idx = nearest_exact (m, c);

          e0 = m[0] - c->coord[idx * 3 + 0];
          e1 = m[1] - c->coord[idx * 3 + 1];
          e2 = m[2] - c->coord[idx * 3 + 2];

          for (gint t = 0; t < ntaps; t++)
            {
              gint   dx = l2r ? taps[t].dx : -taps[t].dx;
              glong  nx = x + dx;
              glong  ny = y + taps[t].dy;
              gfloat wf;
              gfloat *nb;

              if (nx < 0 || nx >= w || ny < 0 || ny >= h)
                continue;

              wf = (gfloat) taps[t].w / (gfloat) divisor;
              nb = wm + ((gsize) ny * w + nx) * 3;
              nb[0] += e0 * wf;
              nb[1] += e1 * wf;
              nb[2] += e2 * wf;
            }

          q = c->srgb + idx * 3;
          px[0] = s[0] + (q[0] - s[0]) * strength;
          px[1] = s[1] + (q[1] - s[1]) * strength;
          px[2] = s[2] + (q[2] - s[2]) * strength;
          px[3] = out_alpha (amode, a);
        }
    }

  g_free (src);
  g_free (wm);
}

static gboolean
process (GeglOperation       *operation,
         GeglBuffer          *input,
         GeglBuffer          *output,
         const GeglRectangle *roi,
         gint                 level)
{
  GeglProperties     *o      = GEGL_PROPERTIES (operation);
  const PaletteCache *c      = o->user_data;
  const Babl         *format = babl_format ("R'G'B'A float");
  GeglRectangle      *bbox;
  gfloat             *buf;
  gfloat              strength;
  gfloat              bcx, bcy;
  gint                amode;

  (void) level;

  if (! c || ! roi || roi->width <= 0 || roi->height <= 0)
    return TRUE;

  strength = CLAMP ((gfloat) o->strength, 0.0f, 1.0f);
  amode    = o->alpha;

  /* Layer center (absolute coords) for the position-based directional mode. */
  bbox = gegl_operation_source_get_bounding_box (operation, "input");
  if (bbox && bbox->width > 0 && bbox->height > 0)
    {
      bcx = bbox->x + bbox->width  * 0.5f;
      bcy = bbox->y + bbox->height * 0.5f;
    }
  else
    {
      bcx = roi->x + roi->width  * 0.5f;
      bcy = roi->y + roi->height * 0.5f;
    }

  buf = g_new (gfloat, (gsize) roi->width * roi->height * 4);
  gegl_buffer_get (input, roi, 1.0, format, buf,
                   GEGL_AUTO_ROWSTRIDE, GEGL_ABYSS_CLAMP);

  if (is_error_diffusion (o->dither))
    {
      process_diffuse (c, buf, roi->width, roi->height, roi->x, roi->y,
                       strength, amode, o->dither, o->serpentine, bcx, bcy);
    }
  else
    {
      gint pattern = (o->dither == GEGL_PQ_DITHER_ORDERED)   ? 1 :
                     (o->dither == GEGL_PQ_DITHER_BLUENOISE) ? 2 : 0;
      process_ordered (c, buf, roi->width, roi->height,
                       roi->x, roi->y, strength, pattern, amode, bcx, bcy);
    }

  gegl_buffer_set (output, roi, 0, format, buf, GEGL_AUTO_ROWSTRIDE);
  g_free (buf);

  return TRUE;
}

/* Floyd-Steinberg must see the whole image in one process() call, otherwise
 * error diffusion would produce visible seams at tile boundaries. */
static GeglRectangle
get_required_for_output (GeglOperation       *operation,
                         const gchar         *input_pad,
                         const GeglRectangle *roi)
{
  GeglProperties *o = GEGL_PROPERTIES (operation);

  (void) input_pad;

  if (needs_whole_image (o->dither, o->alpha))
    {
      GeglRectangle *in = gegl_operation_source_get_bounding_box (operation, "input");
      if (in)
        return *in;
    }
  if (! roi)                       /* defensive: never deref a NULL roi */
    return *GEGL_RECTANGLE (0, 0, 0, 0);
  return *roi;
}

static GeglRectangle
get_cached_region (GeglOperation       *operation,
                   const GeglRectangle *roi)
{
  GeglProperties *o = GEGL_PROPERTIES (operation);

  if (needs_whole_image (o->dither, o->alpha))
    {
      GeglRectangle *in = gegl_operation_source_get_bounding_box (operation, "input");
      if (in)
        return *in;
    }
  if (! roi)                       /* defensive: never deref a NULL roi */
    return *GEGL_RECTANGLE (0, 0, 0, 0);
  return *roi;
}

static void
prepare (GeglOperation *operation)
{
  GeglProperties *o      = GEGL_PROPERTIES (operation);
  const Babl     *format = babl_format ("R'G'B'A float");
  PaletteCache   *c;

  gegl_operation_set_format (operation, "input", format);
  gegl_operation_set_format (operation, "output", format);

  /* (Re)build the palette cache whenever the inputs change. prepare() is
   * single-threaded, so this is the safe place to touch user_data. */
  palette_cache_free (o->user_data);
  c = palette_cache_build (o->palette, o->metric);

  if (o->background)
    gegl_color_get_pixel (o->background, babl_format ("R'G'B' float"), c->bg);

  if (o->color_top)
    gegl_color_get_pixel (o->color_top,    babl_format ("R'G'B' float"), c->dir_col[0]);
  if (o->color_right)
    gegl_color_get_pixel (o->color_right,  babl_format ("R'G'B' float"), c->dir_col[1]);
  if (o->color_bottom)
    gegl_color_get_pixel (o->color_bottom, babl_format ("R'G'B' float"), c->dir_col[2]);
  if (o->color_left)
    gegl_color_get_pixel (o->color_left,   babl_format ("R'G'B' float"), c->dir_col[3]);

  c->dir       = (gfloat) (o->direction * G_PI / 180.0);
  c->relief    = (gfloat) o->relief;
  c->width     = (gfloat) o->width;
  c->threshold = (gfloat) o->threshold;

  o->user_data = c;
}

static void
finalize (GObject *object)
{
  GeglProperties *o = GEGL_PROPERTIES (object);

  palette_cache_free (o->user_data);
  o->user_data = NULL;

  G_OBJECT_CLASS (gegl_op_parent_class)->finalize (object);
}

static void
gegl_op_class_init (GeglOpClass *klass)
{
  GObjectClass             *object_class;
  GeglOperationClass       *operation_class;
  GeglOperationFilterClass *filter_class;

  object_class    = G_OBJECT_CLASS (klass);
  operation_class = GEGL_OPERATION_CLASS (klass);
  filter_class    = GEGL_OPERATION_FILTER_CLASS (klass);

  object_class->finalize                   = finalize;
  operation_class->prepare                 = prepare;
  operation_class->get_required_for_output = get_required_for_output;
  operation_class->get_cached_region       = get_cached_region;
  filter_class->process                    = process;

  gegl_operation_class_set_keys (operation_class,
                                 "name",        "custom:palette-quantize",
                                 "title",       "Palette Quantize",
                                 "categories",  "color:palette",
                                 "description", "Map every pixel to the nearest color in a supplied palette, with optional dithering and alpha handling.",
                                 NULL);
}

#endif
