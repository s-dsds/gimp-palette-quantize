project('gimp-palette-quantize', 'c',
  version: '0.4.0',
  default_options: ['warning_level=2', 'c_std=c11']
)

cc = meson.get_compiler('c')

glib = dependency('glib-2.0')
gobject = dependency('gobject-2.0')
gegl = dependency('gegl-0.4')
# Upstream pkg-config name is 'babl-0.1'; some older toolchains ship 'babl'.
babl = dependency('babl-0.1', required: false)
if not babl.found()
  babl = dependency('babl')
endif
libgimp = dependency('gimp-3.0', required: false)
libgimpui = dependency('gimpui-3.0', required: false)

subdir('src')
