#include "rtsp-server-internal.h"

static gchar *
make_profile_level_id_from_sps (const guint8 *data, gsize size)
{
  if (size < 4)
    return NULL;

  return g_strdup_printf ("%02X%02X%02X", data[1], data[2], data[3]);
}

static void
store_parameter_set (GByteArray **dst, const guint8 *data, gsize size)
{
  if (*dst == NULL)
    *dst = g_byte_array_new ();
  else
    g_byte_array_set_size (*dst, 0);

  g_byte_array_append (*dst, data, size);
}

static void
update_sprop_parameter_sets_unlocked (GstRTSPSinkServer *server)
{
  gchar *sps_b64 = NULL;
  gchar *pps_b64 = NULL;

  g_clear_pointer (&server->sprop_parameter_sets, g_free);
  if (server->sps == NULL || server->pps == NULL)
    return;

  sps_b64 = g_base64_encode (server->sps->data, server->sps->len);
  pps_b64 = g_base64_encode (server->pps->data, server->pps->len);
  server->sprop_parameter_sets = g_strdup_printf ("%s,%s", sps_b64, pps_b64);
  g_free (sps_b64);
  g_free (pps_b64);
}

void
gst_rtsp_sink_sdp_update_unlocked (GstRTSPSinkServer *server)
{
  gchar *fmtp;

  g_clear_pointer (&server->sdp, g_free);

  if (server->sprop_parameter_sets != NULL && server->profile_level_id != NULL) {
    fmtp = g_strdup_printf (
        "a=fmtp:%u packetization-mode=1;sprop-parameter-sets=%s;profile-level-id=%s\r\n",
        RTP_PAYLOAD_TYPE, server->sprop_parameter_sets, server->profile_level_id);
  } else {
    fmtp = g_strdup ("");
  }

  server->sdp = g_strdup_printf (
      "v=0\r\n"
      "o=- 0 0 IN IP4 %s\r\n"
      "s=GStreamer RTSP Sink\r\n"
      "t=0 0\r\n"
      "a=control:*\r\n"
      "m=video 0 RTP/AVP %u\r\n"
      "a=rtpmap:%u H264/%u\r\n"
      "%s"
      "a=control:stream=0\r\n",
      server->address != NULL ? server->address : "0.0.0.0",
      RTP_PAYLOAD_TYPE,
      RTP_PAYLOAD_TYPE,
      RTP_CLOCK_RATE,
      fmtp);
  g_free (fmtp);
}

void
gst_rtsp_sink_server_note_nal_unlocked (GstRTSPSinkServer *server,
    const guint8 *nal, gsize nal_size)
{
  guint8 nal_type;

  if (nal_size == 0)
    return;

  nal_type = nal[0] & 0x1f;
  if (nal_type == 7) {
    store_parameter_set (&server->sps, nal, nal_size);
    g_clear_pointer (&server->profile_level_id, g_free);
    server->profile_level_id = make_profile_level_id_from_sps (nal, nal_size);
    update_sprop_parameter_sets_unlocked (server);
    gst_rtsp_sink_sdp_update_unlocked (server);
  } else if (nal_type == 8) {
    store_parameter_set (&server->pps, nal, nal_size);
    update_sprop_parameter_sets_unlocked (server);
    gst_rtsp_sink_sdp_update_unlocked (server);
  }
}

void
gst_rtsp_sink_server_note_nal (GstRTSPSinkServer *server, const guint8 *nal,
    gsize nal_size)
{
  g_mutex_lock (&server->lock);
  gst_rtsp_sink_server_note_nal_unlocked (server, nal, nal_size);
  g_mutex_unlock (&server->lock);
}

gboolean
gst_rtsp_sink_parse_avcc_codec_data (GstRTSPSinkServer *server,
    GstBuffer *codec_data)
{
  GstMapInfo map;
  const guint8 *p;
  gsize remaining;
  guint num_sps;
  guint i;

  if (codec_data == NULL)
    return TRUE;
  if (!gst_buffer_map (codec_data, &map, GST_MAP_READ))
    return FALSE;
  if (map.size < 7 || map.data[0] != 1) {
    gst_buffer_unmap (codec_data, &map);
    return FALSE;
  }

  server->nal_length_size = (map.data[4] & 0x03) + 1;
  g_clear_pointer (&server->profile_level_id, g_free);
  server->profile_level_id = g_strdup_printf ("%02X%02X%02X", map.data[1],
      map.data[2], map.data[3]);

  p = map.data + 5;
  remaining = map.size - 5;
  num_sps = p[0] & 0x1f;
  p++;
  remaining--;

  for (i = 0; i < num_sps && remaining >= 2; i++) {
    guint16 size = (p[0] << 8) | p[1];

    p += 2;
    remaining -= 2;
    if (remaining < size)
      break;
    gst_rtsp_sink_server_note_nal_unlocked (server, p, size);
    p += size;
    remaining -= size;
  }

  if (remaining >= 1) {
    guint num_pps = p[0];
    guint j;

    p++;
    remaining--;
    for (j = 0; j < num_pps && remaining >= 2; j++) {
      guint16 size = (p[0] << 8) | p[1];

      p += 2;
      remaining -= 2;
      if (remaining < size)
        break;
      gst_rtsp_sink_server_note_nal_unlocked (server, p, size);
      p += size;
      remaining -= size;
    }
  }

  gst_buffer_unmap (codec_data, &map);
  return TRUE;
}
