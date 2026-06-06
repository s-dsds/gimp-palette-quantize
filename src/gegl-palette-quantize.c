/*
 * gegl-palette-quantize.c
 *
 * GEGL point filter: map every pixel to the nearest color from an inline
 * semicolon/comma/whitespace separated palette string, e.g.
 *   #000000;#ffffff;#ff0044
 *
 * Distance can be measured in sRGB, linear RGB, CIE Lab or OKLab. The output
 * is always the exact palette color (in sRGB), so it round-trips back to the
 * #RRGGBB the user picked; only the *matching* uses the chosen metric.
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

property_string (palette, "Palette", "#000000;#ffffff")
  description ("Palette colors as separated #RRGGBB values")

property_enum (metric, "Metric",
               GeglPaletteQuantizeMetric, gegl_palette_quantize_metric,
               GEGL_PQ_METRIC_SRGB)
  description ("Color space in which nearest-color distance is measured")

property_double (strength, "Strength", 1.0)
  value_range (0.0, 1.0)
  ui_range    (0.0, 1.0)
  ui_meta     ("unit", "ratio")
  description ("Blend between original and quantized color")

#else

#define GEGL_OP_POINT_FILTER
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

/* Parsed-and-prepared palette, cached on the operation between tiles. Built
 * once in prepare() (single-threaded); process() only reads it, which is
 * required because point-filter process() runs concurrently across tiles. */
typedef struct
{
  gchar      *palette;   /* copy of the source string, for change detection  */
  gint        metric;    /* metric the coords were built for                 */
  guint       n;         /* number of palette colors                         */
  gfloat     *srgb;      /* n * 3, sRGB 0..1 — written to the output         */
  gfloat     *coord;     /* n * 3, palette colors in the metric space        */
  const Babl *to_coord;  /* fish: "R'G'B'A float" -> metric space (3 ch)     */
} PaletteCache;

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

static PaletteCache *
palette_cache_build (const gchar *palette_str, gint metric)
{
  GArray       *colors = parse_palette (palette_str);
  guint         n      = colors->len;
  PaletteCache *c      = g_new0 (PaletteCache, 1);
  const Babl   *fmt    = babl_format (metric_format (metric));

  c->palette = g_strdup (palette_str ? palette_str : "");
  c->metric  = metric;
  c->n       = n;
  c->srgb    = g_new (gfloat, (gsize) n * 3);
  c->coord   = g_new (gfloat, (gsize) n * 3);

  for (guint i = 0; i < n; i++)
    {
      PaletteColor pc = g_array_index (colors, PaletteColor, i);
      c->srgb[i * 3 + 0] = pc.r;
      c->srgb[i * 3 + 1] = pc.g;
      c->srgb[i * 3 + 2] = pc.b;
    }

  /* Convert the whole palette into the metric space once. */
  babl_process (babl_fish (babl_format ("R'G'B' float"), fmt),
                c->srgb, c->coord, n);

  /* Fish used per tile to convert incoming pixels (drops alpha). */
  c->to_coord = babl_fish (babl_format ("R'G'B'A float"), fmt);

  g_array_unref (colors);
  return c;
}

static inline guint
nearest_index (const gfloat *p, const PaletteCache *c)
{
  guint  best   = 0;
  gfloat best_d = G_MAXFLOAT;

  for (guint i = 0; i < c->n; i++)
    {
      const gfloat *q  = c->coord + i * 3;
      gfloat        d0 = p[0] - q[0];
      gfloat        d1 = p[1] - q[1];
      gfloat        d2 = p[2] - q[2];
      gfloat        d  = d0 * d0 + d1 * d1 + d2 * d2;

      if (d < best_d)
        {
          best_d = d;
          best   = i;
        }
    }

  return best;
}

static gboolean
process (GeglOperation       *operation,
         void                *in_buf,
         void                *out_buf,
         glong                n_pixels,
         const GeglRectangle *roi,
         gint                 level)
{
  GeglProperties     *o = GEGL_PROPERTIES (operation);
  const PaletteCache *c = o->user_data;
  const gfloat       *src = in_buf;
  gfloat             *dst = out_buf;
  gfloat             *coords;
  gfloat              strength;

  (void) roi;
  (void) level;

  if (! c)   /* should have been built in prepare(); defensive only */
    return TRUE;

  strength = CLAMP ((gfloat) o->strength, 0.0f, 1.0f);

  /* Convert this tile's pixels into the metric space in one shot. */
  coords = g_new (gfloat, (gsize) n_pixels * 3);
  babl_process (c->to_coord, in_buf, coords, n_pixels);

  for (glong i = 0; i < n_pixels; i++)
    {
      const gfloat a = src[3];

      if (a <= 0.0f)
        {
          dst[0] = src[0];
          dst[1] = src[1];
          dst[2] = src[2];
          dst[3] = a;
        }
      else
        {
          guint        idx = nearest_index (coords + i * 3, c);
          const gfloat *q  = c->srgb + idx * 3;

          dst[0] = src[0] + (q[0] - src[0]) * strength;
          dst[1] = src[1] + (q[1] - src[1]) * strength;
          dst[2] = src[2] + (q[2] - src[2]) * strength;
          dst[3] = a;
        }

      src += 4;
      dst += 4;
    }

  g_free (coords);

  return TRUE;
}

static void
prepare (GeglOperation *operation)
{
  GeglProperties *o      = GEGL_PROPERTIES (operation);
  const Babl     *format = babl_format ("R'G'B'A float");

  gegl_operation_set_format (operation, "input", format);
  gegl_operation_set_format (operation, "output", format);

  /* (Re)build the palette cache whenever the inputs change. prepare() is
   * single-threaded, so this is the safe place to touch user_data. */
  palette_cache_free (o->user_data);
  o->user_data = palette_cache_build (o->palette, o->metric);
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
  GObjectClass                  *object_class;
  GeglOperationClass            *operation_class;
  GeglOperationPointFilterClass *point_filter_class;

  object_class       = G_OBJECT_CLASS (klass);
  operation_class    = GEGL_OPERATION_CLASS (klass);
  point_filter_class = GEGL_OPERATION_POINT_FILTER_CLASS (klass);

  object_class->finalize       = finalize;
  operation_class->prepare     = prepare;
  point_filter_class->process  = process;

  gegl_operation_class_set_keys (operation_class,
                                 "name",        "custom:palette-quantize",
                                 "title",       "Palette Quantize",
                                 "categories",  "color:palette",
                                 "description", "Map every pixel to the nearest color in a supplied palette.",
                                 NULL);
}

#endif
