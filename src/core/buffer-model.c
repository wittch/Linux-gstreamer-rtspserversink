#include "buffer-model.h"

gboolean
gst_rtsp_sink_describe_buffer(const GstBuffer *buffer, GstRTSPSinkFrameInfo *info)
{
  if (buffer == NULL || info == NULL)
    return FALSE;

  info->is_delta_unit = GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
  info->is_access_unit = !info->is_delta_unit;
  info->has_pts = GST_BUFFER_PTS_IS_VALID(buffer);
  info->has_dts = GST_BUFFER_DTS_IS_VALID(buffer);
  info->has_duration = GST_BUFFER_DURATION_IS_VALID(buffer);
  info->pts = info->has_pts ? GST_BUFFER_PTS(buffer) : GST_CLOCK_TIME_NONE;
  info->dts = info->has_dts ? GST_BUFFER_DTS(buffer) : GST_CLOCK_TIME_NONE;
  info->duration = info->has_duration ? GST_BUFFER_DURATION(buffer) : GST_CLOCK_TIME_NONE;

  return TRUE;
}
