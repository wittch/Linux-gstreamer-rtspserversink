#include <gst/gst.h>

#include "gstrtspsink.h"

#ifndef PACKAGE
#define PACKAGE "gst-rtsp-server-sink"
#endif

static gboolean
plugin_init(GstPlugin *plugin)
{
  return gst_element_register(plugin, "rtspserversink", GST_RANK_NONE, GST_TYPE_RTSP_SINK);
}

GST_PLUGIN_EXPORT const GstPluginDesc gst_plugin_desc = {
  GST_VERSION_MAJOR,
  GST_VERSION_MINOR,
  "gstrtspserversink",
  "Custom RTSP server sink plugin",
  plugin_init,
  "0.1.0",
  "LGPL",
  PACKAGE,
  "gst-rtsp-server-sink",
  "https://example.com",
  NULL,
  GST_PADDING_INIT,
};
