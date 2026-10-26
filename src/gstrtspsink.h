#ifndef __GST_RTSP_SINK_H__
#define __GST_RTSP_SINK_H__

#include <gst/gst.h>

G_BEGIN_DECLS

#define GST_TYPE_RTSP_SINK (gst_rtsp_sink_get_type())
G_DECLARE_FINAL_TYPE (GstRTSPSink, gst_rtsp_sink, GST, RTSP_SINK, GstBin)

G_END_DECLS

#endif
