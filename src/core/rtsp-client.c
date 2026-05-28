#include "rtsp-server-internal.h"

#include "buffer-model.h"
#include "h264-pay.h"
#include "h265-pay.h"
#include "rtsp-parser.h"
#include "rtsp-router.h"

#include <gio/gio.h>
#include <string.h>

typedef struct _GstRTSPSinkClient
{
  GstRTSPSinkServer *server;
  GSocketConnection *connection;
  GInputStream *input;
  GOutputStream *output;
  GSocket *udp_socket;
  GSocketAddress *udp_rtp_address;
  GMutex write_lock;
  gchar *session_id;
  gint ref_count;
  GstRTSPSinkClientState state;
  gboolean use_tcp;
  guint client_rtp_port;
  guint client_rtcp_port;
  guint16 next_seqnum;
  guint32 timestamp_base;
  guint32 ssrc;
  gboolean have_reference_pts;
  GstClockTime reference_pts;
  guint64 last_activity_us;
  gboolean closed;
} GstRTSPSinkClient;

static gchar *
gst_rtsp_sink_client_duplicate_message(const gchar *message, gsize size)
{
  gchar *copy = g_malloc(size + 1);
  memcpy(copy, message, size);
  copy[size] = '\0';
  return copy;
}

static guint
gst_rtsp_sink_client_header_content_length(const gchar *message)
{
  gchar **lines;
  guint content_length = 0;

  if (message == NULL)
    return 0;

  lines = g_strsplit(message, "\r\n", -1);
  if (lines == NULL)
    return 0;

  for (guint i = 1; lines[i] != NULL; i++) {
    const gchar *line = lines[i];
    const gchar *colon;

    if (line[0] == '\0')
      break;

    colon = strchr(line, ':');
    if (colon == NULL)
      continue;

    if (g_ascii_strncasecmp(line, "Content-Length", colon - line) == 0) {
      content_length = (guint) g_ascii_strtoull(colon + 1, NULL, 10);
      break;
    }
  }

  g_strfreev(lines);
  return content_length;
}

static gboolean
gst_rtsp_sink_client_write_all(GstRTSPSinkClient *client,
                               const guint8 *data,
                               gsize size)
{
  gsize written = 0;
  GError *error = NULL;
  gboolean ok;

  if (client == NULL || data == NULL)
    return FALSE;

  g_mutex_lock(&client->write_lock);
  ok = g_output_stream_write_all(client->output, data, size, &written, NULL, &error);
  if (ok)
    ok = g_output_stream_flush(client->output, NULL, &error);
  if (!ok) {
    g_clear_error(&error);
    client->closed = TRUE;
  }
  g_mutex_unlock(&client->write_lock);

  return ok && written == size;
}

static gboolean
gst_rtsp_sink_client_response_ok(const gchar *response)
{
  return response != NULL && g_str_has_prefix(response, "RTSP/1.0 200 ");
}

static gchar *
gst_rtsp_sink_client_error_response(guint status_code, guint cseq)
{
  const gchar *reason = "Error";

  switch (status_code) {
    case 400: reason = "Bad Request"; break;
    case 454: reason = "Session Not Found"; break;
    case 455: reason = "Method Not Valid in This State"; break;
    case 461: reason = "Unsupported Transport"; break;
    case 500: reason = "Internal Server Error"; break;
    case 501: reason = "Not Implemented"; break;
    default: break;
  }

  return g_strdup_printf("RTSP/1.0 %u %s\r\nCSeq: %u\r\n\r\n", status_code, reason, cseq);
}

static gboolean
gst_rtsp_sink_client_parse_client_ports(const gchar *transport,
                                        guint *rtp_port,
                                        guint *rtcp_port)
{
  const gchar *client_port = NULL;
  gchar **tokens;
  gboolean ok = FALSE;

  if (rtp_port != NULL)
    *rtp_port = 0;
  if (rtcp_port != NULL)
    *rtcp_port = 0;

  if (transport == NULL)
    return FALSE;

  tokens = g_strsplit(transport, ";", -1);
  if (tokens == NULL)
    return FALSE;

  for (guint i = 0; tokens[i] != NULL; i++) {
    if (g_ascii_strncasecmp(tokens[i], "client_port=", 12) == 0) {
      client_port = tokens[i] + 12;
      ok = TRUE;
      break;
    }
  }

  if (ok && client_port != NULL) {
    gchar **ports = g_strsplit(client_port, "-", 2);
    if (ports != NULL && ports[0] != NULL) {
      if (rtp_port != NULL)
        *rtp_port = (guint) g_ascii_strtoull(ports[0], NULL, 10);
      if (ports[1] != NULL && rtcp_port != NULL)
        *rtcp_port = (guint) g_ascii_strtoull(ports[1], NULL, 10);
      else if (rtcp_port != NULL)
        *rtcp_port = rtp_port != NULL ? *rtp_port + 1 : 0;
    }
    g_strfreev(ports);
  }

  g_strfreev(tokens);
  return ok;
}

static gboolean
gst_rtsp_sink_client_configure_udp(GstRTSPSinkClient *client)
{
  GSocketAddress *remote;
  GInetAddress *inet_address = NULL;
  GInetAddress *local_address = NULL;
  gchar *host = NULL;
  GSocketAddress *local = NULL;
  gboolean ok = FALSE;
  GSocketFamily family;

  if (client == NULL || client->connection == NULL)
    return FALSE;

  remote = g_socket_connection_get_remote_address(client->connection, NULL);
  if (remote == NULL || !G_IS_INET_SOCKET_ADDRESS(remote))
    return FALSE;

  family = g_socket_address_get_family(remote);
  inet_address = g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(remote));
  if (inet_address == NULL)
    return FALSE;

  host = g_inet_address_to_string(inet_address);
  if (host == NULL)
    return FALSE;

  g_clear_object(&client->udp_rtp_address);
  local_address = g_inet_address_new_from_string(host);
  if (local_address == NULL)
    goto done;

  client->udp_rtp_address =
      G_SOCKET_ADDRESS(g_inet_socket_address_new(local_address, client->client_rtp_port));
  if (client->udp_rtp_address == NULL)
    goto done;

  g_clear_object(&client->udp_socket);
  client->udp_socket = g_socket_new(family,
                                    G_SOCKET_TYPE_DATAGRAM,
                                    G_SOCKET_PROTOCOL_UDP,
                                    NULL);
  if (client->udp_socket == NULL)
    goto done;

  {
    GInetAddress *any = g_inet_address_new_any(family);
    local = G_SOCKET_ADDRESS(g_inet_socket_address_new(any, 0));
    g_clear_object(&any);
  }
  if (local != NULL)
    ok = g_socket_bind(client->udp_socket, local, TRUE, NULL);

done:
  g_clear_object(&local);
  g_clear_object(&local_address);
  g_clear_object(&remote);
  g_free(host);
  return ok;
}

static gboolean
gst_rtsp_sink_client_send_rtp_packet(GstRTSPSinkClient *client,
                                     const guint8 *payload,
                                     gsize payload_size,
                                     guint8 payload_type,
                                     gboolean marker,
                                     guint16 seqnum,
                                     guint32 timestamp)
{
  guint8 header[12];
  guint8 *packet = NULL;
  gsize packet_size;
  gboolean ok = FALSE;

  if (client == NULL || payload == NULL)
    return FALSE;

  header[0] = 0x80;
  header[1] = (marker ? 0x80 : 0x00) | (payload_type & 0x7f);
  header[2] = (guint8) (seqnum >> 8);
  header[3] = (guint8) (seqnum & 0xff);
  header[4] = (guint8) (timestamp >> 24);
  header[5] = (guint8) ((timestamp >> 16) & 0xff);
  header[6] = (guint8) ((timestamp >> 8) & 0xff);
  header[7] = (guint8) (timestamp & 0xff);
  header[8] = (guint8) (client->ssrc >> 24);
  header[9] = (guint8) ((client->ssrc >> 16) & 0xff);
  header[10] = (guint8) ((client->ssrc >> 8) & 0xff);
  header[11] = (guint8) (client->ssrc & 0xff);

  if (client->use_tcp) {
    guint8 frame_header[4];
    guint16 frame_size = (guint16) (sizeof(header) + payload_size);

    frame_header[0] = 0x24;
    frame_header[1] = 0x00;
    frame_header[2] = (guint8) (frame_size >> 8);
    frame_header[3] = (guint8) (frame_size & 0xff);

    packet_size = sizeof(frame_header) + sizeof(header) + payload_size;
    packet = g_malloc(packet_size);
    memcpy(packet, frame_header, sizeof(frame_header));
    memcpy(packet + sizeof(frame_header), header, sizeof(header));
    memcpy(packet + sizeof(frame_header) + sizeof(header), payload, payload_size);
    ok = gst_rtsp_sink_client_write_all(client, packet, packet_size);
    g_free(packet);
    return ok;
  }

  if (client->udp_socket == NULL || client->udp_rtp_address == NULL)
    return FALSE;

  packet_size = sizeof(header) + payload_size;
  packet = g_malloc(packet_size);
  memcpy(packet, header, sizeof(header));
  memcpy(packet + sizeof(header), payload, payload_size);
  ok = g_socket_send_to(client->udp_socket,
                        client->udp_rtp_address,
                        (const gchar *) packet,
                        packet_size,
                        NULL,
                        NULL) >= 0;
  g_free(packet);
  return ok;
}

typedef struct
{
  GstRTSPSinkClient *client;
  guint8 payload_type;
  guint mtu;
  guint32 timestamp;
  gboolean marker_last_packet;
  guint16 *seqnum;
  gboolean h265;
} GstRTSPSinkPacketizeContext;

static gboolean
gst_rtsp_sink_client_send_fragmented(GstRTSPSinkPacketizeContext *ctx,
                                     const guint8 *nal,
                                     gsize nal_size,
                                     gboolean last_nal)
{
  const guint8 *payload = nal;
  gsize payload_size = nal_size;
  gsize header_size;
  gsize max_payload;
  guint8 nal_type;
  guint8 fu_indicator[2];
  gboolean use_fragmentation = FALSE;
  gboolean is_h265 = ctx->h265;

  if (is_h265) {
    nal_type = (guint8) ((nal[0] >> 1) & 0x3f);
    header_size = 3;
    max_payload = ctx->mtu > 12 + header_size ? ctx->mtu - 12 - header_size : 0;
    if (max_payload == 0)
      return FALSE;
    use_fragmentation = nal_size > max_payload;
    if (!use_fragmentation)
      return gst_rtsp_sink_client_send_rtp_packet(ctx->client,
                                                  payload,
                                                  payload_size,
                                                  ctx->payload_type,
                                                  last_nal && ctx->marker_last_packet,
                                                  (*ctx->seqnum)++,
                                                  ctx->timestamp);

    fu_indicator[0] = (nal[0] & 0x81) | (49 << 1);
    fu_indicator[1] = nal[1];
    max_payload = ctx->mtu > 15 ? ctx->mtu - 15 : 0;

    for (gsize offset = 2; offset < nal_size; ) {
      gsize chunk = MIN(max_payload, nal_size - offset);
      guint8 *fu_payload;
      gboolean start = (offset == 2);
      gboolean end = (offset + chunk >= nal_size);

      fu_payload = g_malloc(3 + chunk);
      fu_payload[0] = fu_indicator[0];
      fu_payload[1] = fu_indicator[1];
      fu_payload[2] = (guint8) ((start ? 0x80 : 0x00) | (end ? 0x40 : 0x00) | nal_type);
      memcpy(fu_payload + 3, nal + offset, chunk);

      if (!gst_rtsp_sink_client_send_rtp_packet(ctx->client,
                                                fu_payload,
                                                3 + chunk,
                                                ctx->payload_type,
                                                end && last_nal && ctx->marker_last_packet,
                                                (*ctx->seqnum)++,
                                                ctx->timestamp))
      {
        g_free(fu_payload);
        return FALSE;
      }

      g_free(fu_payload);
      offset += chunk;
    }

    return TRUE;
  }

  nal_type = nal[0] & 0x1f;
  header_size = 1;
  max_payload = ctx->mtu > 12 + header_size ? ctx->mtu - 12 - header_size : 0;
  if (max_payload == 0)
    return FALSE;
  use_fragmentation = nal_size > max_payload;

  if (!use_fragmentation) {
    return gst_rtsp_sink_client_send_rtp_packet(ctx->client,
                                                payload,
                                                payload_size,
                                                ctx->payload_type,
                                                last_nal && ctx->marker_last_packet,
                                                (*ctx->seqnum)++,
                                                ctx->timestamp);
  }

  {
    guint8 fu_indicator = (nal[0] & 0xe0) | 28;
    guint8 fu_header_base = nal_type;

    for (gsize offset = 1; offset < nal_size; ) {
      gsize chunk = MIN(max_payload, nal_size - offset);
      guint8 *fu_payload;
      gboolean start = (offset == 1);
      gboolean end = (offset + chunk >= nal_size);

      fu_payload = g_malloc(2 + chunk);
      fu_payload[0] = fu_indicator;
      fu_payload[1] = (guint8) ((start ? 0x80 : 0x00) | (end ? 0x40 : 0x00) | fu_header_base);
      memcpy(fu_payload + 2, nal + offset, chunk);

      if (!gst_rtsp_sink_client_send_rtp_packet(ctx->client,
                                                fu_payload,
                                                2 + chunk,
                                                ctx->payload_type,
                                                end && last_nal && ctx->marker_last_packet,
                                                (*ctx->seqnum)++,
                                                ctx->timestamp))
      {
        g_free(fu_payload);
        return FALSE;
      }

      g_free(fu_payload);
      offset += chunk;
    }
  }

  return TRUE;
}

static gboolean
gst_rtsp_sink_client_parse_nals(const guint8 *data,
                                gsize size,
                                const gchar *stream_format,
                                gboolean (*callback)(const guint8 *, gsize, gboolean, gpointer),
                                gpointer user_data)
{
  if (data == NULL || size == 0 || callback == NULL)
    return FALSE;

  if (g_strcmp0(stream_format, "avc") == 0 || g_strcmp0(stream_format, "hvc1") == 0) {
    gsize pos = 0;
    while (pos + 4 <= size) {
      guint32 nal_size =
          ((guint32) data[pos] << 24) |
          ((guint32) data[pos + 1] << 16) |
          ((guint32) data[pos + 2] << 8) |
          (guint32) data[pos + 3];
      pos += 4;
      if (nal_size == 0 || pos + nal_size > size)
        return FALSE;
      if (!callback(data + pos, nal_size, pos + nal_size >= size, user_data))
        return FALSE;
      pos += nal_size;
    }
    return TRUE;
  }

  {
    gsize pos = 0;
    gboolean found = FALSE;

    while (pos + 3 < size) {
      gsize start = G_MAXSIZE;
      gsize next = G_MAXSIZE;

      for (gsize i = pos; i + 3 < size; i++) {
        if (data[i] == 0x00 && data[i + 1] == 0x00 &&
            (data[i + 2] == 0x01 ||
             (i + 3 < size && data[i + 2] == 0x00 && data[i + 3] == 0x01))) {
          start = (data[i + 2] == 0x01) ? i + 3 : i + 4;
          break;
        }
      }

      if (start == G_MAXSIZE)
        break;

      for (gsize i = start; i + 3 < size; i++) {
        if (data[i] == 0x00 && data[i + 1] == 0x00 &&
            (data[i + 2] == 0x01 ||
             (i + 3 < size && data[i + 2] == 0x00 && data[i + 3] == 0x01))) {
          next = i;
          break;
        }
      }

      if (next == G_MAXSIZE)
        next = size;

      if (start >= next || start >= size)
        break;

      found = TRUE;
      if (!callback(data + start, next - start, next >= size, user_data))
        return FALSE;
      pos = next;
    }

    return found;
  }
}

static gboolean
gst_rtsp_sink_client_send_nal_cb(const guint8 *nal,
                                 gsize nal_size,
                                 gboolean last_nal,
                                 gpointer user_data)
{
  GstRTSPSinkPacketizeContext *ctx = user_data;
  return gst_rtsp_sink_client_send_fragmented(ctx, nal, nal_size, last_nal);
}

static guint32
gst_rtsp_sink_client_timestamp_for_buffer(GstRTSPSinkClient *client,
                                          const GstBuffer *buffer,
                                          const GstRTSPSinkRtpConfig *rtp)
{
  GstClockTime pts = GST_CLOCK_TIME_NONE;

  if (GST_BUFFER_PTS_IS_VALID(buffer))
    pts = GST_BUFFER_PTS(buffer);
  else if (GST_BUFFER_DTS_IS_VALID(buffer))
    pts = GST_BUFFER_DTS(buffer);

  if (pts == GST_CLOCK_TIME_NONE)
    return client->timestamp_base;

  if (!client->have_reference_pts) {
    client->reference_pts = pts;
    client->have_reference_pts = TRUE;
  }

  if (pts < client->reference_pts)
    return client->timestamp_base;

  return client->timestamp_base +
         (guint32) (((pts - client->reference_pts) * rtp->clock_rate) / GST_SECOND);
}

static gboolean
gst_rtsp_sink_client_send_buffer_internal(GstRTSPSinkClient *client,
                                          const GstBuffer *buffer)
{
  GstMapInfo map;
  GstRTSPSinkRtpConfig const *rtp;
  GstRTSPSinkServerConfig const *config;
  GstRTSPSinkPacketizeContext ctx;
  GstRTSPSinkFrameInfo frame_info = { 0 };
  gboolean ok;
  gboolean marker_last_packet = TRUE;

  if (client == NULL || buffer == NULL || !gst_rtsp_sink_client_is_playing(client))
    return TRUE;

  config = gst_rtsp_sink_server_get_config(client->server);
  rtp = gst_rtsp_sink_server_get_rtp_config(client->server);
  if (config == NULL || rtp == NULL)
    return FALSE;

  if (!gst_buffer_map((GstBuffer *) buffer, &map, GST_MAP_READ))
    return FALSE;

  if (!gst_rtsp_sink_describe_buffer(buffer, &frame_info))
    frame_info.is_access_unit = TRUE;

  ctx.client = client;
  ctx.payload_type = (guint8) rtp->payload_type;
  ctx.mtu = rtp->mtu;
  ctx.timestamp = gst_rtsp_sink_client_timestamp_for_buffer(client, buffer, rtp);
  ctx.marker_last_packet = marker_last_packet && rtp->marker_per_access_unit && frame_info.is_access_unit;
  ctx.seqnum = &client->next_seqnum;
  ctx.h265 = config->codec == GST_RTSP_SINK_CODEC_H265;

  ok = gst_rtsp_sink_client_parse_nals(map.data,
                                       map.size,
                                       config->stream_format,
                                       gst_rtsp_sink_client_send_nal_cb,
                                       &ctx);

  gst_buffer_unmap((GstBuffer *) buffer, &map);
  return ok;
}

static gchar *
gst_rtsp_sink_client_extract_message(GString *incoming)
{
  gchar *header_end;
  gsize header_len;
  guint content_length;
  gsize total_len;

  if (incoming == NULL || incoming->len == 0)
    return NULL;

  header_end = g_strstr_len(incoming->str, incoming->len, "\r\n\r\n");
  if (header_end == NULL)
    return NULL;

  header_len = (gsize) (header_end - incoming->str) + 4;
  content_length = gst_rtsp_sink_client_header_content_length(incoming->str);
  total_len = header_len + content_length;
  if (incoming->len < total_len)
    return NULL;

  gchar *message = gst_rtsp_sink_client_duplicate_message(incoming->str, total_len);
  g_string_erase(incoming, 0, total_len);
  return message;
}

static gboolean
gst_rtsp_sink_client_setup_transport(GstRTSPSinkClient *client, const GstRTSPSinkRequest *request)
{
  gboolean use_tcp = request != NULL && request->transport != NULL &&
                     (g_strrstr(request->transport, "TCP") != NULL ||
                      g_strrstr(request->transport, "interleaved") != NULL);
  guint client_rtp = 0;
  guint client_rtcp = 0;

  client->use_tcp = use_tcp;
  if (use_tcp) {
    client->client_rtp_port = 0;
    client->client_rtcp_port = 0;
    return TRUE;
  }

  if (!gst_rtsp_sink_client_parse_client_ports(request->transport, &client_rtp, &client_rtcp))
    return FALSE;

  client->client_rtp_port = client_rtp;
  client->client_rtcp_port = client_rtcp;
  return gst_rtsp_sink_client_configure_udp(client);
}

static void
gst_rtsp_sink_client_handle_request(GstRTSPSinkClient *client,
                                    GstRTSPSinkRequest *request)
{
  gchar *response;
  gchar *response_copy;
  guint cseq = (guint) g_ascii_strtoull(request != NULL ? request->cseq : "0", NULL, 10);

  if (g_strcmp0(request->method, "SETUP") == 0) {
    if (!gst_rtsp_sink_client_setup_transport(client, request)) {
      response = gst_rtsp_sink_client_error_response(461, cseq);
      response_copy = g_strdup(response);
      gst_rtsp_sink_client_write_all(client, (const guint8 *) response_copy, strlen(response_copy));
      g_free(response_copy);
      g_free(response);
      client->closed = TRUE;
      return;
    }
  }

  response = gst_rtsp_sink_handle_request(client->server, client, request);
  if (response == NULL)
    return;

  response_copy = g_strdup(response);
  gst_rtsp_sink_client_write_all(client, (const guint8 *) response_copy, strlen(response_copy));
  g_free(response_copy);

  if (!gst_rtsp_sink_client_response_ok(response)) {
    g_free(response);
    return;
  }

  if (g_strcmp0(request->method, "SETUP") == 0) {
    g_free(client->session_id);
    client->session_id = g_strdup(gst_rtsp_sink_server_get_session_id(client->server));
    gst_rtsp_sink_client_set_state(client, GST_RTSP_CLIENT_STATE_READY);
  } else if (g_strcmp0(request->method, "PLAY") == 0) {
    gst_rtsp_sink_client_set_state(client, GST_RTSP_CLIENT_STATE_PLAYING);
    gst_rtsp_sink_client_send_cached_start(client);
  } else if (g_strcmp0(request->method, "PAUSE") == 0) {
    gst_rtsp_sink_client_set_state(client, GST_RTSP_CLIENT_STATE_PAUSED);
  } else if (g_strcmp0(request->method, "TEARDOWN") == 0) {
    gst_rtsp_sink_client_set_state(client, GST_RTSP_CLIENT_STATE_CLOSED);
    client->closed = TRUE;
  }

  g_free(response);
}

static gpointer
gst_rtsp_sink_client_thread_main(gpointer data)
{
  GstRTSPSinkClient *client = data;
  GString *incoming = g_string_new(NULL);
  guint8 buffer[4096];

  while (!client->closed) {
    gssize nread = g_input_stream_read(client->input, buffer, sizeof(buffer), NULL, NULL);
    if (nread <= 0)
      break;

    client->last_activity_us = g_get_monotonic_time();
    g_string_append_len(incoming, (const gchar *) buffer, (gsize) nread);

    while (TRUE) {
      gchar *message = gst_rtsp_sink_client_extract_message(incoming);
      if (message == NULL)
        break;

      GstRTSPSinkRequest request = { 0 };
      if (gst_rtsp_sink_parse_request(message, &request)) {
        if (request.body == NULL) {
          gchar *header_end = g_strstr_len(message, -1, "\r\n\r\n");
          if (header_end != NULL && header_end[4] != '\0')
            request.body = g_strdup(header_end + 4);
        }
        gst_rtsp_sink_client_handle_request(client, &request);
      }

      gst_rtsp_sink_request_clear(&request);
      g_free(message);

      if (client->closed)
        break;
    }
  }

  gst_rtsp_sink_client_stop(client);
  g_string_free(incoming, TRUE);
  gst_rtsp_sink_server_remove_client(client->server, client);
  gst_rtsp_sink_client_free(client);
  return NULL;
}

GstRTSPSinkClient *
gst_rtsp_sink_client_new(GstRTSPSinkServer *server, GSocketConnection *connection)
{
  GstRTSPSinkClient *client;

  g_return_val_if_fail(server != NULL, NULL);
  g_return_val_if_fail(connection != NULL, NULL);

  client = g_new0(GstRTSPSinkClient, 1);
  client->server = server;
  client->connection = g_object_ref(connection);
  client->input = g_io_stream_get_input_stream(G_IO_STREAM(connection));
  client->output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
  client->state = GST_RTSP_CLIENT_STATE_IDLE;
  client->timestamp_base = gst_rtsp_sink_server_get_rtp_config(server)->timestamp_base;
  client->next_seqnum = gst_rtsp_sink_server_get_rtp_config(server)->sequence_number_base;
  client->ssrc = gst_rtsp_sink_server_get_rtp_config(server)->ssrc != 0 ?
                 gst_rtsp_sink_server_get_rtp_config(server)->ssrc :
                 (guint32) g_random_int();
  client->session_id = g_strdup_printf("%08x", g_random_int());
  client->ref_count = 1;
  client->last_activity_us = g_get_monotonic_time();
  g_mutex_init(&client->write_lock);

  return client;
}

GstRTSPSinkClient *
gst_rtsp_sink_client_ref(GstRTSPSinkClient *client)
{
  if (client != NULL)
    g_atomic_int_inc(&client->ref_count);
  return client;
}

void
gst_rtsp_sink_client_stop(GstRTSPSinkClient *client)
{
  if (client == NULL || client->closed)
    return;

  client->closed = TRUE;
  if (client->connection != NULL)
    g_io_stream_close(G_IO_STREAM(client->connection), NULL, NULL);
  g_clear_object(&client->udp_rtp_address);
  g_clear_object(&client->udp_socket);
}

void
gst_rtsp_sink_client_free(GstRTSPSinkClient *client)
{
  if (client == NULL)
    return;

  if (!g_atomic_int_dec_and_test(&client->ref_count))
    return;

  gst_rtsp_sink_client_stop(client);
  g_clear_object(&client->connection);
  g_clear_object(&client->udp_rtp_address);
  g_clear_object(&client->udp_socket);
  g_free(client->session_id);
  g_mutex_clear(&client->write_lock);
  g_free(client);
}

gpointer
gst_rtsp_sink_client_run(gpointer data)
{
  GstRTSPSinkClient *client = data;

  g_return_val_if_fail(client != NULL, NULL);

  gst_rtsp_sink_client_thread_main(client);
  return NULL;
}

gboolean
gst_rtsp_sink_client_send_response(GstRTSPSinkClient *client, const gchar *response)
{
  g_return_val_if_fail(client != NULL, FALSE);
  return gst_rtsp_sink_client_write_all(client, (const guint8 *) response, strlen(response));
}

gboolean
gst_rtsp_sink_client_send_cached_start(GstRTSPSinkClient *client)
{
  GstBuffer *parameter_sets = NULL;
  GstBuffer *keyframe = NULL;
  gboolean ok = TRUE;

  if (client == NULL)
    return FALSE;

  if (!gst_rtsp_sink_server_get_warm_start_buffers(client->server, &parameter_sets, &keyframe))
    return TRUE;

  if (parameter_sets != NULL) {
    ok = gst_rtsp_sink_client_send_buffer(client, parameter_sets);
    gst_buffer_unref(parameter_sets);
  }

  if (ok && keyframe != NULL) {
    ok = gst_rtsp_sink_client_send_buffer(client, keyframe);
    gst_buffer_unref(keyframe);
  } else if (keyframe != NULL) {
    gst_buffer_unref(keyframe);
  }

  return ok;
}

gboolean
gst_rtsp_sink_client_send_buffer(GstRTSPSinkClient *client, const GstBuffer *buffer)
{
  return gst_rtsp_sink_client_send_buffer_internal(client, buffer);
}

gboolean
gst_rtsp_sink_client_is_playing(const GstRTSPSinkClient *client)
{
  return client != NULL && client->state == GST_RTSP_CLIENT_STATE_PLAYING && !client->closed;
}

void
gst_rtsp_sink_client_set_state(GstRTSPSinkClient *client, GstRTSPSinkClientState state)
{
  if (client != NULL)
    client->state = state;
}

GstRTSPSinkClientState
gst_rtsp_sink_client_get_state(const GstRTSPSinkClient *client)
{
  return client != NULL ? client->state : GST_RTSP_CLIENT_STATE_CLOSED;
}

const gchar *
gst_rtsp_sink_client_get_session_id(const GstRTSPSinkClient *client)
{
  return client != NULL ? client->session_id : NULL;
}

guint
gst_rtsp_sink_client_get_udp_server_port(const GstRTSPSinkClient *client)
{
  GSocketAddress *local = NULL;
  guint port = 0;

  if (client == NULL || client->udp_socket == NULL)
    return 0;

  local = g_socket_get_local_address(client->udp_socket, NULL);
  if (local != NULL && G_IS_INET_SOCKET_ADDRESS(local))
    port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(local));
  g_clear_object(&local);
  return port;
}

void
gst_rtsp_sink_client_touch(GstRTSPSinkClient *client)
{
  if (client == NULL)
    return;

  client->last_activity_us = g_get_monotonic_time();
}
