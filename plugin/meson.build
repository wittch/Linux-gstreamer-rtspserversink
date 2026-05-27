shared_module(
  'gstrtspserversink',
  'gstrtspsinkplugin.c',
  include_directories: [
    include_directories('..'),
    src_inc,
  ],
  dependencies: [
    glib_dep,
    gobject_dep,
    gst_dep,
    gst_base_dep,
  ],
  link_with: rtspsink_internal_lib,
  name_prefix: '',
  install: true,
  install_dir: join_paths(get_option('libdir'), 'gstreamer-1.0'),
)
