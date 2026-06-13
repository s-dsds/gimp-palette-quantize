/*
 * gimp-palette-quantize.c
 *
 * GIMP 3.2+ plug-in wrapper for the GEGL operation
 *   custom:palette-quantize
 *
 * It shows a native GIMP palette selector, converts the selected GimpPalette
 * to the GEGL op's inline hex palette string, then appends or merges the GEGL
 * filter on the selected drawable/layer/group.
 */

#include <glib.h>
#include <math.h>
#include <string.h>
#include <libgimp/gimp.h>
#include <libgimp/gimpui.h>
#include <gegl.h>
#include <babl/babl.h>

#define PLUG_IN_PROC   "plug-in-palette-quantize-group"
#define PLUG_IN_BINARY "gimp-palette-quantize"
#define GEGL_OP_NAME   "custom:palette-quantize"

struct _PaletteQuantize
{
  GimpPlugIn parent_instance;
};

#define PALETTE_QUANTIZE_TYPE (palette_quantize_get_type ())
G_DECLARE_FINAL_TYPE (PaletteQuantize, palette_quantize, PALETTE, QUANTIZE, GimpPlugIn)

static GList          * palette_quantize_query_procedures (GimpPlugIn           *plug_in);
static GimpProcedure  * palette_quantize_create_procedure (GimpPlugIn           *plug_in,
                                                           const gchar          *name);
static GimpValueArray * palette_quantize_run              (GimpProcedure        *procedure,
                                                           GimpRunMode           run_mode,
                                                           GimpImage            *image,
                                                           GimpDrawable        **drawables,
                                                           GimpProcedureConfig  *config,
                                                           gpointer              run_data);

G_DEFINE_TYPE (PaletteQuantize, palette_quantize, GIMP_TYPE_PLUG_IN)
GIMP_MAIN (PALETTE_QUANTIZE_TYPE)

/* Per-palette-color inclusion flags set by the dialog's swatch grid and read by
 * palette_to_hex_string(). NULL / length mismatch => use every color. */
static gboolean *g_included   = NULL;
static gint      g_included_n = 0;

/* When re-editing an existing filter, the colors of that filter's current
 * palette (parsed from its stored hex string) so the swatch grid can show them
 * even though we have no GimpPalette resource for them. */
static gchar    *g_edit_hex      = NULL;   /* existing filter's palette string */
static gboolean  g_palette_chosen = FALSE; /* user picked a palette in the chooser */

/* The grid's final displayed colors at dialog close, so run() can build the
 * output hex from exactly what was shown (covers the "kept the existing palette"
 * case where the GimpPalette chooser doesn't reflect those colors). */
static gfloat   *g_final_rgb = NULL;
static gint      g_final_n   = 0;

#define GEGL_OP_PALETTE_SEPS ";,\n\r\t "

/* Parse a "#rrggbb;..." string into n*3 sRGB floats (0..1). Returns NULL if
 * no colors parse; *n set to the count. */
static gfloat *
parse_hex_to_rgb (const gchar *hex, gint *n_out)
{
  GArray *arr = g_array_new (FALSE, FALSE, sizeof (gfloat));
  gchar **parts;

  *n_out = 0;
  if (! hex || ! *hex)
    {
      g_array_unref (arr);
      return NULL;
    }

  parts = g_strsplit_set (hex, GEGL_OP_PALETTE_SEPS, -1);
  for (gchar **p = parts; p && *p; p++)
    {
      const gchar *s = *p;
      guint        v;
      gfloat       c;
      while (*s == ' ' || *s == '\t') s++;
      if (*s == '#') s++;
      if (strlen (s) < 6) continue;
      if (sscanf (s, "%06x", &v) != 1) continue;
      c = ((v >> 16) & 0xff) / 255.0f; g_array_append_val (arr, c);
      c = ((v >>  8) & 0xff) / 255.0f; g_array_append_val (arr, c);
      c = ((v      ) & 0xff) / 255.0f; g_array_append_val (arr, c);
    }
  g_strfreev (parts);

  *n_out = (gint) arr->len / 3;
  if (*n_out == 0)
    {
      g_array_unref (arr);
      return NULL;
    }
  return (gfloat *) g_array_free (arr, FALSE);
}

/* Build a hex palette string from the grid's final colors (honoring per-color
 * exclusions). Returns NULL when there were no grid colors (e.g. non-interactive
 * runs), in which case the caller falls back to the GimpPalette resource. */
static gchar *
hex_from_final_colors (void)
{
  GString *out;

  if (! g_final_rgb || g_final_n <= 0)
    return NULL;

  out = g_string_sized_new ((gsize) g_final_n * 8);
  for (gint i = 0; i < g_final_n; i++)
    {
      if (g_included && g_included_n == g_final_n && ! g_included[i])
        continue;
      if (out->len > 0) g_string_append_c (out, ';');
      g_string_append_printf (out, "#%02x%02x%02x",
                              (guint) (CLAMP (g_final_rgb[i*3],   0.0f, 1.0f) * 255.0f + 0.5f),
                              (guint) (CLAMP (g_final_rgb[i*3+1], 0.0f, 1.0f) * 255.0f + 0.5f),
                              (guint) (CLAMP (g_final_rgb[i*3+2], 0.0f, 1.0f) * 255.0f + 0.5f));
    }
  if (out->len == 0)   /* everything excluded -> fall back to all colors */
    for (gint i = 0; i < g_final_n; i++)
      {
        if (i > 0) g_string_append_c (out, ';');
        g_string_append_printf (out, "#%02x%02x%02x",
                                (guint) (CLAMP (g_final_rgb[i*3],   0.0f, 1.0f) * 255.0f + 0.5f),
                                (guint) (CLAMP (g_final_rgb[i*3+1], 0.0f, 1.0f) * 255.0f + 0.5f),
                                (guint) (CLAMP (g_final_rgb[i*3+2], 0.0f, 1.0f) * 255.0f + 0.5f));
      }
  return g_string_free (out, FALSE);
}

/* First existing drawable filter that is our quantize op, or NULL. */
static GimpDrawableFilter *
find_existing_filter (GimpDrawable *drawable)
{
  GimpDrawableFilter **filters = gimp_drawable_get_filters (drawable);
  GimpDrawableFilter  *found   = NULL;

  if (! filters)
    return NULL;

  for (gint i = 0; filters[i] != NULL; i++)
    {
      gchar *op = gimp_drawable_filter_get_operation_name (filters[i]);
      if (! found && g_strcmp0 (op, GEGL_OP_NAME) == 0)
        found = filters[i];
      g_free (op);
    }
  g_free (filters);   /* transfer-container: free the array, not the filters */
  return found;
}

static void
palette_quantize_class_init (PaletteQuantizeClass *klass)
{
  GimpPlugInClass *plug_in_class = GIMP_PLUG_IN_CLASS (klass);

  plug_in_class->query_procedures = palette_quantize_query_procedures;
  plug_in_class->create_procedure = palette_quantize_create_procedure;
}

static void
palette_quantize_init (PaletteQuantize *self)
{
  (void) self;
}

static GList *
palette_quantize_query_procedures (GimpPlugIn *plug_in)
{
  (void) plug_in;
  return g_list_append (NULL, g_strdup (PLUG_IN_PROC));
}

static GimpProcedure *
palette_quantize_create_procedure (GimpPlugIn  *plug_in,
                                   const gchar *name)
{
  GimpProcedure *procedure = NULL;

  if (g_strcmp0 (name, PLUG_IN_PROC) != 0)
    return NULL;

  procedure = gimp_image_procedure_new (plug_in, name,
                                        GIMP_PDB_PROC_TYPE_PLUGIN,
                                        palette_quantize_run,
                                        NULL, NULL);

  gimp_procedure_set_image_types (procedure, "RGB*, GRAY*");
  gimp_procedure_set_sensitivity_mask (procedure,
                                       GIMP_PROCEDURE_SENSITIVE_DRAWABLE |
                                       GIMP_PROCEDURE_SENSITIVE_DRAWABLES);
  gimp_procedure_set_menu_label (procedure, "Quantize to _Palette...");
  gimp_procedure_add_menu_path (procedure, "<Image>/Filters/Color/");
  gimp_procedure_set_documentation (procedure,
                                    "Limit the selected layer or layer group to a GIMP palette",
                                    "Adds or merges a GEGL filter that maps every visible pixel "
                                    "of the selected drawable to the nearest color in a selected "
                                    "GIMP palette. Alpha is preserved.",
                                    NULL);
  gimp_procedure_set_attribution (procedure,
                                  "OpenAI ChatGPT",
                                  "OpenAI ChatGPT",
                                  "2026");

  gimp_procedure_add_palette_argument (procedure,
                                       "palette",
                                       "_Palette",
                                       "Palette whose colors should be enforced",
                                       FALSE,
                                       NULL,
                                       TRUE,
                                       G_PARAM_READWRITE);

  gimp_procedure_add_double_argument (procedure,
                                      "strength",
                                      "_Strength",
                                      "Blend between the original color and nearest palette color",
                                      0.0, 1.0, 1.0,
                                      G_PARAM_READWRITE);

  gimp_procedure_add_boolean_argument (procedure,
                                       "non-destructive",
                                       "_Non-destructive layer filter",
                                       "Append as a non-destructive layer/group filter when possible; otherwise merge destructively",
                                       TRUE,
                                       G_PARAM_READWRITE);

  /* Color distance metric. The ids MUST match the GEGL op's
   * GeglPaletteQuantizeMetric enum values (0=srgb, 1=linear, 2=lab, 3=oklab). */
  {
    GimpChoice *metric = gimp_choice_new ();

    gimp_choice_add (metric, "srgb",    0, "sRGB Euclidean",
                     "Nearest color in sRGB (fast, default)");
    gimp_choice_add (metric, "linear",  1, "Linear RGB",
                     "Nearest color in linear-light RGB");
    gimp_choice_add (metric, "cie-lab", 2, "CIE Lab (perceptual)",
                     "Nearest color in CIE Lab");
    gimp_choice_add (metric, "oklab",   3, "OKLab (perceptual)",
                     "Nearest color in OKLab");

    gimp_procedure_add_choice_argument (procedure,
                                        "metric",
                                        "_Metric",
                                        "Color space used to pick the nearest palette color",
                                        metric,
                                        "srgb",
                                        G_PARAM_READWRITE);
  }

  /* Dithering method. Nicks MUST match the GEGL op's
   * GeglPaletteQuantizeDither enum nicks. */
  {
    GimpChoice *dither = gimp_choice_new ();

    gimp_choice_add (dither, "none",            0, "None",
                     "Hard nearest-color quantization");
    gimp_choice_add (dither, "ordered",         1, "Ordered (Bayer 8x8)",
                     "Position-based ordered dithering");
    gimp_choice_add (dither, "blue-noise",      2, "Blue noise",
                     "Ordered dithering with a natural, non-repeating noise");
    gimp_choice_add (dither, "floyd-steinberg", 3, "Floyd-Steinberg",
                     "Error diffusion (whole layer)");
    gimp_choice_add (dither, "atkinson",        4, "Atkinson",
                     "Error diffusion that keeps flat areas clean");
    gimp_choice_add (dither, "jarvis",          5, "Jarvis-Judice-Ninke",
                     "Large-kernel error diffusion (smooth)");
    gimp_choice_add (dither, "stucki",          6, "Stucki",
                     "Large-kernel error diffusion (smooth)");
    gimp_choice_add (dither, "sierra",          7, "Sierra",
                     "Error diffusion (balanced)");

    gimp_procedure_add_choice_argument (procedure,
                                        "dither",
                                        "_Dithering",
                                        "Dithering method used to distribute quantization error",
                                        dither,
                                        "none",
                                        G_PARAM_READWRITE);
  }

  gimp_procedure_add_boolean_argument (procedure,
                                       "serpentine",
                                       "Serpe_ntine scan",
                                       "Alternate row direction for error-diffusion dithers (reduces worm artifacts)",
                                       FALSE,
                                       G_PARAM_READWRITE);

  /* Alpha handling. Nicks MUST match the GEGL op's GeglPaletteQuantizeAlpha
   * enum nicks. Default "opaque" (ignore alpha): quantize each pixel's own
   * color and output it fully opaque. */
  {
    GimpChoice *alpha = gimp_choice_new ();

    gimp_choice_add (alpha, "preserve",  0, "Preserve alpha",
                     "Quantize color, keep the original alpha");
    gimp_choice_add (alpha, "opaque",    1, "Opaque (ignore alpha)",
                     "Quantize the pixel's own color, output fully opaque");
    gimp_choice_add (alpha, "composite", 2, "Composite over background",
                     "Blend over the background color, then quantize, output opaque");
    gimp_choice_add (alpha, "dir-paint", 3, "Directional paint (by shape)",
                     "Color each stroke by its surface normal: top color on top, etc.");
    gimp_choice_add (alpha, "dir-tint",  4, "Directional tint (by shape)",
                     "Same as directional paint but blended over the original color");
    gimp_choice_add (alpha, "bevel",     5, "Bevel / emboss",
                     "3D bevel: highlight the lit side, shadow the opposite, over Width");

    gimp_procedure_add_choice_argument (procedure,
                                        "alpha",
                                        "_Alpha",
                                        "How to treat alpha so the visible color is an exact palette color",
                                        alpha,
                                        "opaque",
                                        G_PARAM_READWRITE);
  }

  /* Background color for the "composite over background" alpha mode. */
  {
    GeglColor *white;

    gegl_init (NULL, NULL);   /* needed before gegl_color_new(); idempotent */
    white = gegl_color_new ("white");

    gimp_procedure_add_color_argument (procedure,
                                       "background",
                                       "_Background",
                                       "Backdrop color used by the 'composite over background' alpha mode",
                                       FALSE,
                                       white,
                                       G_PARAM_READWRITE);
  }

  /* Four directional colors + direction/relief, for the directional alpha
   * modes. GeglColor passes straight through (same type on both sides). */
  {
    GeglColor *c_top    = gegl_color_new ("white");
    GeglColor *c_right  = gegl_color_new ("gray");
    GeglColor *c_bottom = gegl_color_new ("black");
    GeglColor *c_left   = gegl_color_new ("gray");

    gimp_procedure_add_color_argument (procedure, "color-top", "Top color",
                                       "Directional color toward the top / light",
                                       FALSE, c_top, G_PARAM_READWRITE);
    gimp_procedure_add_color_argument (procedure, "color-right", "Right color",
                                       "Directional color toward the right",
                                       FALSE, c_right, G_PARAM_READWRITE);
    gimp_procedure_add_color_argument (procedure, "color-bottom", "Bottom color",
                                       "Directional color toward the bottom / shadow",
                                       FALSE, c_bottom, G_PARAM_READWRITE);
    gimp_procedure_add_color_argument (procedure, "color-left", "Left color",
                                       "Directional color toward the left",
                                       FALSE, c_left, G_PARAM_READWRITE);
  }

  gimp_procedure_add_double_argument (procedure,
                                      "direction",
                                      "_Direction",
                                      "Lighting direction in degrees (directional alpha modes)",
                                      0.0, 360.0, 0.0,
                                      G_PARAM_READWRITE);

  gimp_procedure_add_double_argument (procedure,
                                      "width",
                                      "_Width",
                                      "Spread/bevel width in pixels (directional 'by shape' and bevel modes)",
                                      0.0, 64.0, 4.0,
                                      G_PARAM_READWRITE);

  gimp_procedure_add_double_argument (procedure,
                                      "relief",
                                      "_Relief",
                                      "Strength of the directional tint / bevel shading",
                                      0.0, 1.0, 0.5,
                                      G_PARAM_READWRITE);

  gimp_procedure_add_double_argument (procedure,
                                      "threshold",
                                      "Alpha _threshold",
                                      "Hard-edge coverage cutoff: below becomes transparent, at/above opaque (all modes except Preserve)",
                                      0.0, 1.0, 0.5,
                                      G_PARAM_READWRITE);

  return procedure;
}

static gchar *
palette_to_hex_string (GimpPalette *palette, GError **error)
{
  const Babl *format;
  guint8     *map = NULL;
  gint        n_colors = 0;
  gsize       n_bytes = 0;
  GString    *out;

  if (! GIMP_IS_PALETTE (palette))
    {
      g_set_error (error, GIMP_PLUG_IN_ERROR, 0, "No valid palette was selected.");
      return NULL;
    }

  format = babl_format ("R'G'B' u8");
  map = gimp_palette_get_colormap (palette, format, &n_colors, &n_bytes);

  if (! map || n_colors <= 0 || n_bytes < 3)
    {
      g_free (map);
      g_set_error (error, GIMP_PLUG_IN_ERROR, 0, "The selected palette contains no RGB colors.");
      return NULL;
    }

  out = g_string_sized_new ((gsize) n_colors * 8);
  for (gint i = 0; i < n_colors; i++)
    {
      const guint8 *p = map + (i * 3);
      /* Honor the dialog's per-color exclusions when they match this palette. */
      if (g_included && g_included_n == n_colors && ! g_included[i])
        continue;
      if (out->len > 0)
        g_string_append_c (out, ';');
      g_string_append_printf (out, "#%02x%02x%02x", p[0], p[1], p[2]);
    }

  if (out->len == 0)   /* everything excluded -> fall back to the full palette */
    for (gint i = 0; i < n_colors; i++)
      {
        const guint8 *p = map + (i * 3);
        if (i > 0) g_string_append_c (out, ';');
        g_string_append_printf (out, "#%02x%02x%02x", p[0], p[1], p[2]);
      }

  g_free (map);
  return g_string_free (out, FALSE);
}

/* Show or hide one argument's widget. set_no_show_all keeps the dialog's own
 * gtk_widget_show_all() from overriding the choice. */
static void
arg_set_visible (GimpProcedureDialog *dialog,
                 const gchar         *prop,
                 gboolean             visible)
{
  GtkWidget *w = gimp_procedure_dialog_get_widget (dialog, prop, G_TYPE_NONE);

  if (w)
    {
      gtk_widget_set_no_show_all (w, TRUE);
      gtk_widget_set_visible (w, visible);
    }
}

/* Only show the controls that the current alpha / dither modes actually use, so
 * the directional colors etc. disappear when they would do nothing. Connected
 * to notify::alpha and notify::dither on the config (and called once up front). */
static void
update_arg_visibility (GimpProcedureConfig *config,
                       GParamSpec          *pspec,
                       gpointer             user_data)
{
  GimpProcedureDialog *dialog = GIMP_PROCEDURE_DIALOG (user_data);
  gchar               *alpha  = NULL;
  gchar               *dither = NULL;
  gboolean             paint_tint, bevel, dir, composite, uses_relief;
  gboolean             not_preserve, diffuse;

  (void) pspec;

  g_object_get (config, "alpha", &alpha, "dither", &dither, NULL);

  paint_tint   = (g_strcmp0 (alpha, "dir-paint") == 0 ||
                  g_strcmp0 (alpha, "dir-tint")  == 0);
  bevel        = (g_strcmp0 (alpha, "bevel") == 0);
  dir          = paint_tint || bevel;
  composite    = (g_strcmp0 (alpha, "composite") == 0);
  uses_relief  = (g_strcmp0 (alpha, "dir-tint") == 0) || bevel;
  not_preserve = (g_strcmp0 (alpha, "preserve") != 0);
  diffuse      = ! (g_strcmp0 (dither, "none")       == 0 ||
                    g_strcmp0 (dither, "ordered")    == 0 ||
                    g_strcmp0 (dither, "blue-noise") == 0);

  arg_set_visible (dialog, "background",   composite);
  /* bevel uses only Top (highlight) and Bottom (shadow); paint/tint use all 4 */
  arg_set_visible (dialog, "color-top",    dir);
  arg_set_visible (dialog, "color-bottom", dir);
  arg_set_visible (dialog, "color-right",  paint_tint);
  arg_set_visible (dialog, "color-left",   paint_tint);
  arg_set_visible (dialog, "direction",    dir);
  arg_set_visible (dialog, "width",        dir);
  arg_set_visible (dialog, "relief",       uses_relief);
  arg_set_visible (dialog, "threshold",    not_preserve);
  arg_set_visible (dialog, "serpentine",   diffuse);

  g_free (alpha);
  g_free (dither);
}

/* A compact palette swatch grid drawn on a single canvas (far denser than a
 * grid of toggle buttons). It mirrors the palette's own column count (the same
 * layout the Palette Editor uses) so it reads as a reference, sizes cells to fit
 * the dialog width, and is shown at its natural height (the viewport only
 * scrolls for very large palettes). Click a swatch to exclude/include it from
 * the quantize palette; excluded swatches are dimmed with an X. Flags live in
 * g_included[]. */
#define PQ_CELL_MAX 26.0   /* don't let swatches grow bigger than this */

typedef struct
{
  GtkWidget           *area;
  GimpProcedureConfig *config;
  GimpPalette         *palette;  /* kept ref, for per-entry name lookups */
  gfloat              *rgb;       /* n*3, owned (freed on rebuild) */
  gint                 n;
  gint                 pal_cols;  /* palette's column hint (0 = auto by width) */
  gint                 last_h;
} PaletteGrid;

static inline gboolean
grid_included (gint i)
{
  return ! (g_included && i < g_included_n) || g_included[i];
}

/* Columns: the palette's own column count when it sets one (matches the Palette
 * Editor), otherwise a width-based fallback. */
static gint
grid_cols (PaletteGrid *g)
{
  if (g->pal_cols > 0)
    return g->pal_cols;
  return MAX (1, gtk_widget_get_allocated_width (g->area) / 20);
}

/* Cell size: fill the width with grid_cols() columns, capped so a small
 * palette's swatches don't become huge. */
static gdouble
grid_cell (PaletteGrid *g)
{
  gdouble cell = (gdouble) gtk_widget_get_allocated_width (g->area) / grid_cols (g);
  return CLAMP (cell, 1.0, PQ_CELL_MAX);
}

static gint
grid_index_at (PaletteGrid *g, gdouble px, gdouble py)
{
  gint    cols = grid_cols (g);
  gdouble cell = grid_cell (g);
  gint    c = (gint) (px / cell);
  gint    r = (gint) (py / cell);
  gint    i;
  if (c < 0 || c >= cols) return -1;
  i = r * cols + c;
  return (i >= 0 && i < g->n) ? i : -1;
}

static gboolean
grid_draw (GtkWidget *w, cairo_t *cr, gpointer data)
{
  PaletteGrid *g    = data;
  gint         cols = grid_cols (g);
  gdouble      cell = grid_cell (g);
  gdouble      s    = MAX (1.0, cell - 2.0);
  (void) w;

  for (gint i = 0; i < g->n; i++)
    {
      gint    c = i % cols, r = i / cols;
      gdouble x = c * cell, y = r * cell;

      cairo_set_source_rgb (cr, g->rgb[i*3], g->rgb[i*3+1], g->rgb[i*3+2]);
      cairo_rectangle (cr, x + 1, y + 1, s, s);
      cairo_fill (cr);

      if (! grid_included (i))
        {
          cairo_set_source_rgba (cr, 0, 0, 0, 0.6);
          cairo_rectangle (cr, x + 1, y + 1, s, s); cairo_fill (cr);
          cairo_set_source_rgb (cr, 1, 1, 1); cairo_set_line_width (cr, 1.0);
          cairo_move_to (cr, x + 3, y + 3);
          cairo_line_to (cr, x + cell - 3, y + cell - 3);
          cairo_move_to (cr, x + cell - 3, y + 3);
          cairo_line_to (cr, x + 3, y + cell - 3);
          cairo_stroke (cr);
        }
      else
        {
          cairo_set_source_rgba (cr, 0, 0, 0, 0.25); cairo_set_line_width (cr, 1.0);
          cairo_rectangle (cr, x + 1.5, y + 1.5, s - 1, s - 1); cairo_stroke (cr);
        }
    }
  return FALSE;
}

static gboolean
grid_press (GtkWidget *w, GdkEventButton *e, gpointer data)
{
  PaletteGrid *g = data;
  gint i = grid_index_at (g, e->x, e->y);
  if (i >= 0 && g_included && i < g_included_n)
    {
      g_included[i] = ! g_included[i];
      gtk_widget_queue_draw (w);
    }
  return TRUE;
}

static gboolean
grid_tooltip (GtkWidget *w, gint x, gint y, gboolean kb, GtkTooltip *tip, gpointer data)
{
  PaletteGrid *g = data;
  gint   i = grid_index_at (g, x, y);
  gchar *name = NULL;
  gchar *txt;
  gchar  hex[8];
  (void) w; (void) kb;
  if (i < 0) return FALSE;

  g_snprintf (hex, sizeof hex, "#%02x%02x%02x",
              (guint) (CLAMP (g->rgb[i*3],   0.0f, 1.0f) * 255.0f + 0.5f),
              (guint) (CLAMP (g->rgb[i*3+1], 0.0f, 1.0f) * 255.0f + 0.5f),
              (guint) (CLAMP (g->rgb[i*3+2], 0.0f, 1.0f) * 255.0f + 0.5f));

  if (g->palette &&
      gimp_palette_get_entry_name (g->palette, i, &name) && name && *name)
    txt = g_strdup_printf ("%s  %s%s", name, hex,
                           grid_included (i) ? "" : "  (excluded)");
  else
    txt = g_strdup_printf ("%s%s", hex,
                           grid_included (i) ? "" : "  (excluded)");

  gtk_tooltip_set_text (tip, txt);
  g_free (txt);
  g_free (name);
  return TRUE;
}

static void
grid_update_height (PaletteGrid *g)
{
  gint cols = grid_cols (g);
  gint rows = (g->n + cols - 1) / cols;
  gint h    = MAX (1, (gint) (rows * grid_cell (g) + 0.999));
  if (h != g->last_h)   /* guard against the set_size_request->reallocate loop */
    {
      g->last_h = h;
      gtk_widget_set_size_request (g->area, -1, h);
    }
}

static void
grid_size_allocate (GtkWidget *w, GdkRectangle *alloc, gpointer data)
{
  (void) w; (void) alloc;
  grid_update_height ((PaletteGrid *) data);
}

static void
grid_rebuild (PaletteGrid *g)
{
  gfloat *fmap;
  gint    n = 0;
  gsize   nbytes = 0;

  g_clear_pointer (&g_included, g_free);
  g_included_n = 0;
  g_clear_pointer (&g->rgb, g_free);
  g_clear_object (&g->palette);
  g->n = 0;
  g->pal_cols = 0;

  /* While re-editing an existing filter and before the user picks a (new)
   * palette, show that filter's current colors parsed from its hex string. */
  if (g_edit_hex && ! g_palette_chosen)
    {
      gfloat *rgb = parse_hex_to_rgb (g_edit_hex, &n);
      if (rgb && n > 0)
        {
          g->rgb       = rgb;
          g->n         = n;
          g->pal_cols  = 0;            /* unknown layout -> auto by width */
          g_included   = g_new (gboolean, n);
          g_included_n = n;
          for (gint i = 0; i < n; i++) g_included[i] = TRUE;
        }
      else
        {
          g_free (rgb);
        }
    }

  if (g->n == 0)
    {
      g_object_get (g->config, "palette", &g->palette, NULL);
      if (GIMP_IS_PALETTE (g->palette))
        {
          fmap = (gfloat *) gimp_palette_get_colormap (g->palette,
                                                       babl_format ("R'G'B' float"),
                                                       &n, &nbytes);
          if (fmap && n > 0)
            {
              g->rgb       = fmap;
              g->n         = n;
              g->pal_cols  = gimp_palette_get_columns (g->palette);
              g_included   = g_new (gboolean, n);
              g_included_n = n;
              for (gint i = 0; i < n; i++) g_included[i] = TRUE;
            }
          else
            {
              g_free (fmap);
            }
        }
    }

  g->last_h = -1;
  grid_update_height (g);
  gtk_widget_queue_draw (g->area);
}

static void
on_palette_changed (GimpProcedureConfig *config, GParamSpec *pspec, gpointer data)
{
  (void) config; (void) pspec;
  g_palette_chosen = TRUE;   /* user actively picked a palette -> use it */
  grid_rebuild ((PaletteGrid *) data);
}

static gboolean
show_dialog (GimpProcedure       *procedure,
             GimpProcedureConfig *config)
{
  GtkWidget   *dialog;
  GtkWidget   *frame, *scroll, *content;
  PaletteGrid  grid;
  gboolean     run;

  gimp_ui_init (PLUG_IN_BINARY);

  dialog = gimp_procedure_dialog_new (procedure, config,
                                      "Quantize to Palette");
  gimp_procedure_dialog_fill (GIMP_PROCEDURE_DIALOG (dialog),
                              "palette",
                              "metric",
                              "dither",
                              "serpentine",
                              "alpha",
                              "background",
                              "color-top",
                              "color-right",
                              "color-bottom",
                              "color-left",
                              "direction",
                              "width",
                              "relief",
                              "threshold",
                              "strength",
                              "non-destructive",
                              NULL);

  /* Compact palette swatch grid (click a color to exclude/include it). */
  grid.config  = config;
  grid.palette = NULL;
  grid.rgb     = NULL;
  grid.n       = 0;
  grid.pal_cols= 0;
  grid.last_h  = -1;
  grid.area    = gtk_drawing_area_new ();
  gtk_widget_add_events (grid.area, GDK_BUTTON_PRESS_MASK);
  gtk_widget_set_has_tooltip (grid.area, TRUE);
  g_signal_connect (grid.area, "draw",               G_CALLBACK (grid_draw),          &grid);
  g_signal_connect (grid.area, "button-press-event", G_CALLBACK (grid_press),         &grid);
  g_signal_connect (grid.area, "query-tooltip",      G_CALLBACK (grid_tooltip),       &grid);
  g_signal_connect (grid.area, "size-allocate",      G_CALLBACK (grid_size_allocate), &grid);

  /* Size to the grid's natural height (no scrollbar) up to a cap, beyond which
   * (very large palettes) it scrolls so the dialog can't grow off-screen. */
  scroll = gtk_scrolled_window_new (NULL, NULL);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scroll), TRUE);
  gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (scroll), 380);
  gtk_container_add (GTK_CONTAINER (scroll), grid.area);

  frame = gtk_frame_new ("Palette colors (click to exclude/include)");
  gtk_container_add (GTK_CONTAINER (frame), scroll);

  content = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
  gtk_box_pack_start (GTK_BOX (content), frame, FALSE, FALSE, 6);
  gtk_widget_show_all (frame);

  grid_rebuild (&grid);

  /* Live-update the swatch grid and which mode-specific controls are shown. */
  g_signal_connect (config, "notify::palette",
                    G_CALLBACK (on_palette_changed), &grid);
  g_signal_connect (config, "notify::alpha",
                    G_CALLBACK (update_arg_visibility), dialog);
  g_signal_connect (config, "notify::dither",
                    G_CALLBACK (update_arg_visibility), dialog);
  update_arg_visibility (config, NULL, GIMP_PROCEDURE_DIALOG (dialog));

  run = gimp_procedure_dialog_run (GIMP_PROCEDURE_DIALOG (dialog));

  g_signal_handlers_disconnect_by_func (config,
                                        G_CALLBACK (on_palette_changed), &grid);
  g_signal_handlers_disconnect_by_func (config,
                                        G_CALLBACK (update_arg_visibility),
                                        dialog);
  gtk_widget_destroy (dialog);

  /* Hand the grid's final colors to run() so the output palette is exactly what
   * was shown (handles the "kept the existing filter's palette" case). */
  g_clear_pointer (&g_final_rgb, g_free);
  g_final_n = 0;
  if (run && grid.rgb && grid.n > 0)
    {
      g_final_rgb = g_memdup2 (grid.rgb, (gsize) grid.n * 3 * sizeof (gfloat));
      g_final_n   = grid.n;
    }

  g_free (grid.rgb);
  g_clear_object (&grid.palette);

  return run;
}

static GimpValueArray *
palette_quantize_run (GimpProcedure        *procedure,
                      GimpRunMode           run_mode,
                      GimpImage            *image,
                      GimpDrawable        **drawables,
                      GimpProcedureConfig  *config,
                      gpointer              run_data)
{
  GimpPDBStatusType status = GIMP_PDB_SUCCESS;
  GError           *error = NULL;
  GimpPalette      *palette = NULL;
  gchar            *hex_palette = NULL;
  gdouble           strength = 1.0;
  gboolean          non_destructive = TRUE;
  gboolean          serpentine = FALSE;
  gchar            *metric = NULL;
  gchar            *dither = NULL;
  gchar            *alpha = NULL;
  GeglColor        *background = NULL;
  GeglColor        *color_top = NULL;
  GeglColor        *color_right = NULL;
  GeglColor        *color_bottom = NULL;
  GeglColor        *color_left = NULL;
  gdouble           direction = 0.0;
  gdouble           width = 4.0;
  gdouble           relief = 0.5;
  gdouble           threshold = 0.5;
  gint              n_drawables;
  GimpDrawableFilter *existing = NULL;

  (void) run_data;

  gegl_init (NULL, NULL);

  /* Reset the dialog/edit state for this run. */
  g_clear_pointer (&g_edit_hex, g_free);
  g_clear_pointer (&g_final_rgb, g_free);
  g_final_n = 0;
  g_palette_chosen = FALSE;

  n_drawables = gimp_core_object_array_get_length ((GObject **) drawables);
  if (n_drawables != 1)
    {
      g_set_error (&error, GIMP_PLUG_IN_ERROR, 0,
                   "Select exactly one layer or layer group before running this filter.");
      return gimp_procedure_new_return_values (procedure, GIMP_PDB_CALLING_ERROR, error);
    }

  /* If this drawable already has our quantize filter, we re-edit it in place
   * (and pre-fill the dialog from its current settings) instead of stacking a
   * second one. */
  existing = find_existing_filter (drawables[0]);

  if (existing && run_mode == GIMP_RUN_INTERACTIVE)
    {
      GObject   *fc   = G_OBJECT (gimp_drawable_filter_get_config (existing));
      gchar     *eh = NULL, *em = NULL, *ed = NULL, *ea = NULL;
      gboolean   es = FALSE;
      GeglColor *ebg = NULL, *ect = NULL, *ecr = NULL, *ecb = NULL, *ecl = NULL;
      gdouble    edir = 0.0, ew = 4.0, er = 0.5, eth = 0.5, estr = 1.0;

      g_object_get (fc, "palette", &eh, "metric", &em, "dither", &ed,
                    "serpentine", &es, "alpha", &ea, "background", &ebg,
                    "color-top", &ect, "color-right", &ecr,
                    "color-bottom", &ecb, "color-left", &ecl,
                    "direction", &edir, "width", &ew, "relief", &er,
                    "threshold", &eth, "strength", &estr, NULL);

      /* Mirror the existing op settings into our dialog config (the palette is a
       * GimpPalette resource we can't set from a hex string, so it is shown via
       * the swatch grid using g_edit_hex instead). */
      if (em) g_object_set (config, "metric", em, NULL);
      if (ed) g_object_set (config, "dither", ed, NULL);
      g_object_set (config, "serpentine", es, NULL);
      if (ea) g_object_set (config, "alpha", ea, NULL);
      if (ebg) g_object_set (config, "background", ebg, NULL);
      if (ect) g_object_set (config, "color-top", ect, NULL);
      if (ecr) g_object_set (config, "color-right", ecr, NULL);
      if (ecb) g_object_set (config, "color-bottom", ecb, NULL);
      if (ecl) g_object_set (config, "color-left", ecl, NULL);
      g_object_set (config, "direction", edir, "width", ew, "relief", er,
                    "threshold", eth, "strength", estr, NULL);

      g_edit_hex = g_strdup (eh);

      g_free (eh); g_free (em); g_free (ed); g_free (ea);
      g_clear_object (&ebg); g_clear_object (&ect); g_clear_object (&ecr);
      g_clear_object (&ecb); g_clear_object (&ecl);
    }

  if (run_mode == GIMP_RUN_INTERACTIVE)
    {
      if (! show_dialog (procedure, config))
        return gimp_procedure_new_return_values (procedure, GIMP_PDB_CANCEL, NULL);
    }

  g_object_get (config,
                "palette", &palette,
                "strength", &strength,
                "non-destructive", &non_destructive,
                "serpentine", &serpentine,
                "metric", &metric,
                "dither", &dither,
                "alpha", &alpha,
                "background", &background,
                "color-top", &color_top,
                "color-right", &color_right,
                "color-bottom", &color_bottom,
                "color-left", &color_left,
                "direction", &direction,
                "width", &width,
                "relief", &relief,
                "threshold", &threshold,
                NULL);

  /* GIMP mirrors the GEGL op's enum properties into the drawable-filter config
   * as nick strings, so they must be passed to *_new_filter() as const gchar*
   * (passing an integer id would be read as a bogus pointer and crash). Our
   * GimpChoice nicks intentionally match the GEGL enum nicks. The background
   * is a GeglColor on both sides and is passed straight through. */
  if (! metric)
    metric = g_strdup ("srgb");
  if (! dither)
    dither = g_strdup ("none");
  if (! alpha)
    alpha = g_strdup ("opaque");
  if (! background)
    background = gegl_color_new ("white");
  if (! color_top)
    color_top = gegl_color_new ("white");
  if (! color_right)
    color_right = gegl_color_new ("gray");
  if (! color_bottom)
    color_bottom = gegl_color_new ("black");
  if (! color_left)
    color_left = gegl_color_new ("gray");

  strength = CLAMP (strength, 0.0, 1.0);
  /* Prefer the exact colors shown in the dialog's swatch grid (covers excluding
   * colors and re-editing a kept palette); fall back to the chosen GimpPalette
   * for non-interactive runs. */
  hex_palette = hex_from_final_colors ();
  if (! hex_palette)
    hex_palette = palette_to_hex_string (palette, &error);
  g_clear_object (&palette);

  if (! hex_palette)
    {
      g_free (metric);
      g_free (dither);
      g_free (alpha);
      g_clear_object (&background);
      g_clear_object (&color_top);
      g_clear_object (&color_right);
      g_clear_object (&color_bottom);
      g_clear_object (&color_left);
      return gimp_procedure_new_return_values (procedure, GIMP_PDB_CALLING_ERROR, error);
    }

  gimp_image_undo_group_start (image);

  if (existing && non_destructive)
    {
      /* Re-edit the existing filter in place instead of stacking a new one. */
      GObject *fc = G_OBJECT (gimp_drawable_filter_get_config (existing));

      g_object_set (fc,
                    "palette", hex_palette,
                    "metric", metric,
                    "dither", dither,
                    "serpentine", serpentine,
                    "alpha", alpha,
                    "background", background,
                    "color-top", color_top,
                    "color-right", color_right,
                    "color-bottom", color_bottom,
                    "color-left", color_left,
                    "direction", direction,
                    "width", width,
                    "relief", relief,
                    "threshold", threshold,
                    "strength", strength,
                    NULL);
      gimp_drawable_filter_update (existing);
    }
  else if (non_destructive)
    {
      GimpDrawableFilter *filter;

      filter = gimp_drawable_append_new_filter (drawables[0],
                                                GEGL_OP_NAME,
                                                "Palette Quantize",
                                                GIMP_LAYER_MODE_REPLACE,
                                                1.0,
                                                "palette", hex_palette,
                                                "metric", metric,
                                                "dither", dither,
                                                "serpentine", serpentine,
                                                "alpha", alpha,
                                                "background", background,
                                                "color-top", color_top,
                                                "color-right", color_right,
                                                "color-bottom", color_bottom,
                                                "color-left", color_left,
                                                "direction", direction,
                                                "width", width,
                                                "relief", relief,
                                                "threshold", threshold,
                                                "strength", strength,
                                                NULL);
      if (! filter)
        {
          gimp_drawable_merge_new_filter (drawables[0],
                                          GEGL_OP_NAME,
                                          "Palette Quantize",
                                          GIMP_LAYER_MODE_REPLACE,
                                          1.0,
                                          "palette", hex_palette,
                                          "metric", metric,
                                          "dither", dither,
                                          "serpentine", serpentine,
                                          "alpha", alpha,
                                          "background", background,
                                          "color-top", color_top,
                                          "color-right", color_right,
                                          "color-bottom", color_bottom,
                                          "color-left", color_left,
                                          "direction", direction,
                                          "width", width,
                                          "relief", relief,
                                          "threshold", threshold,
                                          "strength", strength,
                                          NULL);
        }
    }
  else
    {
      gimp_drawable_merge_new_filter (drawables[0],
                                      GEGL_OP_NAME,
                                      "Palette Quantize",
                                      GIMP_LAYER_MODE_REPLACE,
                                      1.0,
                                      "palette", hex_palette,
                                      "metric", metric,
                                      "dither", dither,
                                      "serpentine", serpentine,
                                      "alpha", alpha,
                                      "background", background,
                                      "color-top", color_top,
                                      "color-right", color_right,
                                      "color-bottom", color_bottom,
                                      "color-left", color_left,
                                      "direction", direction,
                                      "width", width,
                                      "relief", relief,
                                      "threshold", threshold,
                                      "strength", strength,
                                      NULL);
    }

  gimp_image_undo_group_end (image);
  gimp_displays_flush ();

  g_free (hex_palette);
  g_free (metric);
  g_free (dither);
  g_free (alpha);
  g_clear_object (&background);
  g_clear_object (&color_top);
  g_clear_object (&color_right);
  g_clear_object (&color_bottom);
  g_clear_object (&color_left);

  return gimp_procedure_new_return_values (procedure, status, NULL);
}
