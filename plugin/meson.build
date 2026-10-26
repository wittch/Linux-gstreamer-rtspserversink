pluginsdir = get_option('pluginsdir')
if pluginsdir == ''
  pluginsdir = gst_dep.get_variable(pkgconfig: 'pluginsdir')
endif

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
  install_dir: pluginsdir,
)
