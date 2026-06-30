#include "rtsp-server-internal.h"

#include <stdio.h>
#include <string.h>

static GBufferedInputStream *
client_get_buffered_input (GstRTSPSinkClient *client)
{
  GInputStream *base;

  base = g_filter_input_stream_get_base_stream (
      G_FILTER_INPUT_STREAM (client->input));
  return G_BUFFERED_INPUT_STREAM (base);
}

static gboolean
client_skip_interleaved_frames (GstRTSPSinkClient *client, GError **error)
{
  GBufferedInputStream *buffered = client_get_buffered_input (client);
  guint8 byte;

  while (g_buffered_input_stream_peek (buffered, &byte, 0, 1) > 0) {
    guint8 header[4];
    guint16 payload_len;
    gsize bytes_read;

    if (byte != '$')
      return TRUE;

    if (!g_input_stream_read_all (G_INPUT_STREAM (buffered), header,
            sizeof (header), &bytes_read, NULL, error)
        || bytes_read != sizeof (header))
      return FALSE;

    payload_len = (header[2] << 8) | header[3];
    if (payload_len > 0) {
      guint8 *payload = g_malloc (payload_len);

      if (!g_input_stream_read_all (G_INPUT_STREAM (buffered), payload,
              payload_len, &bytes_read, NULL, error)
          || bytes_read != payload_len) {
        g_free (payload);
        return FALSE;
      }
      g_free (payload);
    }
  }

  return TRUE;
}

gchar *
gst_rtsp_sink_extract_path (const gchar *uri)
{
  const gchar *path;
  const gchar *scheme;
  const gchar *query;
  gchar *result;

  if (uri == NULL)
    return NULL;

  scheme = strstr (uri, "://");
  if (scheme == NULL) {
    if (uri[0] != '/')
      return NULL;
    result = g_strdup (uri);
  } else {
    path = strchr (scheme + 3, '/');
    result = path != NULL ? g_strdup (path) : g_strdup ("/");
  }

  query = strchr (result, '?');
  if (query != NULL)
    result[query - result] = '\0';

  return result;
}

gboolean
gst_rtsp_sink_path_matches (const gchar *request_path,
    const gchar *expected_path)
{
  gsize len;

  if (request_path == NULL || expected_path == NULL)
    return FALSE;

  if (expected_path[0] == '\0' || g_strcmp0 (expected_path, "/") == 0)
    return request_path[0] == '/';
  if (g_strcmp0 (request_path, expected_path) == 0)
    return TRUE;

  len = strlen (expected_path);
  if (!g_str_has_prefix (request_path, expected_path))
    return FALSE;

  return request_path[len] == '/';
}

gboolean
gst_rtsp_sink_parse_request (GstRTSPSinkClient *client,
    GstRTSPSinkRequest *request)
{
  gchar *request_line = NULL;
  GError *error = NULL;
  guint content_length = 0;
  gsize request_line_len = 0;

  memset (request, 0, sizeof (*request));
  request->headers = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
      g_free);

  if (!client_skip_interleaved_frames (client, &error)) {
    g_clear_error (&error);
    return FALSE;
  }

  request_line = g_data_input_stream_read_line (client->input, &request_line_len, NULL,
      &error);
  if (request_line != NULL)
    g_strchomp (request_line);
  if (request_line == NULL) {
    g_clear_error (&error);
    g_free (request_line);
    return FALSE;
  }

  {
    gchar *sp1 = memchr (request_line, ' ', request_line_len);
    gchar *sp2 = NULL;

    if (sp1 != NULL)
      sp2 = memchr (sp1 + 1, ' ', request_line_len - ((sp1 + 1) - request_line));

    if (sp1 != NULL && sp2 != NULL && sp1 > request_line && sp2 > sp1 + 1 &&
        sp2[1] != '\0') {
      request->method = g_strndup (request_line, sp1 - request_line);
      request->uri = g_strndup (sp1 + 1, sp2 - (sp1 + 1));
      request->version = g_strdup (sp2 + 1);
    }
  }
  g_free (request_line);

  while (TRUE) {
    gchar *line;
    gchar *colon;

    if (!client_skip_interleaved_frames (client, &error)) {
      g_clear_error (&error);
      return FALSE;
    }

    line = g_data_input_stream_read_line (client->input, NULL, NULL, &error);
    if (line != NULL)
      g_strchomp (line);

    if (line == NULL) {
      g_clear_error (&error);
      return request->method != NULL && request->uri != NULL;
    }
    if (line[0] == '\0') {
      g_free (line);
      break;
    }

    colon = strchr (line, ':');
    if (colon != NULL) {
      gchar *name;
      gchar *value;

      *colon = '\0';
      name = g_ascii_strdown (line, -1);
      value = g_strdup (colon + 1);
      g_strstrip (value);
      g_hash_table_replace (request->headers, name, value);
    }
    g_free (line);
  }

  if (request->method == NULL || request->uri == NULL || request->version == NULL)
    return FALSE;

  {
    const gchar *content_length_text =
        gst_rtsp_sink_request_get_header (request, "content-length");

    if (content_length_text != NULL) {
      gchar *endptr = NULL;
      guint64 parsed = g_ascii_strtoull (content_length_text, &endptr, 10);

      if (endptr == content_length_text || (endptr != NULL && *endptr != '\0') ||
          parsed > G_MAXUINT)
        return FALSE;
      content_length = (guint) parsed;
    }
  }

  request->content_length = content_length;
  if (content_length > 0) {
    gsize bytes_read = 0;

    request->body = g_malloc0 (content_length + 1);
    if (!g_input_stream_read_all (G_INPUT_STREAM (client->input), request->body,
            content_length, &bytes_read, NULL, &error)
        || bytes_read != content_length) {
      g_clear_error (&error);
      return FALSE;
    }
  }

  return TRUE;
}

void
gst_rtsp_sink_request_clear (GstRTSPSinkRequest *request)
{
  g_clear_pointer (&request->method, g_free);
  g_clear_pointer (&request->uri, g_free);
  g_clear_pointer (&request->version, g_free);
  g_clear_pointer (&request->body, g_free);
  if (request->headers != NULL) {
    g_hash_table_unref (request->headers);
    request->headers = NULL;
  }
}

const gchar *
gst_rtsp_sink_request_get_header (const GstRTSPSinkRequest *request,
    const gchar *name)
{
  return g_hash_table_lookup (request->headers, name);
}

gboolean
gst_rtsp_sink_request_get_cseq (const GstRTSPSinkRequest *request, guint *cseq)
{
  const gchar *value = gst_rtsp_sink_request_get_header (request, "cseq");
  gchar *endptr = NULL;
  guint64 parsed;

  if (value == NULL)
    return FALSE;

  parsed = g_ascii_strtoull (value, &endptr, 10);
  if (endptr == value || (endptr != NULL && *endptr != '\0') || parsed > G_MAXUINT)
    return FALSE;

  *cseq = (guint) parsed;
  return TRUE;
}

static gboolean
parse_client_ports (const gchar *transport, guint16 *rtp_port,
    guint16 *rtcp_port)
{
  const gchar *token;
  guint port_a;
  guint port_b;

  token = strstr (transport, "client_port=");
  if (token == NULL)
    return FALSE;

  token += strlen ("client_port=");
  if (sscanf (token, "%u-%u", &port_a, &port_b) != 2)
    return FALSE;
  if (port_a == 0 || port_b == 0 || port_a > 65535 || port_b > 65535)
    return FALSE;

  *rtp_port = (guint16) port_a;
  *rtcp_port = (guint16) port_b;
  return TRUE;
}

static gboolean
parse_interleaved_channels (const gchar *transport, guint8 *rtp_channel,
    guint8 *rtcp_channel)
{
  const gchar *token;
  guint channel_a;
  guint channel_b;

  token = strstr (transport, "interleaved=");
  if (token == NULL) {
    *rtp_channel = 0;
    *rtcp_channel = 1;
    return TRUE;
  }

  token += strlen ("interleaved=");
  if (sscanf (token, "%u-%u", &channel_a, &channel_b) != 2)
    return FALSE;
  if (channel_a > 255 || channel_b > 255)
    return FALSE;

  *rtp_channel = (guint8) channel_a;
  *rtcp_channel = (guint8) channel_b;
  return TRUE;
}

gboolean
gst_rtsp_sink_parse_transport_header (const gchar *transport_header,
    GstRTSPSinkServer *server, TransportSetup *setup)
{
  gchar **tokens;
  guint i;
  gboolean saw_profile = FALSE;

  memset (setup, 0, sizeof (*setup));

  if (transport_header == NULL)
    return FALSE;
  setup->mode_play = TRUE;

  tokens = g_strsplit (transport_header, ";", -1);
  for (i = 0; tokens[i] != NULL; i++) {
    gchar *token = g_strstrip (tokens[i]);

    if (token[0] == '\0')
      continue;
    if (g_ascii_strcasecmp (token, "RTP/AVP/TCP") == 0) {
      if (!server->enable_tcp_interleaved)
        goto fail;
      saw_profile = TRUE;
      setup->transport = GST_RTSP_SINK_TRANSPORT_TCP_INTERLEAVED;
    } else if (g_ascii_strcasecmp (token, "RTP/AVP") == 0) {
      if (!server->enable_udp)
        goto fail;
      saw_profile = TRUE;
      setup->transport = GST_RTSP_SINK_TRANSPORT_UDP;
    } else if (g_ascii_strcasecmp (token, "unicast") == 0) {
      setup->is_unicast = TRUE;
    } else if (g_ascii_strncasecmp (token, "client_port=", 12) == 0) {
      if (!parse_client_ports (token, &setup->client_rtp_port,
              &setup->client_rtcp_port))
        goto fail;
    } else if (g_ascii_strncasecmp (token, "interleaved=", 12) == 0) {
      if (!parse_interleaved_channels (token, &setup->rtp_channel,
              &setup->rtcp_channel))
        goto fail;
    } else if (g_ascii_strncasecmp (token, "mode=", 5) == 0) {
      const gchar *mode = token + 5;

      if (g_ascii_strcasecmp (mode, "\"PLAY\"") != 0 &&
          g_ascii_strcasecmp (mode, "PLAY") != 0)
        goto fail;
      setup->mode_play = TRUE;
    } else {
      goto fail;
    }
  }

  g_strfreev (tokens);

  if (!saw_profile || !setup->is_unicast)
    return FALSE;
  if (setup->transport == GST_RTSP_SINK_TRANSPORT_TCP_INTERLEAVED)
    return TRUE;
  if (setup->transport == GST_RTSP_SINK_TRANSPORT_UDP &&
      setup->client_rtp_port > 0 && setup->client_rtcp_port > 0)
    return TRUE;
  return FALSE;

fail:
  g_strfreev (tokens);
  return FALSE;
}

gchar *
gst_rtsp_sink_parse_session_header (const gchar *session_header)
{
  gchar **parts;
  gchar *session_id;

  if (session_header == NULL)
    return NULL;

  parts = g_strsplit (session_header, ";", 2);
  session_id = g_strdup (g_strstrip (parts[0]));
  g_strfreev (parts);

  if (session_id[0] == '\0') {
    g_free (session_id);
    return NULL;
  }

  return session_id;
}

gboolean
gst_rtsp_sink_bind_udp_socket_pair (GSocketAddress *remote_address,
    GSocket **rtp_socket, GSocket **rtcp_socket, guint16 *rtp_port,
    guint16 *rtcp_port, GError **error)
{
  GSocketFamily family;
  GSocketAddress *bind_addr = NULL;
  GSocketAddress *local_addr = NULL;

  *rtp_socket = NULL;
  *rtcp_socket = NULL;
  *rtp_port = 0;
  *rtcp_port = 0;

  if (!G_IS_INET_SOCKET_ADDRESS (remote_address)) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
        "only IP UDP clients are supported");
    return FALSE;
  }

  family = g_socket_address_get_family (remote_address);
  *rtp_socket = g_socket_new (family, G_SOCKET_TYPE_DATAGRAM,
      G_SOCKET_PROTOCOL_UDP, error);
  if (*rtp_socket == NULL)
    goto fail;
  *rtcp_socket = g_socket_new (family, G_SOCKET_TYPE_DATAGRAM,
      G_SOCKET_PROTOCOL_UDP, error);
  if (*rtcp_socket == NULL)
    goto fail;

  {
    GInetAddress *any_addr = g_inet_address_new_any (family == G_SOCKET_FAMILY_IPV6 ?
        G_SOCKET_FAMILY_IPV6 : G_SOCKET_FAMILY_IPV4);

    bind_addr = g_inet_socket_address_new (any_addr, 0);
    g_object_unref (any_addr);
  }

  if (!g_socket_bind (*rtp_socket, bind_addr, TRUE, error))
    goto fail;
  if (!g_socket_bind (*rtcp_socket, bind_addr, TRUE, error))
    goto fail;

  local_addr = g_socket_get_local_address (*rtp_socket, error);
  if (local_addr == NULL)
    goto fail;
  *rtp_port = g_inet_socket_address_get_port (G_INET_SOCKET_ADDRESS (local_addr));
  g_clear_object (&local_addr);

  local_addr = g_socket_get_local_address (*rtcp_socket, error);
  if (local_addr == NULL)
    goto fail;
  *rtcp_port = g_inet_socket_address_get_port (G_INET_SOCKET_ADDRESS (local_addr));
  g_clear_object (&local_addr);
  g_clear_object (&bind_addr);

  return TRUE;

fail:
  g_clear_object (&local_addr);
  g_clear_object (&bind_addr);
  g_clear_object (rtp_socket);
  g_clear_object (rtcp_socket);
  return FALSE;
}
