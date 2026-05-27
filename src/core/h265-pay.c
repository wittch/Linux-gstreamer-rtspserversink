#include "rtsp-server-internal.h"

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

static guint8
h265_nal_type (const guint8 *nal)
{
  return (nal[0] >> 1) & 0x3f;
}

static gboolean
h265_is_irap (const guint8 *nal, gsize nal_size)
{
  guint8 type;

  if (nal_size < 2)
    return FALSE;
  type = h265_nal_type (nal);
  return type >= 16 && type <= 23;
}

static gboolean
send_h265_nal (GstRTSPSinkClient *client, const guint8 *nal, gsize nal_size,
    gboolean marker, guint32 rtptime)
{
  guint8 payload[RTP_MAX_PAYLOAD];
  guint8 nal_type;
  gsize offset;
  gboolean start;

  if (nal_size < 3)
    return TRUE;
  if (nal_size <= RTP_MAX_PAYLOAD)
    return send_rtp_packet (client, nal, nal_size, marker, rtptime);

  nal_type = h265_nal_type (nal);
  payload[0] = (nal[0] & 0x81) | (49 << 1);
  payload[1] = nal[1];
  offset = 2;
  start = TRUE;

  while (offset < nal_size) {
    gboolean end;
    gsize chunk = MIN ((gsize) (RTP_MAX_PAYLOAD - 3), nal_size - offset);

    end = (offset + chunk) == nal_size;
    payload[2] = nal_type;
    if (start)
      payload[2] |= 0x80;
    if (end)
      payload[2] |= 0x40;
    memcpy (payload + 3, nal + offset, chunk);

    if (!send_rtp_packet (client, payload, chunk + 3, marker && end, rtptime))
      return FALSE;

    start = FALSE;
    offset += chunk;
  }

  return TRUE;
}

static gboolean
send_h265_parameter_sets (GstRTSPSinkClient *client, guint32 rtptime)
{
  GstRTSPSinkServer *server = client->server;
  GByteArray *vps = NULL;
  GByteArray *sps = NULL;
  GByteArray *pps = NULL;
  gboolean ok = TRUE;

  g_mutex_lock (&server->lock);
  if (server->h265_vps != NULL && server->h265_vps->len > 0) {
    vps = g_byte_array_sized_new (server->h265_vps->len);
    g_byte_array_append (vps, server->h265_vps->data, server->h265_vps->len);
  }
  if (server->h265_sps != NULL && server->h265_sps->len > 0) {
    sps = g_byte_array_sized_new (server->h265_sps->len);
    g_byte_array_append (sps, server->h265_sps->data, server->h265_sps->len);
  }
  if (server->h265_pps != NULL && server->h265_pps->len > 0) {
    pps = g_byte_array_sized_new (server->h265_pps->len);
    g_byte_array_append (pps, server->h265_pps->data, server->h265_pps->len);
  }
  g_mutex_unlock (&server->lock);

  if (vps == NULL || sps == NULL || pps == NULL) {
    if (vps != NULL)
      g_byte_array_unref (vps);
    if (sps != NULL)
      g_byte_array_unref (sps);
    if (pps != NULL)
      g_byte_array_unref (pps);
    return TRUE;
  }

  ok = send_h265_nal (client, vps->data, vps->len, FALSE, rtptime);
  if (ok)
    ok = send_h265_nal (client, sps->data, sps->len, FALSE, rtptime);
  if (ok)
    ok = send_h265_nal (client, pps->data, pps->len, FALSE, rtptime);
  g_byte_array_unref (vps);
  g_byte_array_unref (sps);
  g_byte_array_unref (pps);

  return ok;
}

static void
broadcast_h265_nal_cb (GstRTSPSinkClient *client, gpointer user_data)
{
  BroadcastNalData *data = user_data;

  if (client->closed || client->state != GST_RTSP_SINK_STATE_PLAYING)
    return;

  if (client->wait_for_live_idr) {
    if (!data->au_has_idr)
      return;
    if (data->first_in_au) {
      if (!send_h265_parameter_sets (client, data->rtptime))
        return;
      client->wait_for_live_idr = FALSE;
    }
  }

  send_h265_nal (client, data->nal, data->nal_size, data->marker,
      data->rtptime);
}

static void
broadcast_h265_nals (GstRTSPSinkServer *server, GPtrArray *nals, GArray *sizes,
    guint32 rtptime, gboolean au_has_irap)
{
  guint i;
  BroadcastNalData payload;

  for (i = 0; i < nals->len; i++) {
    payload.nal = g_ptr_array_index (nals, i);
    payload.nal_size = g_array_index (sizes, gsize, i);
    payload.marker = (i + 1) == nals->len;
    payload.au_has_idr = au_has_irap;
    payload.first_in_au = (i == 0);
    payload.rtptime = rtptime;
    for_each_playing_client (server, broadcast_h265_nal_cb, &payload);
  }
}

static void
broadcast_h265_length_prefixed_au (GstRTSPSinkServer *server, const guint8 *data,
    gsize size, guint32 rtptime)
{
  gsize offset = 0;
  GPtrArray *nals = g_ptr_array_new ();
  GArray *sizes = g_array_new (FALSE, FALSE, sizeof (gsize));
  gboolean au_has_irap = FALSE;

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
    if (h265_is_irap (nal, nal_size))
      au_has_irap = TRUE;
    offset += nal_size;
  }

  broadcast_h265_nals (server, nals, sizes, rtptime, au_has_irap);
  g_array_unref (sizes);
  g_ptr_array_unref (nals);
}

static void
broadcast_h265_bytestream_au (GstRTSPSinkServer *server, const guint8 *data,
    gsize size, guint32 rtptime)
{
  gsize i = 0;
  GPtrArray *nals = g_ptr_array_new ();
  GArray *sizes = g_array_new (FALSE, FALSE, sizeof (gsize));
  gboolean au_has_irap = FALSE;

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
      if (h265_is_irap (nal, nal_size))
        au_has_irap = TRUE;
    }
    i = end;
  }

  broadcast_h265_nals (server, nals, sizes, rtptime, au_has_irap);
  g_array_unref (sizes);
  g_ptr_array_unref (nals);
}

gboolean
gst_rtsp_sink_server_set_h265_caps_internal (GstRTSPSinkServer *server,
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
        "missing H265 stream-format");
    return FALSE;
  }

  g_mutex_lock (&server->lock);
  gst_rtsp_sink_server_reset_codec_state_unlocked (server);
  server->codec = GST_RTSP_SINK_CODEC_H265;
  server->length_prefixed_format = g_str_equal (stream_format, "hvc1") ||
      g_str_equal (stream_format, "hev1");
  server->nal_length_size = 4;

  codec_data_value = gst_structure_get_value (s, "codec_data");
  if (codec_data_value != NULL)
    codec_data = gst_value_get_buffer (codec_data_value);
  if (server->length_prefixed_format && codec_data != NULL &&
      !gst_rtsp_sink_parse_hvcc_codec_data (server, codec_data)) {
    g_mutex_unlock (&server->lock);
    g_set_error (error, GST_STREAM_ERROR, GST_STREAM_ERROR_FORMAT,
        "invalid hvc codec_data");
    return FALSE;
  }

  gst_rtsp_sink_sdp_update_unlocked (server);
  g_mutex_unlock (&server->lock);

  return TRUE;
}

void
gst_rtsp_sink_server_broadcast_h265 (GstRTSPSinkServer *server,
    const guint8 *data, gsize size, guint32 rtptime)
{
  if (server->length_prefixed_format)
    broadcast_h265_length_prefixed_au (server, data, size, rtptime);
  else
    broadcast_h265_bytestream_au (server, data, size, rtptime);
}
