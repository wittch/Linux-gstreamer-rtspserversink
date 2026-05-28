#include "rtsp-server-internal.h"

static gchar *
build_date_header (void)
{
  GDateTime *now = g_date_time_new_now_utc ();
  gchar *formatted = g_date_time_format (now, "%a, %d %b %Y %H:%M:%S GMT");
  gchar *header = g_strdup_printf ("Date: %s\r\n", formatted);

  g_free (formatted);
  g_date_time_unref (now);
  return header;
}

static gchar *
build_basic_response (guint code, const gchar *reason, const gchar *cseq,
    const gchar *extra_headers, const gchar *body)
{
  gchar *date_header = build_date_header ();
  gchar *response;
  gsize body_len = body != NULL ? strlen (body) : 0;

  response = g_strdup_printf (
      "RTSP/1.0 %u %s\r\n"
      "CSeq: %s\r\n"
      "%s"
      "%s"
      "Content-Length: %u\r\n"
      "\r\n"
      "%s",
      code, reason, cseq != NULL ? cseq : "0",
      date_header,
      extra_headers != NULL ? extra_headers : "",
      (guint) body_len, body != NULL ? body : "");
  g_free (date_header);

  return response;
}

static gchar *
make_session_id (GstRTSPSinkServer *server)
{
  return g_strdup_printf ("%u", ++server->next_session_id);
}

static gboolean
accepts_sdp (const GstRTSPSinkRequest *request)
{
  const gchar *accept = gst_rtsp_sink_request_get_header (request, "accept");

  if (accept == NULL)
    return TRUE;

  return strstr (accept, "application/sdp") != NULL ||
      strstr (accept, "*/*") != NULL;
}

static gboolean
require_session (GstRTSPSinkClient *client, const GstRTSPSinkRequest *request)
{
  g_autofree gchar *session_id = NULL;

  session_id = gst_rtsp_sink_parse_session_header (
      gst_rtsp_sink_request_get_header (request, "session"));
  if (session_id == NULL || client->session_id == NULL)
    return FALSE;

  return g_strcmp0 (session_id, client->session_id) == 0;
}

static gboolean
server_has_other_playing_clients_unlocked (GstRTSPSinkServer *server,
    GstRTSPSinkClient *self)
{
  GList *iter;

  for (iter = server->clients; iter != NULL; iter = iter->next) {
    GstRTSPSinkClient *client = iter->data;

    if (client == self)
      continue;
    if (!client->closed && client->state == GST_RTSP_SINK_STATE_PLAYING)
      return TRUE;
  }

  return FALSE;
}

static gchar *
build_response_base_url (GstRTSPSinkServer *server,
    const GstRTSPSinkRequest *request)
{
  const gchar *host = gst_rtsp_sink_request_get_header (request, "host");
  const gchar *scheme;
  const gchar *path;

  if (request->uri != NULL && g_str_has_prefix (request->uri, "rtsp://")) {
    path = strchr (request->uri + strlen ("rtsp://"), '/');
    if (path != NULL)
      return g_strndup (request->uri, path - request->uri);
    return g_strdup (request->uri);
  }

  scheme = "rtsp://";
  if (host != NULL && host[0] != '\0')
    return g_strdup_printf ("%s%s", scheme, host);

  return g_strdup_printf ("%s%s:%u", scheme, server->address, server->port);
}

gchar *
gst_rtsp_sink_build_rtsp_url (GstRTSPSinkServer *server)
{
  return g_strdup_printf ("rtsp://%s:%u%s", server->address, server->port,
      server->path);
}

gchar *
gst_rtsp_sink_handle_request (GstRTSPSinkClient *client,
    const GstRTSPSinkRequest *request)
{
  GstRTSPSinkServer *server = client->server;
  gchar *path = gst_rtsp_sink_extract_path (request->uri);
  const gchar *cseq = gst_rtsp_sink_request_get_header (request, "cseq");
  gchar *response = NULL;
  gchar *url = NULL;
  gchar *base_url = NULL;
  guint parsed_cseq = 0;

  (void) parsed_cseq;

  if (!gst_rtsp_sink_request_get_cseq (request, &parsed_cseq)) {
    response = build_basic_response (400, "Bad Request", cseq, NULL, NULL);
    goto done;
  }
  if (g_strcmp0 (request->version, "RTSP/1.0") != 0) {
    response = build_basic_response (505, "RTSP Version not supported", cseq,
        NULL, NULL);
    goto done;
  }

  g_mutex_lock (&server->lock);
  if (!gst_rtsp_sink_path_matches (path, server->path)) {
    g_mutex_unlock (&server->lock);
    response = build_basic_response (404, "Not Found", cseq, NULL, NULL);
    goto done;
  }

  base_url = build_response_base_url (server, request);
  url = g_strdup_printf ("%s%s", base_url, server->path);
  gst_rtsp_sink_client_touch_keepalive (client);

  if (g_ascii_strcasecmp (request->method, "OPTIONS") == 0) {
    response = build_basic_response (200, "OK", cseq,
        "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, GET_PARAMETER\r\n",
        NULL);
  } else if (g_ascii_strcasecmp (request->method, "DESCRIBE") == 0) {
    gchar *extra_headers;

    if (!accepts_sdp (request)) {
      g_mutex_unlock (&server->lock);
      response = build_basic_response (406, "Not Acceptable", cseq, NULL, NULL);
      goto done;
    }

    extra_headers = g_strdup_printf (
        "Content-Type: application/sdp\r\n"
        "Content-Base: %s/\r\n",
        url);
    GST_DEBUG ("serving DESCRIBE SDP: media=%s encoding-name=%s fmtp=%s",
        GST_STR_NULL (server->rtp_media),
        GST_STR_NULL (server->rtp_encoding_name),
        GST_STR_NULL (server->rtp_fmtp));
    response = build_basic_response (200, "OK", cseq, extra_headers,
        server->sdp != NULL ? server->sdp : "");
    g_free (extra_headers);
  } else if (g_ascii_strcasecmp (request->method, "SETUP") == 0) {
    const gchar *transport = gst_rtsp_sink_request_get_header (request,
        "transport");
    TransportSetup setup;

    if (client->state == GST_RTSP_SINK_STATE_PLAYING) {
      g_mutex_unlock (&server->lock);
      response = build_basic_response (455, "Method Not Valid in This State",
          cseq, NULL, NULL);
      goto done;
    }
    if (!gst_rtsp_sink_parse_transport_header (transport, server, &setup)) {
      g_mutex_unlock (&server->lock);
      response = build_basic_response (461, "Unsupported Transport", cseq,
          NULL, NULL);
      goto done;
    }

    if (client->session_id == NULL)
      client->session_id = make_session_id (server);

    gst_rtsp_sink_client_reset_transport (client);
    client->transport = setup.transport;
    client->seqnum = g_random_int_range (0, G_MAXUINT16);
    if (client->ssrc != server->rtp_ssrc) {
      g_clear_pointer (&client->rtcp_cname, g_free);
      client->ssrc = server->rtp_ssrc;
      client->rtcp_cname = g_strdup_printf ("%08x@%s", client->ssrc,
          server->address != NULL ? server->address : "localhost");
    }

    if (setup.transport == GST_RTSP_SINK_TRANSPORT_TCP_INTERLEAVED) {
      gchar *extra_headers;

      client->state = GST_RTSP_SINK_STATE_READY;
      client->rtp_channel = setup.rtp_channel;
      client->rtcp_channel = setup.rtcp_channel;
      if (server->have_rtp_ssrc) {
        extra_headers = g_strdup_printf (
            "Transport: RTP/AVP/TCP;unicast;interleaved=%u-%u;ssrc=%08X;mode=play\r\n"
            "Session: %s\r\n",
            client->rtp_channel, client->rtcp_channel, client->ssrc,
            client->session_id);
      } else {
        extra_headers = g_strdup_printf (
            "Transport: RTP/AVP/TCP;unicast;interleaved=%u-%u;mode=play\r\n"
            "Session: %s\r\n",
            client->rtp_channel, client->rtcp_channel, client->session_id);
      }
      response = build_basic_response (200, "OK", cseq, extra_headers, NULL);
      g_free (extra_headers);
    } else {
      gchar *extra_headers;
      GError *udp_error = NULL;

      client->client_rtp_port = setup.client_rtp_port;
      client->client_rtcp_port = setup.client_rtcp_port;
      client->client_rtp_addr = gst_rtsp_sink_client_remote_address_with_port (
          client, client->client_rtp_port);
      client->client_rtcp_addr = gst_rtsp_sink_client_remote_address_with_port (
          client, client->client_rtcp_port);
      if (client->client_rtp_addr == NULL || client->client_rtcp_addr == NULL ||
          !gst_rtsp_sink_bind_udp_socket_pair (client->client_rtp_addr,
              &client->server_rtp_socket, &client->server_rtcp_socket,
              &setup.client_rtp_port, &setup.client_rtcp_port, &udp_error)) {
        g_mutex_unlock (&server->lock);
        g_clear_error (&udp_error);
        response = build_basic_response (461, "Unsupported Transport", cseq,
            NULL, NULL);
        goto done;
      }

      client->state = GST_RTSP_SINK_STATE_READY;
      if (server->have_rtp_ssrc) {
        extra_headers = g_strdup_printf (
            "Transport: RTP/AVP;unicast;client_port=%u-%u;server_port=%u-%u;ssrc=%08X;mode=play\r\n"
            "Session: %s\r\n",
            client->client_rtp_port, client->client_rtcp_port,
            setup.client_rtp_port, setup.client_rtcp_port, client->ssrc,
            client->session_id);
      } else {
        extra_headers = g_strdup_printf (
            "Transport: RTP/AVP;unicast;client_port=%u-%u;server_port=%u-%u;mode=play\r\n"
            "Session: %s\r\n",
            client->client_rtp_port, client->client_rtcp_port,
            setup.client_rtp_port, setup.client_rtcp_port, client->session_id);
      }
      response = build_basic_response (200, "OK", cseq, extra_headers, NULL);
      g_free (extra_headers);
    }
    GST_DEBUG ("SETUP complete state=%s transport=%s session=%s",
        "READY",
        setup.transport == GST_RTSP_SINK_TRANSPORT_TCP_INTERLEAVED ? "tcp" : "udp",
        GST_STR_NULL (client->session_id));
  } else if (g_ascii_strcasecmp (request->method, "PLAY") == 0) {
    if (!require_session (client, request)) {
      g_mutex_unlock (&server->lock);
      response = build_basic_response (454, "Session Not Found", cseq, NULL,
          NULL);
      goto done;
    }
    if (client->state != GST_RTSP_SINK_STATE_READY &&
        client->state != GST_RTSP_SINK_STATE_PAUSED) {
      g_mutex_unlock (&server->lock);
      response = build_basic_response (455, "Method Not Valid in This State",
          cseq, NULL, NULL);
      goto done;
    }

    client->state = GST_RTSP_SINK_STATE_PLAYING;
    GST_DEBUG ("PLAY entered state=%s session=%s queue_len=%d",
        "PLAYING", GST_STR_NULL (client->session_id),
        server->rtp_queue != NULL ? g_async_queue_length (server->rtp_queue) : -1);
    if (server->have_rtp_ssrc && client->ssrc != server->rtp_ssrc) {
      g_clear_pointer (&client->rtcp_cname, g_free);
      client->ssrc = server->rtp_ssrc;
      client->rtcp_cname = g_strdup_printf ("%08x@%s", client->ssrc,
          server->address != NULL ? server->address : "localhost");
    }

    if (!server_has_other_playing_clients_unlocked (server, client)) {
      GST_DEBUG ("PLAY is first active client, flushing stale queued RTP packets");
      gst_rtsp_sink_server_flush_pending_rtp_unlocked (server);
    }
    response = build_basic_response (200, "OK", cseq, NULL, NULL);
    gst_rtsp_sink_client_maybe_send_rtcp (client, TRUE, FALSE);
  } else if (g_ascii_strcasecmp (request->method, "PAUSE") == 0) {
    gchar *extra_headers;

    if (!require_session (client, request)) {
      g_mutex_unlock (&server->lock);
      response = build_basic_response (454, "Session Not Found", cseq, NULL,
          NULL);
      goto done;
    }
    if (client->state != GST_RTSP_SINK_STATE_PLAYING) {
      g_mutex_unlock (&server->lock);
      response = build_basic_response (455, "Method Not Valid in This State",
          cseq, NULL, NULL);
      goto done;
    }

    client->state = GST_RTSP_SINK_STATE_PAUSED;
    extra_headers = g_strdup_printf ("Session: %s\r\n", client->session_id);
    response = build_basic_response (200, "OK", cseq, extra_headers, NULL);
    g_free (extra_headers);
  } else if (g_ascii_strcasecmp (request->method, "TEARDOWN") == 0) {
    gchar *extra_headers = NULL;

    if (!require_session (client, request)) {
      g_mutex_unlock (&server->lock);
      response = build_basic_response (454, "Session Not Found", cseq, NULL,
          NULL);
      goto done;
    }
    if (client->state != GST_RTSP_SINK_STATE_READY &&
        client->state != GST_RTSP_SINK_STATE_PLAYING &&
        client->state != GST_RTSP_SINK_STATE_PAUSED) {
      g_mutex_unlock (&server->lock);
      response = build_basic_response (455, "Method Not Valid in This State",
          cseq, NULL, NULL);
      goto done;
    }

    extra_headers = g_strdup_printf ("Session: %s\r\n", client->session_id);
    response = build_basic_response (200, "OK", cseq, extra_headers, NULL);
    g_free (extra_headers);
    gst_rtsp_sink_client_maybe_send_rtcp (client, TRUE, TRUE);
    gst_rtsp_sink_client_reset_transport (client);
    client->rtcp_bye_sent = TRUE;
    client->state = GST_RTSP_SINK_STATE_CLOSED;
    client->closed = TRUE;
  } else if (g_ascii_strcasecmp (request->method, "GET_PARAMETER") == 0) {
    gchar *extra_headers;

    if (!require_session (client, request)) {
      g_mutex_unlock (&server->lock);
      response = build_basic_response (454, "Session Not Found", cseq, NULL,
          NULL);
      goto done;
    }
    if (client->state != GST_RTSP_SINK_STATE_READY &&
        client->state != GST_RTSP_SINK_STATE_PLAYING &&
        client->state != GST_RTSP_SINK_STATE_PAUSED) {
      g_mutex_unlock (&server->lock);
      response = build_basic_response (455, "Method Not Valid in This State",
          cseq, NULL, NULL);
      goto done;
    }

    extra_headers = g_strdup_printf ("Session: %s\r\n", client->session_id);
    response = build_basic_response (200, "OK", cseq, extra_headers, NULL);
    g_free (extra_headers);
  } else {
    response = build_basic_response (405, "Method Not Allowed", cseq, NULL,
        NULL);
  }

  g_mutex_unlock (&server->lock);

done:
  g_free (base_url);
  g_free (url);
  g_free (path);
  return response;
}
