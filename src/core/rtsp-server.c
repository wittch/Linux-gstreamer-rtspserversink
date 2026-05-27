#include "rtsp-server-internal.h"

void
gst_rtsp_sink_server_reset_codec_state_unlocked (GstRTSPSinkServer *server)
{
  g_clear_pointer (&server->rtp_caps, gst_caps_unref);
  g_clear_pointer (&server->rtp_media, g_free);
  g_clear_pointer (&server->rtp_encoding_name, g_free);
  g_clear_pointer (&server->rtp_fmtp, g_free);
  server->rtp_payload_type = 0;
  server->rtp_clock_rate = 0;
  server->have_latest_rtp = FALSE;
  server->latest_seqnum = 0;
  server->latest_rtptime = 0;
  server->codec = GST_RTSP_SINK_CODEC_UNKNOWN;
  server->length_prefixed_format = FALSE;
  server->nal_length_size = 4;
  g_clear_pointer (&server->h264_profile_level_id, g_free);
  g_clear_pointer (&server->h264_sprop_parameter_sets, g_free);
  g_clear_pointer (&server->h265_sprop_vps, g_free);
  g_clear_pointer (&server->h265_sprop_sps, g_free);
  g_clear_pointer (&server->h265_sprop_pps, g_free);
  if (server->h264_sps != NULL)
    g_byte_array_set_size (server->h264_sps, 0);
  if (server->h264_pps != NULL)
    g_byte_array_set_size (server->h264_pps, 0);
  if (server->h265_vps != NULL)
    g_byte_array_set_size (server->h265_vps, 0);
  if (server->h265_sps != NULL)
    g_byte_array_set_size (server->h265_sps, 0);
  if (server->h265_pps != NULL)
    g_byte_array_set_size (server->h265_pps, 0);
}

static guint
server_client_count_unlocked (GstRTSPSinkServer *server)
{
  GList *iter;
  guint count = 0;

  for (iter = server->clients; iter != NULL; iter = iter->next) {
    GstRTSPSinkClient *client = iter->data;

    if (!client->closed)
      count++;
  }

  return count;
}

static gboolean
parse_port (guint configured_port, guint16 *port, GError **error)
{
  if (configured_port == 0 || configured_port > 65535) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
        "port must be between 1 and 65535");
    return FALSE;
  }

  *port = (guint16) configured_port;
  return TRUE;
}

static gpointer
client_thread_main (gpointer data)
{
  GstRTSPSinkClient *client = data;

  while (!client->closed) {
    GstRTSPSinkRequest request;
    gchar *response;

    if (!gst_rtsp_sink_parse_request (client, &request))
      break;

    response = gst_rtsp_sink_handle_request (client, &request);
    if (response != NULL)
      gst_rtsp_sink_client_write_text (client, response);

    g_free (response);
    gst_rtsp_sink_request_clear (&request);
  }

  client->closed = TRUE;
  client->state = GST_RTSP_SINK_STATE_CLOSED;
  g_io_stream_close (G_IO_STREAM (client->connection), NULL, NULL);
  return NULL;
}

static gpointer
accept_thread_main (gpointer data)
{
  GstRTSPSinkServer *server = data;

  while (TRUE) {
    GError *error = NULL;
    GSocketConnection *connection;
    GstRTSPSinkClient *client;

    connection = g_socket_listener_accept (server->listener, NULL,
        server->cancellable, &error);
    if (connection == NULL) {
      if (error != NULL && g_error_matches (error, G_IO_ERROR,
              G_IO_ERROR_CANCELLED)) {
        g_clear_error (&error);
        break;
      }
      g_clear_error (&error);
      continue;
    }

    client = gst_rtsp_sink_client_new (server, connection);
    g_object_unref (connection);

    g_mutex_lock (&server->lock);
    if (server->stopping || server_client_count_unlocked (server) >=
        server->max_clients) {
      g_mutex_unlock (&server->lock);
      g_io_stream_close (G_IO_STREAM (client->connection), NULL, NULL);
      gst_rtsp_sink_client_free (client);
      continue;
    }
    server->clients = g_list_append (server->clients, client);
    g_mutex_unlock (&server->lock);

    client->thread = g_thread_new ("rtspsink-client", client_thread_main,
        client);
  }

  return NULL;
}

GstRTSPSinkServer *
gst_rtsp_sink_server_new (void)
{
  GstRTSPSinkServer *server = g_new0 (GstRTSPSinkServer, 1);

  g_mutex_init (&server->lock);
  g_cond_init (&server->cond);
  gst_rtsp_sink_server_reset_codec_state_unlocked (server);
  server->next_session_id = 1000;
  server->max_clients = 16;
  server->latency_ms = 200;
  server->drop_slow_clients = TRUE;
  server->enable_udp = TRUE;
  server->enable_tcp_interleaved = TRUE;
  server->auth_mode = g_strdup ("none");
  server->username = g_strdup ("");
  server->password = g_strdup ("");
  server->realm = g_strdup ("GStreamer RTSP Sink");
  return server;
}

void
gst_rtsp_sink_server_free (GstRTSPSinkServer *server)
{
  if (server == NULL)
    return;

  gst_rtsp_sink_server_stop (server);
  g_cond_clear (&server->cond);
  g_mutex_clear (&server->lock);
  g_clear_pointer (&server->rtp_caps, gst_caps_unref);
  g_clear_pointer (&server->rtp_media, g_free);
  g_clear_pointer (&server->rtp_encoding_name, g_free);
  g_clear_pointer (&server->rtp_fmtp, g_free);
  g_clear_pointer (&server->address, g_free);
  g_clear_pointer (&server->path, g_free);
  g_clear_pointer (&server->auth_mode, g_free);
  g_clear_pointer (&server->username, g_free);
  g_clear_pointer (&server->password, g_free);
  g_clear_pointer (&server->realm, g_free);
  g_clear_pointer (&server->sdp, g_free);
  g_clear_pointer (&server->h264_profile_level_id, g_free);
  g_clear_pointer (&server->h264_sprop_parameter_sets, g_free);
  g_clear_pointer (&server->h265_sprop_vps, g_free);
  g_clear_pointer (&server->h265_sprop_sps, g_free);
  g_clear_pointer (&server->h265_sprop_pps, g_free);
  if (server->h264_sps != NULL)
    g_byte_array_unref (server->h264_sps);
  if (server->h264_pps != NULL)
    g_byte_array_unref (server->h264_pps);
  if (server->h265_vps != NULL)
    g_byte_array_unref (server->h265_vps);
  if (server->h265_sps != NULL)
    g_byte_array_unref (server->h265_sps);
  if (server->h265_pps != NULL)
    g_byte_array_unref (server->h265_pps);
  g_free (server);
}

gboolean
gst_rtsp_sink_server_start (GstRTSPSinkServer *server,
    const GstRTSPSinkServerConfig *config, GError **error)
{
  GInetAddress *inet_address = NULL;
  GSocketAddress *socket_address = NULL;
  GSocket *socket = NULL;
  guint16 port;

  g_return_val_if_fail (server != NULL, FALSE);
  g_return_val_if_fail (config != NULL, FALSE);

  if (config->auth_mode != NULL && g_strcmp0 (config->auth_mode, "none") != 0) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
        "RTSP authentication modes are not implemented yet");
    return FALSE;
  }

  if (!parse_port (config->port, &port, error))
    return FALSE;

  inet_address = g_inet_address_new_from_string (config->address);
  if (inet_address == NULL) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
        "invalid bind address '%s'", config->address);
    return FALSE;
  }

  socket_address = g_inet_socket_address_new (inet_address, port);
  socket = g_socket_new (g_socket_address_get_family (socket_address),
      G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, error);
  if (socket == NULL)
    goto fail;

  g_socket_set_listen_backlog (socket, config->backlog);
  if (!g_socket_bind (socket, socket_address, TRUE, error))
    goto fail;
  if (!g_socket_listen (socket, error))
    goto fail;

  g_mutex_lock (&server->lock);
  server->listener = g_socket_listener_new ();
  server->cancellable = g_cancellable_new ();
  if (!g_socket_listener_add_socket (server->listener, socket, NULL, error)) {
    g_mutex_unlock (&server->lock);
    goto fail;
  }
  server->stopping = FALSE;
  server->have_clock_base = FALSE;
  server->have_latest_rtp = FALSE;
  server->latest_seqnum = 0;
  server->latest_rtptime = 0;
  gst_rtsp_sink_apply_config (server, config);
  gst_rtsp_sink_sdp_update_unlocked (server);
  server->accept_thread = g_thread_new ("rtspsink-accept", accept_thread_main,
      server);
  g_mutex_unlock (&server->lock);

  g_object_unref (socket);
  g_object_unref (socket_address);
  g_object_unref (inet_address);
  return TRUE;

fail:
  g_clear_object (&socket);
  g_clear_object (&socket_address);
  g_clear_object (&inet_address);
  g_clear_object (&server->listener);
  g_clear_object (&server->cancellable);
  return FALSE;
}

void
gst_rtsp_sink_server_stop (GstRTSPSinkServer *server)
{
  GList *iter;
  GList *clients = NULL;

  if (server == NULL)
    return;

  g_mutex_lock (&server->lock);
  server->stopping = TRUE;
  if (server->cancellable != NULL)
    g_cancellable_cancel (server->cancellable);
  while (server->active_pushers > 0)
    g_cond_wait (&server->cond, &server->lock);
  clients = g_list_copy (server->clients);
  g_mutex_unlock (&server->lock);

  for (iter = clients; iter != NULL; iter = iter->next) {
    GstRTSPSinkClient *client = iter->data;

    client->closed = TRUE;
    client->state = GST_RTSP_SINK_STATE_CLOSED;
    if (client->connection != NULL)
      g_io_stream_close (G_IO_STREAM (client->connection), NULL, NULL);
  }

  if (server->accept_thread != NULL) {
    g_thread_join (server->accept_thread);
    server->accept_thread = NULL;
  }

  for (iter = clients; iter != NULL; iter = iter->next) {
    GstRTSPSinkClient *client = iter->data;

    if (client->thread != NULL) {
      g_thread_join (client->thread);
      client->thread = NULL;
    }
    gst_rtsp_sink_client_free (client);
  }
  g_list_free (clients);

  g_mutex_lock (&server->lock);
  g_list_free (server->clients);
  server->clients = NULL;
  g_clear_object (&server->listener);
  g_clear_object (&server->cancellable);
  g_mutex_unlock (&server->lock);
}

gboolean
gst_rtsp_sink_server_set_caps (GstRTSPSinkServer *server, GstCaps *caps,
    GError **error)
{
  GstStructure *s;
  const gchar *name;

  g_return_val_if_fail (server != NULL, FALSE);
  g_return_val_if_fail (caps != NULL, FALSE);

  s = gst_caps_get_structure (caps, 0);
  name = gst_structure_get_name (s);
  if (g_strcmp0 (name, "application/x-rtp") == 0)
    return gst_rtsp_sink_server_set_rtp_caps_internal (server, caps, error);

  g_set_error (error, GST_STREAM_ERROR, GST_STREAM_ERROR_FORMAT,
      "unsupported caps type '%s'", GST_STR_NULL (name));
  return FALSE;
}

void
gst_rtsp_sink_server_push_buffer (GstRTSPSinkServer *server, GstBuffer *buffer)
{
  g_return_if_fail (server != NULL);
  g_return_if_fail (buffer != NULL);

  gst_rtsp_sink_server_push_buffer_internal (server, buffer);
}
