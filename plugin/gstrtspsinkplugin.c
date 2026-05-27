#include <gst/gst.h>

#include "gstrtspsink.h"

#ifndef PACKAGE
#define PACKAGE "gst-rtsp-server-sink"
#endif

#ifndef PACKAGE_NAME
#define PACKAGE_NAME "gst-rtsp-server-sink"
#endif

#ifndef GST_PACKAGE_ORIGIN
#define GST_PACKAGE_ORIGIN "https://example.invalid/gst-rtsp-server-sink"
#endif

static gboolean
plugin_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, "rtspserversink", GST_RANK_NONE,
      GST_TYPE_RTSP_SINK);
}

GST_PLUGIN_DEFINE (
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    rtspserversink,
    "RTSP sink element",
    plugin_init,
    "0.1.0",
    "LGPL",
    PACKAGE_NAME,
    GST_PACKAGE_ORIGIN)
