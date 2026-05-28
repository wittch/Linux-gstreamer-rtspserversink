#pragma once

#include <gst/gst.h>

gboolean gst_rtsp_sink_h264_analyze_buffer(const GstBuffer *buffer,
                                           const gchar *stream_format,
                                           gboolean *is_keyframe,
                                           gboolean *has_parameter_sets);
gboolean gst_rtsp_sink_h264_packetize(GstBuffer *buffer);
