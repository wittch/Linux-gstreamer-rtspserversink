src_inc = include_directories('.', 'core')

rtspsink_sources = files(
  'gstrtspsink.c',
  'core/rtsp-client.c',
  'core/rtsp-config.c',
  'core/h264-pay.c',
  'core/rtsp-parser.c',
  'core/rtsp-router.c',
  'core/rtsp-server.c',
  'core/sdp-builder.c',
)

rtspsink_internal_lib = static_library(
  'rtspsinkinternal',
  rtspsink_sources,
  include_directories: src_inc,
  dependencies: [
    glib_dep,
    gobject_dep,
    gio_dep,
    gst_dep,
    gst_base_dep,
  ],
  install: false,
)

rtspsink_internal_dep = declare_dependency(
  link_with: rtspsink_internal_lib,
  include_directories: src_inc,
  dependencies: [
    glib_dep,
    gobject_dep,
    gio_dep,
    gst_dep,
    gst_base_dep,
  ],
)
