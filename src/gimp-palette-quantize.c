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
   * enum nicks. Default "composite" so the visible color is an exact palette
   * color (semi-transparent pixels are blended over the background first). */
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
    gimp_choice_add (alpha, "dir-bbox",  5, "Directional gradient (per stroke)",
                     "Color each separate stroke by position within its own bounds");
    gimp_choice_add (alpha, "bevel",     6, "Bevel / emboss",
                     "3D bevel: highlight the lit side, shadow the opposite, over Width");

    gimp_procedure_add_choice_argument (procedure,
                                        "alpha",
                                        "_Alpha",
                                        "How to treat alpha so the visible color is an exact palette color",
                                        alpha,
                                        "composite",
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
      g_string_append_printf (out, "#%02x%02x%02x", p[0], p[1], p[2]);
      if (i + 1 < n_colors)
        g_string_append_c (out, ';');
    }

  g_free (map);
  return g_string_free (out, FALSE);
}

static gboolean
show_dialog (GimpProcedure       *procedure,
             GimpProcedureConfig *config)
{
  GtkWidget *dialog;
  gboolean   run;

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
                              "strength",
                              "non-destructive",
                              NULL);
  run = gimp_procedure_dialog_run (GIMP_PROCEDURE_DIALOG (dialog));
  gtk_widget_destroy (dialog);

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
  gint              n_drawables;

  (void) run_data;

  gegl_init (NULL, NULL);

  n_drawables = gimp_core_object_array_get_length ((GObject **) drawables);
  if (n_drawables != 1)
    {
      g_set_error (&error, GIMP_PLUG_IN_ERROR, 0,
                   "Select exactly one layer or layer group before running this filter.");
      return gimp_procedure_new_return_values (procedure, GIMP_PDB_CALLING_ERROR, error);
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
    alpha = g_strdup ("composite");
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
  hex_palette = palette_to_hex_string (palette, &error);
  g_clear_object (&palette);

  if (! hex_palette)
    {
      g_free (metric);
      g_free (dither);
      g_free (alpha);
      g_clear_object (&background);
      return gimp_procedure_new_return_values (procedure, GIMP_PDB_CALLING_ERROR, error);
    }

  gimp_image_undo_group_start (image);

  if (non_destructive)
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
                                          "color-top", color_top,
                                          "color-right", color_right,
                                          "color-bottom", color_bottom,
                                          "color-left", color_left,
                                          "direction", direction,
                                      "width", width,
                                          "width", width,
                                          "relief", relief,
                                                "color-top", color_top,
                                                "color-right", color_right,
                                                "color-bottom", color_bottom,
                                                "color-left", color_left,
                                                "direction", direction,
                                      "width", width,
                                          "width", width,
                                                "width", width,
                                                "relief", relief,
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
                                          "color-top", color_top,
                                          "color-right", color_right,
                                          "color-bottom", color_bottom,
                                          "color-left", color_left,
                                          "direction", direction,
                                      "width", width,
                                          "width", width,
                                          "relief", relief,
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
