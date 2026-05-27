project(
  'gst-rtsp-server-sink',
  'c',
  version: '0.1.0',
  default_options: [
    'warning_level=2',
    'c_std=c11',
  ],
)

gst_dep = dependency('gstreamer-1.0', version: '>=1.22')
gst_base_dep = dependency('gstreamer-base-1.0', version: '>=1.22')
glib_dep = dependency('glib-2.0', version: '>=2.68')
gobject_dep = dependency('gobject-2.0', version: '>=2.68')
gio_dep = dependency('gio-2.0', version: '>=2.68')

subdir('src')
subdir('plugin')
