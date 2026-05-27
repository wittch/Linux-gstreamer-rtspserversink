#include "rtsp-server-internal.h"

static GBufferedInputStream *
client_get_buffered_input (GstRTSPSinkClient *client)
{
  GInputStream *base;

  base = g_filter_input_stream_get_base_stream (
      G_FILTER_INPUT_STREAM (client->input));
  return G_BUFFERED_INPUT_STREAM (base);
}

GstRTSPSinkClient *
gst_rtsp_sink_client_new (GstRTSPSinkServer *server,
    GSocketConnection *connection)
{
  GstRTSPSinkClient *client;
  GInputStream *base_input;
  GInputStream *buffered_input;

  client = g_new0 (GstRTSPSinkClient, 1);
  client->server = server;
  client->connection = g_object_ref (connection);

  base_input = g_io_stream_get_input_stream (G_IO_STREAM (connection));
  buffered_input = g_buffered_input_stream_new (base_input);
  g_buffered_input_stream_set_buffer_size (
      G_BUFFERED_INPUT_STREAM (buffered_input), 4096);
  client->input = g_data_input_stream_new (buffered_input);
  g_object_unref (buffered_input);

  client->output = g_io_stream_get_output_stream (G_IO_STREAM (connection));
  client->seqnum = g_random_int_range (0, G_MAXUINT16);
  client->ssrc = g_random_int ();
  client->rtcp_cname = g_strdup_printf ("%08x@%s", client->ssrc,
      server->address != NULL ? server->address : "localhost");
  g_mutex_init (&client->write_lock);
  gst_rtsp_sink_client_reset_transport (client);
  gst_rtsp_sink_client_touch_keepalive (client);

  return client;
}

void
gst_rtsp_sink_client_free (GstRTSPSinkClient *client)
{
  if (client == NULL)
    return;

  g_clear_pointer (&client->session_id, g_free);
  g_clear_object (&client->server_rtp_socket);
  g_clear_object (&client->server_rtcp_socket);
  g_clear_object (&client->client_rtp_addr);
  g_clear_object (&client->client_rtcp_addr);
  g_clear_pointer (&client->rtcp_cname, g_free);
  g_clear_object (&client->input);
  g_clear_object (&client->connection);
  g_mutex_clear (&client->write_lock);
  g_free (client);
}

static gboolean
write_all_locked (GOutputStream *output, const guint8 *data, gsize size,
    GError **error)
{
  gsize written = 0;

  return g_output_stream_write_all (output, data, size, &written, NULL, error);
}

gboolean
gst_rtsp_sink_client_write_bytes (GstRTSPSinkClient *client, const guint8 *data,
    gsize size, gboolean flush)
{
  GError *error = NULL;
  gboolean ok;

  if (client->closed)
    return FALSE;

  g_mutex_lock (&client->write_lock);
  ok = write_all_locked (client->output, data, size, &error);
  if (ok && flush)
    ok = g_output_stream_flush (client->output, NULL, &error);
  g_mutex_unlock (&client->write_lock);

  if (!ok) {
    g_clear_error (&error);
    client->closed = TRUE;
    client->state = GST_RTSP_SINK_STATE_CLOSED;
  }

  return ok;
}

gboolean
gst_rtsp_sink_client_write_text (GstRTSPSinkClient *client, const gchar *text)
{
  return gst_rtsp_sink_client_write_bytes (client, (const guint8 *) text,
      strlen (text), TRUE);
}

void
gst_rtsp_sink_client_reset_transport (GstRTSPSinkClient *client)
{
  client->state = GST_RTSP_SINK_STATE_INIT;
  client->transport = GST_RTSP_SINK_TRANSPORT_NONE;
  client->client_rtp_port = 0;
  client->client_rtcp_port = 0;
  client->rtp_channel = 0;
  client->rtcp_channel = 1;
  client->have_rtp_info = FALSE;
  client->packet_count = 0;
  client->octet_count = 0;
  client->last_rtcp_monotonic_us = 0;
  client->pending_play_start = FALSE;
  client->wait_for_live_idr = FALSE;
  g_clear_object (&client->server_rtp_socket);
  g_clear_object (&client->server_rtcp_socket);
  g_clear_object (&client->client_rtp_addr);
  g_clear_object (&client->client_rtcp_addr);
}

void
gst_rtsp_sink_client_touch_keepalive (GstRTSPSinkClient *client)
{
  client->last_keepalive_monotonic_us = g_get_monotonic_time ();
}

GSocketAddress *
gst_rtsp_sink_client_remote_address_with_port (GstRTSPSinkClient *client,
    guint16 port)
{
  GSocketAddress *remote_address;
  GInetAddress *inet_address;
  GSocketAddress *result = NULL;

  remote_address = g_socket_connection_get_remote_address (client->connection,
      NULL);
  if (remote_address == NULL)
    return NULL;
  if (!G_IS_INET_SOCKET_ADDRESS (remote_address)) {
    g_object_unref (remote_address);
    return NULL;
  }

  inet_address = g_inet_socket_address_get_address (
      G_INET_SOCKET_ADDRESS (remote_address));
  if (inet_address != NULL)
    result = g_inet_socket_address_new (inet_address, port);

  g_object_unref (remote_address);
  return result;
}

GBufferedInputStream *
gst_rtsp_sink_client_peek_input (GstRTSPSinkClient *client)
{
  return client_get_buffered_input (client);
}
