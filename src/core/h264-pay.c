#include "rtsp-server-internal.h"

typedef struct
{
  const guint8 *data;
  gsize size;
  guint8 payload_type;
  guint32 timestamp;
  guint16 seqnum;
  guint32 ssrc;
  gsize header_len;
  gsize payload_len;
  gboolean marker;
} RtpPacketInfo;

static gboolean
rtp_packet_info_parse (const guint8 *data, gsize size, RtpPacketInfo *info)
{
  guint8 csrc_count;
  gboolean has_extension;
  gsize header_len;

  if (data == NULL || size < 12 || info == NULL)
    return FALSE;

  if (((data[0] >> 6) & 0x03) != 2)
    return FALSE;

  csrc_count = data[0] & 0x0f;
  has_extension = (data[0] & 0x10) != 0;
  header_len = 12 + ((gsize) csrc_count * 4);
  if (size < header_len)
    return FALSE;

  if (has_extension) {
    gsize extension_len;

    if (size < header_len + 4)
      return FALSE;
    extension_len = (gsize) ((data[header_len + 2] << 8) | data[header_len + 3]);
    header_len += 4 + (extension_len * 4);
    if (size < header_len)
      return FALSE;
  }

  info->data = data;
  info->size = size;
  info->payload_type = data[1] & 0x7f;
  info->seqnum = (guint16) ((data[2] << 8) | data[3]);
  info->timestamp = ((guint32) data[4] << 24) | ((guint32) data[5] << 16) |
      ((guint32) data[6] << 8) | (guint32) data[7];
  info->ssrc = ((guint32) data[8] << 24) | ((guint32) data[9] << 16) |
      ((guint32) data[10] << 8) | (guint32) data[11];
  info->header_len = header_len;
  info->payload_len = size - header_len;
  info->marker = (data[1] & 0x80) != 0;
  return TRUE;
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
send_udp_packet (GSocket *socket, GSocketAddress *address,
    const guint8 *payload, gsize payload_size, GstRTSPSinkClient *client)
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
      g_byte_array_append (packet, (const guint8 *) client->rtcp_cname,
          cname_len);
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
send_packet_to_client (GstRTSPSinkClient *client, const guint8 *data,
    gsize size, const RtpPacketInfo *info)
{
  gboolean ok;
  const gchar *transport_name = "none";

  if (client->transport == GST_RTSP_SINK_TRANSPORT_TCP_INTERLEAVED) {
    transport_name = "tcp";
    ok = send_interleaved_packet (client, client->rtp_channel, data, size);
  } else if (client->transport == GST_RTSP_SINK_TRANSPORT_UDP &&
      client->server_rtp_socket != NULL && client->client_rtp_addr != NULL) {
    transport_name = "udp";
    ok = send_udp_packet (client->server_rtp_socket, client->client_rtp_addr,
        data, size, client);
  } else {
    return FALSE;
  }

  GST_DEBUG ("rtp-forward transport=%s state=%d pt=%u seq=%u ts=%u marker=%d "
      "ssrc=%08x size=%" G_GSIZE_FORMAT " client_ssrc=%08x queue_len=%d",
      transport_name, client->state, info->payload_type, info->seqnum,
      info->timestamp, info->marker, info->ssrc, size, client->ssrc,
      client->server != NULL && client->server->rtp_queue != NULL ?
      g_async_queue_length (client->server->rtp_queue) : -1);

  if (!ok)
    return FALSE;

  client->have_rtp_info = TRUE;
  client->last_seq_sent = info->seqnum;
  client->last_rtptime = info->timestamp;
  client->packet_count++;
  client->octet_count += info->payload_len;

  if (client->transport == GST_RTSP_SINK_TRANSPORT_UDP)
    gst_rtsp_sink_client_maybe_send_rtcp (client, FALSE, FALSE);

  return TRUE;
}

static gboolean
server_has_playing_clients_unlocked (GstRTSPSinkServer *server)
{
  GList *iter;

  for (iter = server->clients; iter != NULL; iter = iter->next) {
    GstRTSPSinkClient *client = iter->data;

    if (!client->closed && client->state == GST_RTSP_SINK_STATE_PLAYING)
      return TRUE;
  }

  return FALSE;
}

static void
log_rtp_packet (const RtpPacketInfo *info, GstRTSPSinkServer *server)
{
  GST_DEBUG ("rtp-recv pt=%u seq=%u ts=%u marker=%d ssrc=%08x size=%" G_GSIZE_FORMAT
      " header=%" G_GSIZE_FORMAT " payload=%" G_GSIZE_FORMAT " queue_len=%d playing=%d",
      info->payload_type, info->seqnum, info->timestamp, info->marker,
      info->ssrc, info->size, info->header_len, info->payload_len,
      server->rtp_queue != NULL ? g_async_queue_length (server->rtp_queue) : -1,
      server_has_playing_clients_unlocked (server));
}

static void
record_server_rtp_state (GstRTSPSinkServer *server, const RtpPacketInfo *info)
{
  g_mutex_lock (&server->lock);
  if (!server->have_rtp_ssrc) {
    server->rtp_ssrc = info->ssrc;
    server->have_rtp_ssrc = TRUE;
  }
  server->have_latest_rtp = TRUE;
  server->latest_seqnum = info->seqnum;
  server->latest_rtptime = info->timestamp;
  g_mutex_unlock (&server->lock);
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
broadcast_rtp_cb (GstRTSPSinkClient *client, gpointer user_data)
{
  const RtpPacketInfo *info = user_data;

  if (client->closed || client->state != GST_RTSP_SINK_STATE_PLAYING)
    return;

  if (!send_packet_to_client (client, info->data, info->size, info))
    return;
}

void
gst_rtsp_sink_server_broadcast_rtp (GstRTSPSinkServer *server,
    const guint8 *data, gsize size)
{
  RtpPacketInfo info;

  if (!rtp_packet_info_parse (data, size, &info))
    return;

  record_server_rtp_state (server, &info);

  g_mutex_lock (&server->lock);
  log_rtp_packet (&info, server);
  if (!server_has_playing_clients_unlocked (server)) {
    g_mutex_unlock (&server->lock);
    return;
  }
  g_mutex_unlock (&server->lock);

  for_each_playing_client (server, broadcast_rtp_cb, &info);
}

void
gst_rtsp_sink_server_push_buffer_internal (GstRTSPSinkServer *server,
    GstBuffer *buffer)
{
  GstMapInfo map;
  GstRTSPQueuedRtpPacket *packet;
  gint queue_len;

  g_return_if_fail (server != NULL);
  g_return_if_fail (buffer != NULL);

  if (!gst_buffer_map (buffer, &map, GST_MAP_READ))
    return;

  g_mutex_lock (&server->lock);
  if (server->stopping || server->rtp_queue == NULL) {
    g_mutex_unlock (&server->lock);
    gst_buffer_unmap (buffer, &map);
    return;
  }
  server->active_pushers++;
  g_mutex_unlock (&server->lock);

  packet = gst_rtsp_sink_rtp_queue_packet_new (map.data, map.size);
  g_async_queue_push (server->rtp_queue, packet);

  while ((queue_len = g_async_queue_length (server->rtp_queue)) >
      (gint) server->rtp_queue_max_packets) {
    GstRTSPQueuedRtpPacket *dropped;

    dropped = g_async_queue_try_pop (server->rtp_queue);
    if (dropped == NULL)
      break;

    gst_rtsp_sink_rtp_queue_packet_free (dropped);
    g_mutex_lock (&server->lock);
    if (server->active_pushers > 0)
      server->active_pushers--;
    server->rtp_packets_dropped++;
    if (server->stopping && server->active_pushers == 0)
      g_cond_signal (&server->cond);
    g_mutex_unlock (&server->lock);

    GST_DEBUG ("rtp-queue-drop size=%d max=%u dropped=%" G_GUINT64_FORMAT,
        queue_len, server->rtp_queue_max_packets, server->rtp_packets_dropped);
  }

  GST_DEBUG ("rtp-queue-enqueue size=%d max=%u", g_async_queue_length (server->rtp_queue),
      server->rtp_queue_max_packets);
  gst_buffer_unmap (buffer, &map);
}
