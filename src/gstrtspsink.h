#pragma once

#include <gst/base/gstbasesink.h>

G_BEGIN_DECLS

#define GST_TYPE_RTSP_SINK (gst_rtsp_sink_get_type())
G_DECLARE_FINAL_TYPE(GstRTSPSink, gst_rtsp_sink, GST, RTSP_SINK, GstBaseSink)

G_END_DECLS
