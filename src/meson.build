shared_module(
  'gegl-palette-quantize',
  'gegl-palette-quantize.c',
  dependencies: [glib, gobject, gegl, babl],
  name_prefix: '',
  install: false
)

if libgimp.found() and libgimpui.found()
  executable(
    'gimp-palette-quantize',
    'gimp-palette-quantize.c',
    dependencies: [glib, gobject, gegl, babl, libgimp, libgimpui],
    install: false
  )
else
  warning('gimp-3.0 and/or gimpui-3.0 development files were not found; building only the GEGL operation.')
endif
