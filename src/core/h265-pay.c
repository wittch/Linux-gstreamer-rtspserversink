#include "h265-pay.h"

static gboolean
gst_rtsp_sink_h265_scan_nals(const guint8 *data,
                             gsize size,
                             gboolean *is_keyframe,
                             gboolean *has_parameter_sets)
{
  gsize pos = 0;
  gboolean found_nal = FALSE;

  while (pos + 3 < size) {
    gsize start = G_MAXSIZE;
    gsize next = G_MAXSIZE;

    for (gsize i = pos; i + 3 < size; i++) {
      if (data[i] == 0x00 && data[i + 1] == 0x00 && (data[i + 2] == 0x01 ||
          (i + 3 < size && data[i + 2] == 0x00 && data[i + 3] == 0x01))) {
        start = (data[i + 2] == 0x01) ? i + 3 : i + 4;
        break;
      }
    }

    if (start == G_MAXSIZE)
      break;

    for (gsize i = start; i + 3 < size; i++) {
      if (data[i] == 0x00 && data[i + 1] == 0x00 && (data[i + 2] == 0x01 ||
          (i + 3 < size && data[i + 2] == 0x00 && data[i + 3] == 0x01))) {
        next = i;
        break;
      }
    }

    if (next == G_MAXSIZE)
      next = size;

    if (start >= next || start >= size)
      break;

    guint8 nal_type = (data[start] >> 1) & 0x3f;
    found_nal = TRUE;
    if ((nal_type == 19 || nal_type == 20) && is_keyframe != NULL)
      *is_keyframe = TRUE;
    if (nal_type == 32 || nal_type == 33 || nal_type == 34) {
      if (has_parameter_sets != NULL)
        *has_parameter_sets = TRUE;
    }

    pos = next;
  }

  return found_nal;
}

static gboolean
gst_rtsp_sink_h265_scan_hvc1(const guint8 *data,
                             gsize size,
                             gboolean *is_keyframe,
                             gboolean *has_parameter_sets)
{
  gsize pos = 0;
  gboolean found_nal = FALSE;

  while (pos + 4 <= size) {
    guint32 nal_size =
        ((guint32) data[pos] << 24) |
        ((guint32) data[pos + 1] << 16) |
        ((guint32) data[pos + 2] << 8) |
        (guint32) data[pos + 3];
    pos += 4;

    if (nal_size == 0 || pos + nal_size > size)
      return FALSE;

    guint8 nal_type = (data[pos] >> 1) & 0x3f;
    found_nal = TRUE;
    if ((nal_type == 19 || nal_type == 20) && is_keyframe != NULL)
      *is_keyframe = TRUE;
    if (nal_type == 32 || nal_type == 33 || nal_type == 34) {
      if (has_parameter_sets != NULL)
        *has_parameter_sets = TRUE;
    }

    pos += nal_size;
  }

  return found_nal;
}

gboolean
gst_rtsp_sink_h265_analyze_buffer(const GstBuffer *buffer,
                                  const gchar *stream_format,
                                  gboolean *is_keyframe,
                                  gboolean *has_parameter_sets)
{
  GstMapInfo map;
  gboolean ok;

  if (is_keyframe != NULL)
    *is_keyframe = FALSE;
  if (has_parameter_sets != NULL)
    *has_parameter_sets = FALSE;

  if (buffer == NULL)
    return FALSE;

  if (!gst_buffer_map((GstBuffer *) buffer, &map, GST_MAP_READ))
    return FALSE;

  if (g_strcmp0(stream_format, "hvc1") == 0)
    ok = gst_rtsp_sink_h265_scan_hvc1(map.data, map.size, is_keyframe, has_parameter_sets);
  else
    ok = gst_rtsp_sink_h265_scan_nals(map.data, map.size, is_keyframe, has_parameter_sets);

  gst_buffer_unmap((GstBuffer *) buffer, &map);
  return ok;
}

gboolean
gst_rtsp_sink_h265_packetize(GstBuffer *buffer)
{
  return gst_rtsp_sink_h265_analyze_buffer(buffer, "byte-stream", NULL, NULL);
}
