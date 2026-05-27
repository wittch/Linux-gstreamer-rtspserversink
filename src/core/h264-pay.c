#include "rtsp-server-internal.h"

static guint32
buffer_rtptime_unlocked (GstRTSPSinkServer *server, GstClockTime pts)
{
  guint64 scaled;

  if (!GST_CLOCK_TIME_IS_VALID (pts))
    pts = 0;

  if (!server->have_clock_base) {
    server->base_pts = pts;
    server->have_clock_base = TRUE;
  }

  if (pts < server->base_pts)
    pts = server->base_pts;

  scaled = gst_util_uint64_scale (pts - server->base_pts, RTP_CLOCK_RATE,
      GST_SECOND);
  return (guint32) scaled;
}

static gboolean
send_interleaved_packet (GstRTSPSinkClient *client, guint8 channel,
    const guint8 *payload, gsize payload_size)
{
  guint8 header[4];
  GByteArray *packet;
  gboolean ok;

  if (payload_size > 0xffff)
    return FALSE;

  header[0] = '$';
  header[1] = channel;
  header[2] = (payload_size >> 8) & 0xff;
  header[3] = payload_size & 0xff;

  packet = g_byte_array_sized_new (4 + payload_size);
  g_byte_array_append (packet, header, sizeof (header));
  g_byte_array_append (packet, payload, payload_size);
  ok = gst_rtsp_sink_client_write_bytes (client, packet->data, packet->len,
      FALSE);
  g_byte_array_unref (packet);

  return ok;
}

static gboolean
send_udp_packet (GSocket *socket, GSocketAddress *address, const guint8 *payload,
    gsize payload_size, GstRTSPSinkClient *client)
{
  GError *error = NULL;
  gssize written;

  written = g_socket_send_to (socket, address, (const gchar *) payload,
      payload_size, NULL, &error);
  if (written < 0 || (gsize) written != payload_size) {
    g_clear_error (&error);
    client->closed = TRUE;
    client->state = GST_RTSP_SINK_STATE_CLOSED;
    return FALSE;
  }

  return TRUE;
}

static void
append_u32_be (GByteArray *array, guint32 value)
{
  guint8 bytes[4];

  bytes[0] = (value >> 24) & 0xff;
  bytes[1] = (value >> 16) & 0xff;
  bytes[2] = (value >> 8) & 0xff;
  bytes[3] = value & 0xff;
  g_byte_array_append (array, bytes, sizeof (bytes));
}

static void
append_u64_ntp_be (GByteArray *array)
{
  gint64 now_us = g_get_real_time ();
  guint64 unix_seconds = now_us / G_USEC_PER_SEC;
  guint64 ntp_seconds = unix_seconds + 2208988800ULL;
  guint64 fractional = ((now_us % G_USEC_PER_SEC) << 32) / G_USEC_PER_SEC;

  append_u32_be (array, (guint32) ntp_seconds);
  append_u32_be (array, (guint32) fractional);
}

void
gst_rtsp_sink_client_maybe_send_rtcp (GstRTSPSinkClient *client,
    gboolean force_sender_report, gboolean send_bye)
{
  GByteArray *packet;
  guint32 packet_ssrc;
  gint64 now_us;
  gsize packet_size;

  if (client->transport != GST_RTSP_SINK_TRANSPORT_UDP ||
      client->server_rtcp_socket == NULL || client->client_rtcp_addr == NULL)
    return;

  now_us = g_get_monotonic_time ();
  if (!force_sender_report && !send_bye &&
      client->last_rtcp_monotonic_us != 0 &&
      now_us - client->last_rtcp_monotonic_us < 5 * G_USEC_PER_SEC)
    return;

  packet_ssrc = client->ssrc;
  packet = g_byte_array_new ();

  if (!send_bye) {
    guint8 sr_header[] = { 0x80, 200, 0, 6 };

    g_byte_array_append (packet, sr_header, sizeof (sr_header));
    append_u32_be (packet, packet_ssrc);
    append_u64_ntp_be (packet);
    append_u32_be (packet, client->have_rtp_info ? client->last_rtptime : 0);
    append_u32_be (packet, (guint32) client->packet_count);
    append_u32_be (packet, (guint32) client->octet_count);

    if (client->rtcp_cname != NULL) {
      guint8 sdes_header[] = { 0x81, 202, 0, 0 };
      guint8 item_type = 1;
      guint8 cname_len = (guint8) MIN (strlen (client->rtcp_cname), 255);
      guint8 end = 0;
      guint pad;
      guint16 words;

      g_byte_array_append (packet, sdes_header, sizeof (sdes_header));
      append_u32_be (packet, packet_ssrc);
      g_byte_array_append (packet, &item_type, 1);
      g_byte_array_append (packet, &cname_len, 1);
      g_byte_array_append (packet, (const guint8 *) client->rtcp_cname, cname_len);
      g_byte_array_append (packet, &end, 1);

      pad = (4 - (packet->len % 4)) % 4;
      while (pad-- > 0)
        g_byte_array_append (packet, &end, 1);

      words = (guint16) ((packet->len - 28) / 4);
      packet->data[30] = (words >> 8) & 0xff;
      packet->data[31] = words & 0xff;
    }
  }

  if (send_bye) {
    guint8 bye_header[] = { 0x81, 203, 0, 1 };

    g_byte_array_append (packet, bye_header, sizeof (bye_header));
    append_u32_be (packet, packet_ssrc);
  }

  packet_size = packet->len;
  send_udp_packet (client->server_rtcp_socket, client->client_rtcp_addr,
      packet->data, packet_size, client);
  client->last_rtcp_monotonic_us = now_us;
  g_byte_array_unref (packet);
}

static gboolean
send_rtp_packet (GstRTSPSinkClient *client, const guint8 *payload,
    gsize payload_size, gboolean marker, guint32 rtptime)
{
  guint8 packet[12 + RTP_MAX_PAYLOAD];
  guint16 seq = client->seqnum++;

  if (payload_size > RTP_MAX_PAYLOAD)
    return FALSE;

  packet[0] = 0x80;
  packet[1] = (marker ? 0x80 : 0x00) | RTP_PAYLOAD_TYPE;
  packet[2] = (seq >> 8) & 0xff;
  packet[3] = seq & 0xff;
  packet[4] = (rtptime >> 24) & 0xff;
  packet[5] = (rtptime >> 16) & 0xff;
  packet[6] = (rtptime >> 8) & 0xff;
  packet[7] = rtptime & 0xff;
  packet[8] = (client->ssrc >> 24) & 0xff;
  packet[9] = (client->ssrc >> 16) & 0xff;
  packet[10] = (client->ssrc >> 8) & 0xff;
  packet[11] = client->ssrc & 0xff;
  memcpy (packet + 12, payload, payload_size);

  client->have_rtp_info = TRUE;
  client->last_seq_sent = seq;
  client->last_rtptime = rtptime;
  client->packet_count++;
  client->octet_count += payload_size;

  if (client->transport == GST_RTSP_SINK_TRANSPORT_TCP_INTERLEAVED)
    return send_interleaved_packet (client, client->rtp_channel, packet,
        12 + payload_size);

  if (client->transport == GST_RTSP_SINK_TRANSPORT_UDP &&
      client->server_rtp_socket != NULL && client->client_rtp_addr != NULL) {
    if (!send_udp_packet (client->server_rtp_socket, client->client_rtp_addr,
            packet, 12 + payload_size, client))
      return FALSE;
    gst_rtsp_sink_client_maybe_send_rtcp (client, FALSE, FALSE);
    return TRUE;
  }

  return FALSE;
}

static gboolean
send_h264_nal (GstRTSPSinkClient *client, const guint8 *nal, gsize nal_size,
    gboolean marker, guint32 rtptime)
{
  guint8 nal_type;
  guint8 nri;
  gsize offset;
  guint8 fu_indicator;
  guint8 fu_header;
  gboolean start;
  gboolean end;
  guint8 payload[RTP_MAX_PAYLOAD];
  gsize chunk;

  if (nal_size == 0)
    return TRUE;
  if (nal_size <= RTP_MAX_PAYLOAD)
    return send_rtp_packet (client, nal, nal_size, marker, rtptime);

  nal_type = nal[0] & 0x1f;
  nri = nal[0] & 0x60;
  fu_indicator = nri | 28;
  offset = 1;
  start = TRUE;

  while (offset < nal_size) {
    chunk = MIN ((gsize) (RTP_MAX_PAYLOAD - 2), nal_size - offset);
    end = (offset + chunk) == nal_size;
    fu_header = nal_type;
    if (start)
      fu_header |= 0x80;
    if (end)
      fu_header |= 0x40;

    payload[0] = fu_indicator;
    payload[1] = fu_header;
    memcpy (payload + 2, nal + offset, chunk);

    if (!send_rtp_packet (client, payload, chunk + 2, marker && end, rtptime))
      return FALSE;

    start = FALSE;
    offset += chunk;
  }

  return TRUE;
}

static gboolean
send_stap_a (GstRTSPSinkClient *client, guint32 rtptime)
{
  GstRTSPSinkServer *server = client->server;
  GByteArray *payload;
  GByteArray *sps = NULL;
  GByteArray *pps = NULL;
  guint8 indicator = 24;
  guint16 size16;
  gboolean ok;

  g_mutex_lock (&server->lock);
  if (server->sps != NULL) {
    sps = g_byte_array_sized_new (server->sps->len);
    g_byte_array_append (sps, server->sps->data, server->sps->len);
  }
  if (server->pps != NULL) {
    pps = g_byte_array_sized_new (server->pps->len);
    g_byte_array_append (pps, server->pps->data, server->pps->len);
  }
  g_mutex_unlock (&server->lock);

  if (sps == NULL || pps == NULL) {
    if (sps != NULL)
      g_byte_array_unref (sps);
    if (pps != NULL)
      g_byte_array_unref (pps);
    return TRUE;
  }

  payload = g_byte_array_new ();
  g_byte_array_append (payload, &indicator, 1);

  size16 = g_htons ((guint16) sps->len);
  g_byte_array_append (payload, (guint8 *) &size16, 2);
  g_byte_array_append (payload, sps->data, sps->len);

  size16 = g_htons ((guint16) pps->len);
  g_byte_array_append (payload, (guint8 *) &size16, 2);
  g_byte_array_append (payload, pps->data, pps->len);

  ok = send_rtp_packet (client, payload->data, payload->len, FALSE, rtptime);
  g_byte_array_unref (payload);
  g_byte_array_unref (sps);
  g_byte_array_unref (pps);

  return ok;
}

static void
for_each_playing_client (GstRTSPSinkServer *server,
    void (*func) (GstRTSPSinkClient *, gpointer), gpointer user_data)
{
  GList *snapshot = NULL;
  GList *iter;

  g_mutex_lock (&server->lock);
  for (iter = server->clients; iter != NULL; iter = iter->next) {
    GstRTSPSinkClient *client = iter->data;

    if (!client->closed && client->state == GST_RTSP_SINK_STATE_PLAYING)
      snapshot = g_list_prepend (snapshot, client);
  }
  g_mutex_unlock (&server->lock);

  for (iter = snapshot; iter != NULL; iter = iter->next) {
    GstRTSPSinkClient *client = iter->data;

    if (!client->closed && client->state == GST_RTSP_SINK_STATE_PLAYING)
      func (client, user_data);
  }

  g_list_free (snapshot);
}

static void
broadcast_nal_cb (GstRTSPSinkClient *client, gpointer user_data)
{
  BroadcastNalData *data = user_data;

  if (client->closed || client->state != GST_RTSP_SINK_STATE_PLAYING)
    return;

  if (client->wait_for_live_idr) {
    if (!data->prepend_stap)
      return;
    client->wait_for_live_idr = FALSE;
  }

  if (data->prepend_stap)
    send_stap_a (client, data->rtptime);
  send_h264_nal (client, data->nal, data->nal_size, data->marker,
      data->rtptime);
}

static void
broadcast_avc_au (GstRTSPSinkServer *server, const guint8 *data, gsize size,
    guint32 rtptime)
{
  gsize offset = 0;
  GPtrArray *nals = g_ptr_array_new ();
  GArray *sizes = g_array_new (FALSE, FALSE, sizeof (gsize));
  guint i;
  BroadcastNalData payload;

  while (offset + server->nal_length_size <= size) {
    guint32 nal_size = 0;
    gsize nal_size_value;
    const guint8 *nal;

    if (server->nal_length_size == 4)
      nal_size = GST_READ_UINT32_BE (data + offset);
    else if (server->nal_length_size == 2)
      nal_size = GST_READ_UINT16_BE (data + offset);
    else if (server->nal_length_size == 1)
      nal_size = data[offset];
    else
      break;

    offset += server->nal_length_size;
    if (offset + nal_size > size)
      break;

    nal = data + offset;
    gst_rtsp_sink_server_note_nal (server, nal, nal_size);
    nal_size_value = nal_size;
    g_ptr_array_add (nals, (gpointer) nal);
    g_array_append_val (sizes, nal_size_value);
    offset += nal_size;
  }

  for (i = 0; i < nals->len; i++) {
    const guint8 *nal = g_ptr_array_index (nals, i);
    gsize nal_size = g_array_index (sizes, gsize, i);
    guint8 nal_type = nal[0] & 0x1f;

    payload.nal = nal;
    payload.nal_size = nal_size;
    payload.marker = (i + 1) == nals->len;
    payload.prepend_stap = (nal_type == 5);
    payload.rtptime = rtptime;
    for_each_playing_client (server, broadcast_nal_cb, &payload);
  }

  g_array_unref (sizes);
  g_ptr_array_unref (nals);
}

static void
broadcast_bytestream_au (GstRTSPSinkServer *server, const guint8 *data,
    gsize size, guint32 rtptime)
{
  gsize i = 0;
  GPtrArray *nals = g_ptr_array_new ();
  GArray *sizes = g_array_new (FALSE, FALSE, sizeof (gsize));
  guint idx;
  BroadcastNalData payload;

  while (i + 3 < size) {
    gsize start = G_MAXSIZE;
    gsize end = size;
    gsize prefix = 0;
    const guint8 *nal;
    gsize nal_size;

    while (i + 3 < size) {
      if (i + 4 < size && data[i] == 0 && data[i + 1] == 0 &&
          data[i + 2] == 0 && data[i + 3] == 1) {
        start = i;
        prefix = 4;
        break;
      }
      if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
        start = i;
        prefix = 3;
        break;
      }
      i++;
    }
    if (start == G_MAXSIZE)
      break;

    i = start + prefix;
    end = i;
    while (end + 3 < size) {
      if ((end + 4 < size && data[end] == 0 && data[end + 1] == 0 &&
              data[end + 2] == 0 && data[end + 3] == 1) ||
          (data[end] == 0 && data[end + 1] == 0 && data[end + 2] == 1))
        break;
      end++;
    }

    nal = data + i;
    nal_size = end - i;
    if (nal_size > 0) {
      gst_rtsp_sink_server_note_nal (server, nal, nal_size);
      g_ptr_array_add (nals, (gpointer) nal);
      g_array_append_val (sizes, nal_size);
    }
    i = end;
  }

  for (idx = 0; idx < nals->len; idx++) {
    const guint8 *nal = g_ptr_array_index (nals, idx);
    gsize nal_size = g_array_index (sizes, gsize, idx);
    guint8 nal_type = nal[0] & 0x1f;

    payload.nal = nal;
    payload.nal_size = nal_size;
    payload.marker = (idx + 1) == nals->len;
    payload.prepend_stap = (nal_type == 5);
    payload.rtptime = rtptime;
    for_each_playing_client (server, broadcast_nal_cb, &payload);
  }

  g_array_unref (sizes);
  g_ptr_array_unref (nals);
}

gboolean
gst_rtsp_sink_server_set_h264_caps_internal (GstRTSPSinkServer *server,
    GstCaps *caps, GError **error)
{
  GstStructure *s;
  const gchar *stream_format;
  const GValue *codec_data_value;
  GstBuffer *codec_data = NULL;

  s = gst_caps_get_structure (caps, 0);
  stream_format = gst_structure_get_string (s, "stream-format");
  if (stream_format == NULL) {
    g_set_error (error, GST_STREAM_ERROR, GST_STREAM_ERROR_FORMAT,
        "missing H264 stream-format");
    return FALSE;
  }

  g_mutex_lock (&server->lock);
  server->avc_format = g_str_equal (stream_format, "avc") ||
      g_str_equal (stream_format, "avc3");
  server->nal_length_size = 4;

  codec_data_value = gst_structure_get_value (s, "codec_data");
  if (codec_data_value != NULL)
    codec_data = gst_value_get_buffer (codec_data_value);
  if (server->avc_format && codec_data != NULL &&
      !gst_rtsp_sink_parse_avcc_codec_data (server, codec_data)) {
    g_mutex_unlock (&server->lock);
    g_set_error (error, GST_STREAM_ERROR, GST_STREAM_ERROR_FORMAT,
        "invalid avc codec_data");
    return FALSE;
  }

  gst_rtsp_sink_sdp_update_unlocked (server);
  g_mutex_unlock (&server->lock);

  return TRUE;
}

void
gst_rtsp_sink_server_push_buffer_internal (GstRTSPSinkServer *server,
    GstBuffer *buffer)
{
  GstMapInfo map;
  guint32 rtptime;

  if (!gst_buffer_map (buffer, &map, GST_MAP_READ))
    return;

  g_mutex_lock (&server->lock);
  if (server->stopping) {
    g_mutex_unlock (&server->lock);
    gst_buffer_unmap (buffer, &map);
    return;
  }
  server->active_pushers++;
  rtptime = buffer_rtptime_unlocked (server, GST_BUFFER_PTS (buffer));
  server->latest_rtptime = rtptime;
  {
    gboolean avc_format = server->avc_format;

    g_mutex_unlock (&server->lock);
    if (avc_format)
      broadcast_avc_au (server, map.data, map.size, rtptime);
    else
      broadcast_bytestream_au (server, map.data, map.size, rtptime);
  }

  g_mutex_lock (&server->lock);
  server->active_pushers--;
  if (server->stopping && server->active_pushers == 0)
    g_cond_signal (&server->cond);
  g_mutex_unlock (&server->lock);

  gst_buffer_unmap (buffer, &map);
}
