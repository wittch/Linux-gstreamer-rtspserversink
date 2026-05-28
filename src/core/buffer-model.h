#pragma once

#include <gst/gst.h>

typedef struct _GstRTSPSinkFrameInfo
{
  gboolean is_access_unit;
  gboolean is_delta_unit;
  gboolean has_pts;
  gboolean has_dts;
  gboolean has_duration;
  GstClockTime pts;
  GstClockTime dts;
  GstClockTime duration;
} GstRTSPSinkFrameInfo;

gboolean gst_rtsp_sink_describe_buffer(const GstBuffer *buffer, GstRTSPSinkFrameInfo *info);
