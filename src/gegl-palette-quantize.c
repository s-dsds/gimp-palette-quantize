/*
 * gegl-palette-quantize.c
 *
 * GEGL filter: map every pixel to the nearest color from an inline
 * semicolon/comma/whitespace separated palette string, e.g.
 *   #000000;#ffffff;#ff0044
 *
 * Features:
 *   - distance metric: sRGB / linear RGB / CIE Lab / OKLab
 *   - dithering:       none / ordered (Bayer 8x8) / Floyd-Steinberg
 *   - alpha handling:  preserve / opaque / composite over a background color
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
  enum_value (GEGL_PQ_DITHER_NONE,    "none",            "None")
  enum_value (GEGL_PQ_DITHER_ORDERED, "ordered",         "Ordered (Bayer 8x8)")
  enum_value (GEGL_PQ_DITHER_FS,      "floyd-steinberg", "Floyd-Steinberg")
enum_end (GeglPaletteQuantizeDither)

enum_start (gegl_palette_quantize_alpha)
  enum_value (GEGL_PQ_ALPHA_PRESERVE,  "preserve",  "Preserve alpha")
  enum_value (GEGL_PQ_ALPHA_OPAQUE,    "opaque",    "Opaque (ignore alpha)")
  enum_value (GEGL_PQ_ALPHA_COMPOSITE, "composite", "Composite over background")
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

property_enum (alpha, "Alpha",
               GeglPaletteQuantizeAlpha, gegl_palette_quantize_alpha,
               GEGL_PQ_ALPHA_COMPOSITE)
  description ("How to treat alpha so the visible color is an exact palette color")

property_color (background, "Background", "white")
  description ("Backdrop color used by the 'composite over background' alpha mode")

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
  const Babl *from_rgb;  /* fish: "R'G'B' float" -> metric space (3 ch)      */
  gfloat      amp;       /* ordered-dither amplitude (mean palette spacing)  */
  gfloat      bg[3];     /* background color (sRGB), for composite mode      */
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
  c->from_rgb = babl_fish (babl_format ("R'G'B' float"), fmt);
  c->bg[0] = c->bg[1] = c->bg[2] = 1.0f;   /* default white; set in prepare */

  for (guint i = 0; i < n; i++)
    {
      PaletteColor pc = g_array_index (colors, PaletteColor, i);
      c->srgb[i * 3 + 0] = pc.r;
      c->srgb[i * 3 + 1] = pc.g;
      c->srgb[i * 3 + 2] = pc.b;
    }

  /* Palette in the metric space (used for both matching and dithering). */
  babl_process (c->from_rgb, c->srgb, c->coord, n);

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

/* Build the straight-RGB color that will be matched and used as the blend
 * source, per alpha mode. For 'composite', blend over the background color. */
static void
build_src (const PaletteCache *c, const gfloat *buf, gfloat *src,
           glong npix, gint amode)
{
  for (glong i = 0; i < npix; i++)
    {
      gfloat a = buf[i * 4 + 3];

      if (amode == GEGL_PQ_ALPHA_COMPOSITE && a < 1.0f)
        {
          gfloat ia = 1.0f - a;
          src[i * 3 + 0] = buf[i * 4 + 0] * a + c->bg[0] * ia;
          src[i * 3 + 1] = buf[i * 4 + 1] * a + c->bg[1] * ia;
          src[i * 3 + 2] = buf[i * 4 + 2] * a + c->bg[2] * ia;
        }
      else
        {
          src[i * 3 + 0] = buf[i * 4 + 0];
          src[i * 3 + 1] = buf[i * 4 + 1];
          src[i * 3 + 2] = buf[i * 4 + 2];
        }
    }
}

static inline gfloat
out_alpha (gint amode, gfloat a)
{
  return (amode == GEGL_PQ_ALPHA_PRESERVE) ? a : 1.0f;
}

static void
process_simple (const PaletteCache *c, gfloat *buf, glong w, glong h,
                glong ox, glong oy, gfloat strength, gboolean ordered, gint amode)
{
  glong   npix = w * h;
  gfloat *src  = g_new (gfloat, (gsize) npix * 3);   /* match & blend source  */
  gfloat *cm   = g_new (gfloat, (gsize) npix * 3);   /* src in metric space   */

  build_src (c, buf, src, npix, amode);
  babl_process (c->from_rgb, src, cm, npix);

  for (glong y = 0; y < h; y++)
    for (glong x = 0; x < w; x++)
      {
        gsize         p  = (gsize) y * w + x;
        gfloat       *px = buf + p * 4;
        const gfloat *s  = src + p * 3;
        const gfloat *m  = cm + p * 3;
        gfloat        a  = px[3];
        gfloat        mm[3];
        const gfloat *q;
        guint         idx;

        if (a <= 0.0f)
          continue;  /* fully transparent stays transparent */

        if (ordered)
          {
            gint   bx  = (gint) ((ox + x) & 7);
            gint   by  = (gint) ((oy + y) & 7);
            gfloat off = (((bayer8[by][bx] + 0.5f) / 64.0f) - 0.5f) * c->amp;
            mm[0] = m[0] + off; mm[1] = m[1] + off; mm[2] = m[2] + off;
            idx = nearest_exact (mm, c);
          }
        else
          {
            idx = nearest_exact (m, c);
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

static void
process_floyd_steinberg (const PaletteCache *c, gfloat *buf, glong w, glong h,
                         gfloat strength, gint amode)
{
  glong   npix = w * h;
  gfloat *src  = g_new (gfloat, (gsize) npix * 3);   /* blend source          */
  gfloat *wm   = g_new (gfloat, (gsize) npix * 3);   /* working metric values */

  build_src (c, buf, src, npix, amode);
  babl_process (c->from_rgb, src, wm, npix);

  for (glong y = 0; y < h; y++)
    for (glong x = 0; x < w; x++)
      {
        gsize         p   = (gsize) y * w + x;
        gfloat       *px  = buf + p * 4;
        const gfloat *s   = src + p * 3;
        gfloat       *m   = wm + p * 3;
        gfloat        a   = px[3];
        const gfloat *q;
        guint         idx;
        gfloat        e0, e1, e2;

        if (a <= 0.0f)
          continue;  /* leave transparent pixels untouched, no diffusion */

        idx = nearest_exact (m, c);

        e0 = m[0] - c->coord[idx * 3 + 0];
        e1 = m[1] - c->coord[idx * 3 + 1];
        e2 = m[2] - c->coord[idx * 3 + 2];

        /* Floyd-Steinberg distribution (in metric space): 7/16,3/16,5/16,1/16 */
        if (x + 1 < w)
          {
            gfloat *nb = wm + (p + 1) * 3;
            nb[0] += e0 * (7.0f / 16.0f);
            nb[1] += e1 * (7.0f / 16.0f);
            nb[2] += e2 * (7.0f / 16.0f);
          }
        if (y + 1 < h)
          {
            if (x > 0)
              {
                gfloat *nb = wm + (p + w - 1) * 3;
                nb[0] += e0 * (3.0f / 16.0f);
                nb[1] += e1 * (3.0f / 16.0f);
                nb[2] += e2 * (3.0f / 16.0f);
              }
            {
              gfloat *nb = wm + (p + w) * 3;
              nb[0] += e0 * (5.0f / 16.0f);
              nb[1] += e1 * (5.0f / 16.0f);
              nb[2] += e2 * (5.0f / 16.0f);
            }
            if (x + 1 < w)
              {
                gfloat *nb = wm + (p + w + 1) * 3;
                nb[0] += e0 * (1.0f / 16.0f);
                nb[1] += e1 * (1.0f / 16.0f);
                nb[2] += e2 * (1.0f / 16.0f);
              }
          }

        q = c->srgb + idx * 3;
        px[0] = s[0] + (q[0] - s[0]) * strength;
        px[1] = s[1] + (q[1] - s[1]) * strength;
        px[2] = s[2] + (q[2] - s[2]) * strength;
        px[3] = out_alpha (amode, a);
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
  gfloat             *buf;
  gfloat              strength;
  gint                amode;

  (void) level;

  if (! c || roi->width <= 0 || roi->height <= 0)
    return TRUE;

  strength = CLAMP ((gfloat) o->strength, 0.0f, 1.0f);
  amode    = o->alpha;

  buf = g_new (gfloat, (gsize) roi->width * roi->height * 4);
  gegl_buffer_get (input, roi, 1.0, format, buf,
                   GEGL_AUTO_ROWSTRIDE, GEGL_ABYSS_CLAMP);

  switch (o->dither)
    {
    case GEGL_PQ_DITHER_FS:
      process_floyd_steinberg (c, buf, roi->width, roi->height, strength, amode);
      break;
    case GEGL_PQ_DITHER_ORDERED:
      process_simple (c, buf, roi->width, roi->height,
                      roi->x, roi->y, strength, TRUE, amode);
      break;
    case GEGL_PQ_DITHER_NONE:
    default:
      process_simple (c, buf, roi->width, roi->height,
                      roi->x, roi->y, strength, FALSE, amode);
      break;
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

  if (o->dither == GEGL_PQ_DITHER_FS)
    {
      GeglRectangle *in = gegl_operation_source_get_bounding_box (operation, "input");
      if (in)
        return *in;
    }
  return *roi;
}

static GeglRectangle
get_cached_region (GeglOperation       *operation,
                   const GeglRectangle *roi)
{
  GeglProperties *o = GEGL_PROPERTIES (operation);

  if (o->dither == GEGL_PQ_DITHER_FS)
    {
      GeglRectangle *in = gegl_operation_source_get_bounding_box (operation, "input");
      if (in)
        return *in;
    }
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
