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

typedef struct
{
  RtpPacketInfo info;
  gboolean has_keyframe;
} BroadcastRtpData;

typedef struct
{
  guint8 *data;
  gsize size;
  RtpPacketInfo info;
  gboolean has_keyframe;
} CachedRtpPacket;

static void
cached_rtp_packet_free (gpointer data)
{
  CachedRtpPacket *packet = data;

  if (packet == NULL)
    return;

  g_free (packet->data);
  g_free (packet);
}

static CachedRtpPacket *
cached_rtp_packet_new (const guint8 *data, gsize size,
    const RtpPacketInfo *info, gboolean has_keyframe)
{
  CachedRtpPacket *packet = g_new0 (CachedRtpPacket, 1);

  packet->data = g_memdup2 (data, size);
  packet->size = size;
  packet->info = *info;
  packet->info.data = packet->data;
  packet->has_keyframe = has_keyframe;
  return packet;
}

static void
ensure_rtp_au_buffers_unlocked (GstRTSPSinkServer *server)
{
  if (server->current_au_packets == NULL)
    server->current_au_packets = g_ptr_array_new_with_free_func (
        cached_rtp_packet_free);
  if (server->cached_idr_au_packets == NULL)
    server->cached_idr_au_packets = g_ptr_array_new_with_free_func (
        cached_rtp_packet_free);
}

static GPtrArray *
copy_rtp_au_packets (GPtrArray *src)
{
  GPtrArray *dst;
  guint i;

  if (src == NULL)
    return NULL;

  dst = g_ptr_array_new_with_free_func (cached_rtp_packet_free);
  for (i = 0; i < src->len; i++) {
    CachedRtpPacket *packet = g_ptr_array_index (src, i);
    CachedRtpPacket *copy;

    if (packet == NULL)
      continue;

    copy = cached_rtp_packet_new (packet->data, packet->size, &packet->info,
        packet->has_keyframe);
    g_ptr_array_add (dst, copy);
  }

  return dst;
}

void
gst_rtsp_sink_server_clear_rtp_au_cache_unlocked (GstRTSPSinkServer *server)
{
  if (server == NULL)
    return;

  if (server->current_au_packets != NULL)
    g_ptr_array_set_size (server->current_au_packets, 0);
  if (server->cached_idr_au_packets != NULL)
    g_ptr_array_set_size (server->cached_idr_au_packets, 0);
  server->current_au_has_idr = FALSE;
}

GPtrArray *
gst_rtsp_sink_server_copy_cached_idr_au_unlocked (GstRTSPSinkServer *server)
{
  if (server == NULL || server->cached_idr_au_packets == NULL ||
      server->cached_idr_au_packets->len == 0)
    return NULL;

  return copy_rtp_au_packets (server->cached_idr_au_packets);
}

gboolean
gst_rtsp_sink_server_get_rtp_au_start_info (GPtrArray *au_packets,
    guint16 *seqnum, guint32 *rtptime)
{
  CachedRtpPacket *packet;

  if (au_packets == NULL || au_packets->len == 0 || seqnum == NULL ||
      rtptime == NULL)
    return FALSE;

  packet = g_ptr_array_index (au_packets, 0);
  if (packet == NULL)
    return FALSE;

  *seqnum = packet->info.seqnum;
  *rtptime = packet->info.timestamp;
  return TRUE;
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
rtp_h264_payload_is_keyframe (const guint8 *payload, gsize payload_size)
{
  gsize offset = 0;

  if (payload == NULL || payload_size == 0)
    return FALSE;

  switch (payload[0] & 0x1f) {
    case 5:
      return TRUE;
    case 24:
      offset = 1;
      while (offset + 2 <= payload_size) {
        gsize nal_size = ((gsize) payload[offset] << 8) | payload[offset + 1];

        offset += 2;
        if (offset + nal_size > payload_size)
          break;
        if (nal_size > 0 && (payload[offset] & 0x1f) == 5)
          return TRUE;
        offset += nal_size;
      }
      return FALSE;
    case 28:
      if (payload_size < 2)
        return FALSE;
      return (payload[1] & 0x80) != 0 && (payload[1] & 0x1f) == 5;
    default:
      return FALSE;
  }
}

static gboolean
rtp_h265_nal_is_irap (guint8 nal_type)
{
  return nal_type >= 16 && nal_type <= 23;
}

static gboolean
rtp_h265_payload_is_keyframe (const guint8 *payload, gsize payload_size)
{
  gsize offset = 0;
  guint8 nal_type;

  if (payload == NULL || payload_size < 2)
    return FALSE;

  nal_type = (payload[0] >> 1) & 0x3f;
  if (rtp_h265_nal_is_irap (nal_type))
    return TRUE;

  switch (nal_type) {
    case 48:
      offset = 2;
      while (offset + 2 <= payload_size) {
        gsize nal_size = ((gsize) payload[offset] << 8) | payload[offset + 1];

        offset += 2;
        if (offset + nal_size > payload_size)
          break;
        if (nal_size >= 2 && rtp_h265_nal_is_irap ((payload[offset] >> 1) & 0x3f))
          return TRUE;
        offset += nal_size;
      }
      return FALSE;
    case 49:
      if (payload_size < 3)
        return FALSE;
      return (payload[2] & 0x80) != 0 &&
          rtp_h265_nal_is_irap (payload[2] & 0x3f);
    default:
      return FALSE;
  }
}

static gboolean
rtp_packet_is_keyframe (const gchar *encoding_name, const guint8 *payload,
    gsize payload_size)
{
  if (encoding_name == NULL)
    return FALSE;

  if (g_ascii_strcasecmp (encoding_name, "H264") == 0)
    return rtp_h264_payload_is_keyframe (payload, payload_size);
  if (g_ascii_strcasecmp (encoding_name, "H265") == 0)
    return rtp_h265_payload_is_keyframe (payload, payload_size);

  return FALSE;
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

  if (client->transport == GST_RTSP_SINK_TRANSPORT_TCP_INTERLEAVED)
    ok = send_interleaved_packet (client, client->rtp_channel, data, size);
  else if (client->transport == GST_RTSP_SINK_TRANSPORT_UDP &&
      client->server_rtp_socket != NULL && client->client_rtp_addr != NULL)
    ok = send_udp_packet (client->server_rtp_socket, client->client_rtp_addr,
        data, size, client);
  else
    return FALSE;

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
send_rtp_au_to_client_unlocked (GstRTSPSinkClient *client, GPtrArray *au_packets)
{
  guint i;

  if (client == NULL || au_packets == NULL)
    return FALSE;

  for (i = 0; i < au_packets->len; i++) {
    CachedRtpPacket *packet = g_ptr_array_index (au_packets, i);

    if (packet == NULL)
      continue;
    if (!send_packet_to_client (client, packet->data, packet->size,
            &packet->info))
      return FALSE;
  }

  return TRUE;
}

gboolean
gst_rtsp_sink_server_send_rtp_au_to_client (GstRTSPSinkServer *server,
    GstRTSPSinkClient *client, GPtrArray *au_packets)
{
  (void) server;

  return send_rtp_au_to_client_unlocked (client, au_packets);
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

static GList *
snapshot_waiting_clients (GstRTSPSinkServer *server)
{
  GList *snapshot = NULL;
  GList *iter;

  g_mutex_lock (&server->lock);
  for (iter = server->clients; iter != NULL; iter = iter->next) {
    GstRTSPSinkClient *client = iter->data;

    if (!client->closed && client->state == GST_RTSP_SINK_STATE_PLAYING &&
        client->wait_for_live_idr && client->pending_play_au == NULL)
      snapshot = g_list_prepend (snapshot, client);
  }
  g_mutex_unlock (&server->lock);

  return snapshot;
}

static void
send_cached_au_to_waiting_clients (GstRTSPSinkServer *server, GPtrArray *au)
{
  GList *snapshot;
  GList *iter;

  if (au == NULL || au->len == 0)
    return;

  snapshot = snapshot_waiting_clients (server);
  for (iter = snapshot; iter != NULL; iter = iter->next) {
    GstRTSPSinkClient *client = iter->data;

    if (client->closed || client->state != GST_RTSP_SINK_STATE_PLAYING ||
        !client->wait_for_live_idr)
      continue;

    if (send_rtp_au_to_client_unlocked (client, au))
      client->wait_for_live_idr = FALSE;
  }

  g_list_free (snapshot);
}

static void
broadcast_rtp_cb (GstRTSPSinkClient *client, gpointer user_data)
{
  const BroadcastRtpData *payload = user_data;

  if (client->closed || client->state != GST_RTSP_SINK_STATE_PLAYING)
    return;

  if (client->pending_play_au != NULL)
    return;

  if (client->wait_for_live_idr && !payload->has_keyframe)
    return;

  if (!send_packet_to_client (client, payload->info.data, payload->info.size,
          &payload->info))
    return;

  if (client->wait_for_live_idr && payload->has_keyframe)
    client->wait_for_live_idr = FALSE;
}

void
gst_rtsp_sink_server_broadcast_rtp (GstRTSPSinkServer *server,
    const guint8 *data, gsize size)
{
  RtpPacketInfo info;
  BroadcastRtpData payload;
  gchar *encoding_name = NULL;
  gboolean has_keyframe = FALSE;
  GPtrArray *replay_au = NULL;
  CachedRtpPacket *packet = NULL;

  if (!rtp_packet_info_parse (data, size, &info))
    return;

  g_mutex_lock (&server->lock);
  if (server->rtp_encoding_name != NULL)
    encoding_name = g_strdup (server->rtp_encoding_name);
  ensure_rtp_au_buffers_unlocked (server);
  g_mutex_unlock (&server->lock);

  if (encoding_name != NULL)
    has_keyframe = rtp_packet_is_keyframe (encoding_name, data + info.header_len,
        info.payload_len);

  record_server_rtp_state (server, &info);

  g_mutex_lock (&server->lock);
  packet = cached_rtp_packet_new (data, size, &info, has_keyframe);
  g_ptr_array_add (server->current_au_packets, packet);
  if (has_keyframe)
    server->current_au_has_idr = TRUE;
  if (info.marker) {
    if (server->current_au_has_idr) {
      GPtrArray *cached_copy = copy_rtp_au_packets (server->current_au_packets);

      if (cached_copy != NULL) {
        g_clear_pointer (&server->cached_idr_au_packets, g_ptr_array_unref);
        server->cached_idr_au_packets = copy_rtp_au_packets (server->current_au_packets);
        replay_au = cached_copy;
      }
    }
    g_ptr_array_set_size (server->current_au_packets, 0);
    server->current_au_has_idr = FALSE;
  }
  g_mutex_unlock (&server->lock);

  payload.info = info;
  payload.has_keyframe = has_keyframe;
  for_each_playing_client (server, broadcast_rtp_cb, &payload);

  if (replay_au != NULL) {
    send_cached_au_to_waiting_clients (server, replay_au);
    g_ptr_array_unref (replay_au);
  }

  g_free (encoding_name);
}

void
gst_rtsp_sink_server_push_buffer_internal (GstRTSPSinkServer *server,
    GstBuffer *buffer)
{
  GstMapInfo map;
  GstRTSPQueuedRtpPacket *packet;

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

  gst_buffer_unmap (buffer, &map);
}
