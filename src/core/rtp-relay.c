#include "rtsp-server-internal.h"

static const gchar *
rtp_transport_name (GstRTSPSinkTransport transport)
{
  switch (transport) {
    case GST_RTSP_SINK_TRANSPORT_UDP:
      return "udp";
    case GST_RTSP_SINK_TRANSPORT_TCP_INTERLEAVED:
      return "tcp";
    default:
      return "none";
  }
}

static const gchar *
rtsp_session_state_name (GstRTSPSinkSessionState state)
{
  switch (state) {
    case GST_RTSP_SINK_STATE_INIT:
      return "INIT";
    case GST_RTSP_SINK_STATE_READY:
      return "READY";
    case GST_RTSP_SINK_STATE_PLAYING:
      return "PLAYING";
    case GST_RTSP_SINK_STATE_PAUSED:
      return "PAUSED";
    case GST_RTSP_SINK_STATE_CLOSED:
      return "CLOSED";
    default:
      return "UNKNOWN";
  }
}

static gchar *
rtp_prefix_hex (const guint8 *data, gsize size)
{
  GString *hex;
  gsize i;
  gsize limit = MIN (size, 16);

  hex = g_string_sized_new (limit * 2);
  for (i = 0; i < limit; i++)
    g_string_append_printf (hex, "%02x", data[i]);
  return g_string_free (hex, FALSE);
}

static gboolean
rtp_seqnum_is_newer (guint16 seqnum, guint16 prev_seqnum)
{
  guint16 delta = seqnum - prev_seqnum;

  return delta != 0 && delta < 0x8000;
}

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
rtp_packet_info_equal (const RtpPacketInfo *a, const RtpPacketInfo *b)
{
  if (a == NULL || b == NULL)
    return FALSE;

  return a->size == b->size &&
      a->payload_type == b->payload_type &&
      a->timestamp == b->timestamp &&
      a->seqnum == b->seqnum &&
      a->ssrc == b->ssrc &&
      a->header_len == b->header_len &&
      a->payload_len == b->payload_len &&
      a->marker == b->marker;
}

static gboolean send_packet_to_client (GstRTSPSinkClient *client,
    const guint8 *data, gsize size, const RtpPacketInfo *expected_info);

static void
server_cache_warm_start_packet_unlocked (GstRTSPSinkServer *server,
    const RtpPacketInfo *info)
{
  GstRTSPQueuedRtpPacket *packet;

  if (server == NULL || server->warm_start_packets == NULL || info == NULL)
    return;

  packet = gst_rtsp_sink_rtp_queue_packet_new (info->data, info->size, info);
  g_queue_push_tail (server->warm_start_packets, packet);

  while (g_queue_get_length (server->warm_start_packets) >
      server->warm_start_max_packets) {
    GstRTSPQueuedRtpPacket *dropped = g_queue_pop_head (server->warm_start_packets);

    gst_rtsp_sink_rtp_queue_packet_free (dropped);
  }
}

gboolean
gst_rtsp_sink_server_replay_warm_start (GstRTSPSinkServer *server,
    GstRTSPSinkClient *client)
{
  GPtrArray *snapshot;
  GList *iter;
  guint i;
  gboolean ok = TRUE;

  g_return_val_if_fail (server != NULL, FALSE);
  g_return_val_if_fail (client != NULL, FALSE);

  snapshot = g_ptr_array_new_with_free_func (
      (GDestroyNotify) gst_rtsp_sink_rtp_queue_packet_free);

  g_mutex_lock (&server->lock);
  if (server->warm_start_packets != NULL) {
    for (iter = server->warm_start_packets->head; iter != NULL;
        iter = iter->next) {
      GstRTSPQueuedRtpPacket *packet = iter->data;

      if (packet == NULL || !packet->have_info)
        continue;

      g_ptr_array_add (snapshot, gst_rtsp_sink_rtp_queue_packet_new (
              packet->data, packet->size, &packet->info));
    }
  }
  g_mutex_unlock (&server->lock);

  if (snapshot->len == 0) {
    g_ptr_array_unref (snapshot);
    return TRUE;
  }

  GST_DEBUG ("warm-start replay count=%u session=%s transport=%s",
      snapshot->len, GST_STR_NULL (client->session_id),
      rtp_transport_name (client->transport));

  for (i = 0; i < snapshot->len; i++) {
    GstRTSPQueuedRtpPacket *packet = g_ptr_array_index (snapshot, i);

    if (!send_packet_to_client (client, packet->data, packet->size,
            &packet->info)) {
      ok = FALSE;
      break;
    }
  }

  g_ptr_array_unref (snapshot);
  return ok;
}

static gint
server_playing_client_count_unlocked (GstRTSPSinkServer *server)
{
  GList *iter;
  gint count = 0;

  for (iter = server->clients; iter != NULL; iter = iter->next) {
    GstRTSPSinkClient *client = iter->data;

    if (!client->closed && client->state == GST_RTSP_SINK_STATE_PLAYING)
      count++;
  }

  return count;
}

static void
log_rtp_packet_snapshot (const gchar *tag, const RtpPacketInfo *info,
    GstRTSPSinkClient *client, gint queue_len, gint playing_clients)
{
  g_autofree gchar *prefix_hex = rtp_prefix_hex (info->data, info->size);
  const gchar *transport_name = client != NULL ?
      rtp_transport_name (client->transport) : "n/a";
  const gchar *client_state = client != NULL ?
      rtsp_session_state_name (client->state) : "n/a";

  GST_DEBUG ("%s pt=%u seq=%u ts=%u marker=%d ssrc=%08x size=%" G_GSIZE_FORMAT
      " head16=%s queue_len=%d client_state=%s transport=%s playing_clients=%d",
      tag, info->payload_type, info->seqnum, info->timestamp, info->marker,
      info->ssrc, info->size, prefix_hex, queue_len, client_state,
      transport_name, playing_clients);
}

static gboolean
send_interleaved_packet (GstRTSPSinkClient *client, guint8 channel,
    const guint8 *payload, gsize payload_size, const RtpPacketInfo *info)
{
  guint8 header[4];
  GByteArray *packet;
  gboolean ok;

  if (payload_size > 0xffff)
    return FALSE;

  g_assert_nonnull (info);
  g_assert_cmpuint (payload_size, ==, info->size);

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
    const guint8 *payload, gsize payload_size, GstRTSPSinkClient *client,
    const RtpPacketInfo *info)
{
  GError *error = NULL;
  gssize written;

  if (info != NULL)
    g_assert_cmpuint (payload_size, ==, info->size);
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
      packet->data, packet_size, client, NULL);
  client->last_rtcp_monotonic_us = now_us;
  g_byte_array_unref (packet);
}

static gboolean
send_packet_to_client (GstRTSPSinkClient *client, const guint8 *data,
    gsize size, const RtpPacketInfo *expected_info)
{
  RtpPacketInfo output_info;
  gboolean ok;
  g_autofree gchar *prefix_hex = rtp_prefix_hex (data, size);
  const gchar *transport_name;

  g_return_val_if_fail (client != NULL, FALSE);
  g_return_val_if_fail (expected_info != NULL, FALSE);
  transport_name = rtp_transport_name (client->transport);
  g_assert_cmpint (client->state, ==, GST_RTSP_SINK_STATE_PLAYING);
  g_assert_cmpuint (size, ==, expected_info->size);
  g_assert (rtp_packet_info_parse (data, size, &output_info));
  g_assert (rtp_packet_info_equal (expected_info, &output_info));
  g_assert_cmpuint (output_info.payload_type,
      ==, client->server != NULL && client->server->rtp_payload_type != 0 ?
      client->server->rtp_payload_type : output_info.payload_type);

  if (client->transport == GST_RTSP_SINK_TRANSPORT_TCP_INTERLEAVED) {
    ok = send_interleaved_packet (client, client->rtp_channel, data, size,
        &output_info);
  } else if (client->transport == GST_RTSP_SINK_TRANSPORT_UDP &&
      client->server_rtp_socket != NULL && client->client_rtp_addr != NULL) {
    ok = send_udp_packet (client->server_rtp_socket, client->client_rtp_addr,
        data, size, client, &output_info);
  } else {
    return FALSE;
  }

  GST_DEBUG ("rtp-send transport=%s state=%s pt=%u seq=%u ts=%u marker=%d "
      "ssrc=%08x size=%" G_GSIZE_FORMAT " head16=%s queue_len=%d",
      transport_name, rtsp_session_state_name (client->state),
      output_info.payload_type, output_info.seqnum,
      output_info.timestamp, output_info.marker, output_info.ssrc, size,
      prefix_hex,
      client->server != NULL && client->server->rtp_queue != NULL ?
      g_async_queue_length (client->server->rtp_queue) : -1);

  if (!ok)
    return FALSE;

  if (client->have_rtp_info &&
      !(rtp_seqnum_is_newer (output_info.seqnum, client->last_seq_sent) ||
        output_info.seqnum == client->last_seq_sent)) {
    GST_WARNING ("non-monotonic RTP sequence for client: prev=%u next=%u",
        client->last_seq_sent, output_info.seqnum);
  }

  client->have_rtp_info = TRUE;
  client->last_seq_sent = output_info.seqnum;
  client->last_rtptime = output_info.timestamp;
  client->packet_count++;
  client->octet_count += output_info.payload_len;

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
  g_autofree gchar *prefix_hex = rtp_prefix_hex (info->data, info->size);
  gint queue_len = server->rtp_queue != NULL ?
      g_async_queue_length (server->rtp_queue) : -1;
  gint playing_clients = server_playing_client_count_unlocked (server);

  GST_DEBUG ("rtp-recv pt=%u seq=%u ts=%u marker=%d ssrc=%08x size=%" G_GSIZE_FORMAT
      " head16=%s header=%" G_GSIZE_FORMAT " payload=%" G_GSIZE_FORMAT
      " queue_len=%d playing_clients=%d transport_caps_pt=%u",
      info->payload_type, info->seqnum, info->timestamp, info->marker,
      info->ssrc, info->size, prefix_hex, info->header_len, info->payload_len,
      queue_len, playing_clients, server->rtp_payload_type);
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
  //server_cache_warm_start_packet_unlocked (server, info);
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
  const GstRTSPQueuedRtpPacket *packet = user_data;
  RtpPacketInfo send_info;
  gint playing_clients = -1;

  if (client->closed || client->state != GST_RTSP_SINK_STATE_PLAYING)
    return;

  g_assert_nonnull (packet);
  g_assert (packet->have_info);
  g_assert (rtp_packet_info_parse (packet->data, packet->size, &send_info));
  if (client->server != NULL) {
    g_mutex_lock (&client->server->lock);
    playing_clients = server_playing_client_count_unlocked (client->server);
    g_mutex_unlock (&client->server->lock);
  }
  log_rtp_packet_snapshot ("rtp-send-pre", &send_info, client,
      client->server != NULL && client->server->rtp_queue != NULL ?
      g_async_queue_length (client->server->rtp_queue) : -1,
      playing_clients);
  if (!rtp_packet_info_equal (&packet->info, &send_info)) {
    g_warning ("queued RTP snapshot changed before send path");
    g_assert_not_reached ();
  }

  if (!send_packet_to_client (client, packet->data, packet->size,
          &packet->info))
    return;
}

void
gst_rtsp_sink_server_broadcast_rtp (GstRTSPSinkServer *server,
    const GstRTSPQueuedRtpPacket *packet)
{
  RtpPacketInfo send_info;

  g_return_if_fail (server != NULL);
  g_return_if_fail (packet != NULL);
  g_return_if_fail (packet->have_info);
  g_assert (rtp_packet_info_parse (packet->data, packet->size, &send_info));
  if (!rtp_packet_info_equal (&packet->info, &send_info)) {
    g_warning ("queued RTP snapshot changed before send path");
    g_assert_not_reached ();
  }

  record_server_rtp_state (server, &send_info);

  g_mutex_lock (&server->lock);
  log_rtp_packet (&send_info, server);
  g_assert_cmpuint (server->rtp_payload_type, ==, send_info.payload_type);
  if (!server_has_playing_clients_unlocked (server)) {
    g_mutex_unlock (&server->lock);
    return;
  }
  g_mutex_unlock (&server->lock);

  for_each_playing_client (server, broadcast_rtp_cb, (gpointer) packet);
}

static gboolean
gst_rtsp_sink_server_queue_rtp_packet (GstRTSPSinkServer *server,
    const guint8 *data, gsize size, const RtpPacketInfo *info)
{
  GstRTSPQueuedRtpPacket *packet;
  gint queue_len;

  if (info != NULL)
    record_server_rtp_state (server, info);

  g_mutex_lock (&server->lock);
  if (server->stopping || server->rtp_queue == NULL) {
    g_mutex_unlock (&server->lock);
    return FALSE;
  }
  server->active_pushers++;
  g_mutex_unlock (&server->lock);

  packet = gst_rtsp_sink_rtp_queue_packet_new (data, size, info);
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

  return TRUE;
}

void
gst_rtsp_sink_server_push_buffer_internal (GstRTSPSinkServer *server,
    GstBuffer *buffer)
{
  GstMapInfo map;
  RtpPacketInfo info;

  g_return_if_fail (server != NULL);
  g_return_if_fail (buffer != NULL);

  if (!gst_buffer_map (buffer, &map, GST_MAP_READ))
    return;

  if (!rtp_packet_info_parse (map.data, map.size, &info)) {
    GST_WARNING ("dropping non-RTP buffer on application/x-rtp pad");
    gst_buffer_unmap (buffer, &map);
    return;
  }

  g_mutex_lock (&server->lock);
  log_rtp_packet (&info, server);
  g_assert_cmpuint (server->rtp_payload_type, ==, info.payload_type);
  g_mutex_unlock (&server->lock);

  if (!gst_rtsp_sink_server_queue_rtp_packet (server, map.data, map.size,
          &info))
    GST_WARNING ("dropping RTP packet because server is stopping");

  gst_buffer_unmap (buffer, &map);
}
