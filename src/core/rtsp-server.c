#include "rtsp-server-internal.h"

#include "buffer-model.h"
#include "h264-pay.h"
#include "h265-pay.h"
#include "rtspsink-server.h"

#include <gio/gio.h>

gboolean
gst_rtsp_sink_validate_input_caps(const GstCaps *caps,
                                  GstRTSPSinkCodec *codec,
                                  gchar **stream_format,
                                  gchar **alignment)
{
  const GstStructure *structure;
  const gchar *media_type;
  const gchar *caps_stream_format;
  const gchar *caps_alignment;

  if (caps == NULL || gst_caps_is_empty(caps) || gst_caps_is_any(caps))
    return FALSE;

  structure = gst_caps_get_structure(caps, 0);
  if (structure == NULL)
    return FALSE;

  media_type = gst_structure_get_name(structure);
  caps_stream_format = gst_structure_get_string(structure, "stream-format");
  caps_alignment = gst_structure_get_string(structure, "alignment");

  if (g_strcmp0(media_type, "video/x-h264") == 0) {
    gboolean stream_ok = g_strcmp0(caps_stream_format, "byte-stream") == 0 ||
                         g_strcmp0(caps_stream_format, "avc") == 0;
    gboolean alignment_ok = g_strcmp0(caps_alignment, "au") == 0 ||
                            g_strcmp0(caps_alignment, "nal") == 0;

    if (!stream_ok || !alignment_ok)
      return FALSE;

    if (codec != NULL)
      *codec = GST_RTSP_SINK_CODEC_H264;
  } else if (g_strcmp0(media_type, "video/x-h265") == 0) {
    gboolean stream_ok = g_strcmp0(caps_stream_format, "byte-stream") == 0 ||
                         g_strcmp0(caps_stream_format, "hvc1") == 0;
    gboolean alignment_ok = g_strcmp0(caps_alignment, "au") == 0 ||
                            g_strcmp0(caps_alignment, "nal") == 0;

    if (!stream_ok || !alignment_ok)
      return FALSE;

    if (codec != NULL)
      *codec = GST_RTSP_SINK_CODEC_H265;
  } else {
    return FALSE;
  }

  if (stream_format != NULL)
    *stream_format = g_strdup(caps_stream_format != NULL ? caps_stream_format : "byte-stream");
  if (alignment != NULL)
    *alignment = g_strdup(caps_alignment != NULL ? caps_alignment : "au");

  return TRUE;
}

struct _GstRTSPSinkServer
{
  GstRTSPSinkServerConfig *config;
  GstCaps *caps;
  gboolean running;
  gboolean flushing;
  GstRTSPSinkServerState state;
  guint16 next_seqnum;
  guint32 next_timestamp;
  guint32 ssrc;
  gchar *session_id;
  GstBuffer *cached_parameter_sets;
  GstBuffer *cached_keyframe;
  GstSegment segment;
  gboolean have_segment;
  GMutex lock;
  GCond cond;
  GQueue *buffer_queue;
  GSocketListener *listener;
  GThread *accept_thread;
  GPtrArray *clients;
};

static void
gst_rtsp_sink_server_update_state(GstRTSPSinkServer *server, GstRTSPSinkServerState state)
{
  if (server != NULL)
    server->state = state;
}

static void
gst_rtsp_sink_server_clear_queue(GstRTSPSinkServer *server)
{
  if (server == NULL || server->buffer_queue == NULL)
    return;

  while (!g_queue_is_empty(server->buffer_queue)) {
    GstBuffer *buffer = g_queue_pop_head(server->buffer_queue);
    if (buffer != NULL)
      gst_buffer_unref(buffer);
  }
}

static void
gst_rtsp_sink_server_replace_cached_buffer(GstBuffer **slot, GstBuffer *buffer)
{
  g_clear_pointer(slot, gst_buffer_unref);
  if (buffer != NULL)
    *slot = gst_buffer_ref(buffer);
}

static gpointer
gst_rtsp_sink_server_accept_loop(gpointer data)
{
  GstRTSPSinkServer *server = data;

  while (TRUE) {
    GError *error = NULL;
    GSocketConnection *connection = NULL;
    GstRTSPSinkClient *client = NULL;

    connection = g_socket_listener_accept(server->listener, NULL, NULL, &error);
    if (connection == NULL) {
      if (!server->running) {
        g_clear_error(&error);
        break;
      }

      if (error != NULL &&
          (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ||
           g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CLOSED))) {
        g_clear_error(&error);
        break;
      }

      g_clear_error(&error);
      g_usleep(100000);
      continue;
    }

    if (!server->running) {
      g_object_unref(connection);
      break;
    }

    client = gst_rtsp_sink_client_new(server, connection);
    g_object_unref(connection);
    if (client == NULL)
      continue;

    if (!gst_rtsp_sink_server_add_client(server, client)) {
      gst_rtsp_sink_client_free(client);
      continue;
    }

    {
      GThread *thread = g_thread_new("rtsp-client", gst_rtsp_sink_client_run, client);
      if (thread != NULL) {
        g_thread_unref(thread);
      } else {
        gst_rtsp_sink_server_remove_client(server, client);
        gst_rtsp_sink_client_free(client);
      }
    }
  }

  return NULL;
}

GstRTSPSinkServer *
gst_rtsp_sink_server_new(const GstRTSPSinkServerConfig *config)
{
  GstRTSPSinkServer *server = g_new0(GstRTSPSinkServer, 1);

  if (config != NULL) {
    server->config = gst_rtsp_sink_server_config_new();
    server->config->port = config->port;
    g_free(server->config->path);
    server->config->path = g_strdup(config->path);
    server->config->transport_mode = config->transport_mode;
    server->config->auth_enabled = config->auth_enabled;
    g_free(server->config->auth_user);
    server->config->auth_user = g_strdup(config->auth_user);
    g_free(server->config->auth_password);
    server->config->auth_password = g_strdup(config->auth_password);
    server->config->codec = config->codec;
    g_free(server->config->stream_format);
    server->config->stream_format = g_strdup(config->stream_format);
    g_free(server->config->alignment);
    server->config->alignment = g_strdup(config->alignment);
    server->config->rtp = config->rtp;
  } else {
    server->config = gst_rtsp_sink_server_config_new();
  }

  g_mutex_init(&server->lock);
  g_cond_init(&server->cond);
  server->buffer_queue = g_queue_new();
  server->clients = g_ptr_array_new();
  server->state = GST_RTSP_SERVER_STATE_IDLE;
  gst_segment_init(&server->segment, GST_FORMAT_TIME);
  server->session_id = g_strdup_printf("%08x", g_random_int());
  server->next_seqnum = server->config->rtp.sequence_number_base;
  server->next_timestamp = server->config->rtp.timestamp_base;
  server->ssrc = server->config->rtp.ssrc != 0 ? server->config->rtp.ssrc : g_random_int();
  server->config->rtp.ssrc = server->ssrc;
  return server;
}

void
gst_rtsp_sink_server_free(GstRTSPSinkServer *server)
{
  if (server == NULL)
    return;

  gst_rtsp_sink_server_stop(server);
  g_clear_object(&server->listener);
  g_clear_pointer(&server->caps, gst_caps_unref);
  gst_rtsp_sink_server_config_free(server->config);
  gst_rtsp_sink_server_update_state(server, GST_RTSP_SERVER_STATE_CLOSED);
  g_clear_pointer(&server->session_id, g_free);
  g_clear_pointer(&server->cached_parameter_sets, gst_buffer_unref);
  g_clear_pointer(&server->cached_keyframe, gst_buffer_unref);
  if (server->buffer_queue != NULL)
    g_queue_free(server->buffer_queue);
  if (server->clients != NULL)
    g_ptr_array_free(server->clients, TRUE);
  g_cond_clear(&server->cond);
  g_mutex_clear(&server->lock);
  g_free(server);
}

const GstRTSPSinkRtpConfig *
gst_rtsp_sink_server_get_rtp_config(const GstRTSPSinkServer *server)
{
  g_return_val_if_fail(server != NULL, NULL);
  return &server->config->rtp;
}

const GstRTSPSinkServerConfig *
gst_rtsp_sink_server_get_config(const GstRTSPSinkServer *server)
{
  g_return_val_if_fail(server != NULL, NULL);
  return server->config;
}

const gchar *
gst_rtsp_sink_server_get_session_id(const GstRTSPSinkServer *server)
{
  g_return_val_if_fail(server != NULL, NULL);
  return server->session_id;
}

GstRTSPSinkServerState
gst_rtsp_sink_server_get_state(const GstRTSPSinkServer *server)
{
  g_return_val_if_fail(server != NULL, GST_RTSP_SERVER_STATE_CLOSED);
  return server->state;
}

gboolean
gst_rtsp_sink_server_get_warm_start_buffers(GstRTSPSinkServer *server,
                                            GstBuffer **parameter_sets,
                                            GstBuffer **keyframe)
{
  g_return_val_if_fail(server != NULL, FALSE);

  if (parameter_sets != NULL)
    *parameter_sets = NULL;
  if (keyframe != NULL)
    *keyframe = NULL;

  g_mutex_lock(&server->lock);
  if (parameter_sets != NULL && server->cached_parameter_sets != NULL)
    *parameter_sets = gst_buffer_ref(server->cached_parameter_sets);
  if (keyframe != NULL && server->cached_keyframe != NULL)
    *keyframe = gst_buffer_ref(server->cached_keyframe);
  g_mutex_unlock(&server->lock);

  return TRUE;
}

gboolean
gst_rtsp_sink_server_enqueue_warm_start(GstRTSPSinkServer *server)
{
  GstBuffer *parameter_sets = NULL;
  GstBuffer *keyframe = NULL;
  gboolean ok = FALSE;

  if (server == NULL)
    return FALSE;

  if (!gst_rtsp_sink_server_get_warm_start_buffers(server, &parameter_sets, &keyframe))
    return FALSE;

  g_mutex_lock(&server->lock);
  if (parameter_sets != NULL) {
    gst_rtsp_sink_server_replace_cached_buffer(&server->cached_parameter_sets, parameter_sets);
    ok = TRUE;
  }
  if (keyframe != NULL) {
    gst_rtsp_sink_server_replace_cached_buffer(&server->cached_keyframe, keyframe);
    ok = TRUE;
  }
  g_mutex_unlock(&server->lock);

  if (parameter_sets != NULL)
    gst_buffer_unref(parameter_sets);
  if (keyframe != NULL)
    gst_buffer_unref(keyframe);

  return ok;
}

void
gst_rtsp_sink_server_update_segment(GstRTSPSinkServer *server, const GstSegment *segment)
{
  g_return_if_fail(server != NULL);

  g_mutex_lock(&server->lock);
  if (segment != NULL) {
    server->segment = *segment;
    server->have_segment = TRUE;
  }
  g_mutex_unlock(&server->lock);
}

void
gst_rtsp_sink_server_reset_rtp_state(GstRTSPSinkServer *server,
                                     guint16 sequence_number_base,
                                     guint32 timestamp_base,
                                     guint32 ssrc)
{
  g_return_if_fail(server != NULL);

  g_mutex_lock(&server->lock);
  server->next_seqnum = sequence_number_base;
  server->next_timestamp = timestamp_base;
  server->ssrc = ssrc;
  server->config->rtp.sequence_number_base = sequence_number_base;
  server->config->rtp.timestamp_base = timestamp_base;
  server->config->rtp.ssrc = ssrc;
  g_mutex_unlock(&server->lock);
}

gboolean
gst_rtsp_sink_server_add_client(GstRTSPSinkServer *server, GstRTSPSinkClient *client)
{
  g_return_val_if_fail(server != NULL, FALSE);
  g_return_val_if_fail(client != NULL, FALSE);

  g_mutex_lock(&server->lock);
  if (server->clients == NULL) {
    g_mutex_unlock(&server->lock);
    return FALSE;
  }

  g_ptr_array_add(server->clients, gst_rtsp_sink_client_ref(client));
  g_mutex_unlock(&server->lock);
  return TRUE;
}

void
gst_rtsp_sink_server_remove_client(GstRTSPSinkServer *server, GstRTSPSinkClient *client)
{
  if (server == NULL || client == NULL)
    return;

  g_mutex_lock(&server->lock);
  if (server->clients != NULL) {
    for (guint i = 0; i < server->clients->len; i++) {
      if (g_ptr_array_index(server->clients, i) == client) {
        g_ptr_array_remove_index(server->clients, i);
        g_cond_broadcast(&server->cond);
        break;
      }
    }
  }
  g_mutex_unlock(&server->lock);

  gst_rtsp_sink_client_free(client);
}

gboolean
gst_rtsp_sink_server_broadcast_buffer(GstRTSPSinkServer *server, GstBuffer *buffer)
{
  GPtrArray *snapshot;

  g_return_val_if_fail(server != NULL, FALSE);
  g_return_val_if_fail(buffer != NULL, FALSE);

  snapshot = g_ptr_array_new();
  g_mutex_lock(&server->lock);
  if (server->clients != NULL) {
    for (guint i = 0; i < server->clients->len; i++) {
      GstRTSPSinkClient *client = g_ptr_array_index(server->clients, i);
      g_ptr_array_add(snapshot, gst_rtsp_sink_client_ref(client));
    }
  }
  g_mutex_unlock(&server->lock);

  for (guint i = 0; i < snapshot->len; i++) {
    GstRTSPSinkClient *client = g_ptr_array_index(snapshot, i);

    if (!gst_rtsp_sink_client_send_buffer(client, buffer))
      gst_rtsp_sink_client_stop(client);

    gst_rtsp_sink_client_free(client);
  }

  g_ptr_array_free(snapshot, TRUE);
  return TRUE;
}

gboolean
gst_rtsp_sink_server_start(GstRTSPSinkServer *server, GError **error)
{
  g_return_val_if_fail(server != NULL, FALSE);

  if (server->running)
    return TRUE;

  g_clear_object(&server->listener);
  server->listener = g_socket_listener_new();
  if (server->listener == NULL)
    return FALSE;

  if (!g_socket_listener_add_inet_port(server->listener, server->config->port, NULL, error)) {
    g_clear_object(&server->listener);
    return FALSE;
  }

  g_mutex_lock(&server->lock);
  server->running = TRUE;
  server->flushing = FALSE;
  gst_rtsp_sink_server_update_state(server, GST_RTSP_SERVER_STATE_READY);
  g_mutex_unlock(&server->lock);

  server->accept_thread = g_thread_new("rtsp-accept", gst_rtsp_sink_server_accept_loop, server);
  return TRUE;
}

void
gst_rtsp_sink_server_stop(GstRTSPSinkServer *server)
{
  GPtrArray *snapshot = NULL;

  if (server == NULL)
    return;

  g_mutex_lock(&server->lock);
  if (!server->running) {
    g_mutex_unlock(&server->lock);
    return;
  }

  server->running = FALSE;
  server->flushing = TRUE;
  gst_rtsp_sink_server_update_state(server, GST_RTSP_SERVER_STATE_STOPPING);
  gst_rtsp_sink_server_clear_queue(server);
  snapshot = g_ptr_array_new();
  if (server->clients != NULL) {
    for (guint i = 0; i < server->clients->len; i++)
      g_ptr_array_add(snapshot, gst_rtsp_sink_client_ref(g_ptr_array_index(server->clients, i)));
  }
  g_mutex_unlock(&server->lock);

  g_clear_object(&server->listener);

  for (guint i = 0; i < snapshot->len; i++) {
    GstRTSPSinkClient *client = g_ptr_array_index(snapshot, i);
    gst_rtsp_sink_client_stop(client);
    gst_rtsp_sink_client_free(client);
  }
  g_ptr_array_free(snapshot, TRUE);

  if (server->accept_thread != NULL) {
    g_thread_join(server->accept_thread);
    server->accept_thread = NULL;
  }

  g_mutex_lock(&server->lock);
  while (server->clients != NULL && server->clients->len > 0)
    g_cond_wait(&server->cond, &server->lock);
  g_mutex_unlock(&server->lock);
}

gboolean
gst_rtsp_sink_server_set_caps(GstRTSPSinkServer *server, const GstCaps *caps)
{
  g_return_val_if_fail(server != NULL, FALSE);

  GstRTSPSinkCodec codec = GST_RTSP_SINK_CODEC_H264;
  gchar *stream_format = NULL;
  gchar *alignment = NULL;

  if (!gst_rtsp_sink_validate_input_caps(caps, &codec, &stream_format, &alignment)) {
    g_free(stream_format);
    g_free(alignment);
    return FALSE;
  }

  g_clear_pointer(&server->caps, gst_caps_unref);
  if (caps != NULL)
    server->caps = gst_caps_copy(caps);

  server->config->codec = codec;
  g_free(server->config->stream_format);
  server->config->stream_format = stream_format;
  g_free(server->config->alignment);
  server->config->alignment = alignment;
  if (server->running)
    gst_rtsp_sink_server_update_state(server, GST_RTSP_SERVER_STATE_READY);

  return TRUE;
}

gboolean
gst_rtsp_sink_server_push_buffer(GstRTSPSinkServer *server, GstBuffer *buffer)
{
  g_return_val_if_fail(server != NULL, FALSE);
  g_return_val_if_fail(buffer != NULL, FALSE);

  g_mutex_lock(&server->lock);
  if (!server->running) {
    g_mutex_unlock(&server->lock);
    return FALSE;
  }

  if (server->state == GST_RTSP_SERVER_STATE_READY || server->state == GST_RTSP_SERVER_STATE_PAUSED)
    gst_rtsp_sink_server_update_state(server, GST_RTSP_SERVER_STATE_PLAYING);

  if (server->config->codec == GST_RTSP_SINK_CODEC_H264) {
    gboolean is_keyframe = FALSE;
    gboolean has_parameter_sets = FALSE;
    if (gst_rtsp_sink_h264_analyze_buffer(buffer, server->config->stream_format, &is_keyframe, &has_parameter_sets)) {
      if (has_parameter_sets)
        gst_rtsp_sink_server_replace_cached_buffer(&server->cached_parameter_sets, buffer);
      if (is_keyframe)
        gst_rtsp_sink_server_replace_cached_buffer(&server->cached_keyframe, buffer);
    }
  } else {
    gboolean is_keyframe = FALSE;
    gboolean has_parameter_sets = FALSE;
    if (gst_rtsp_sink_h265_analyze_buffer(buffer, server->config->stream_format, &is_keyframe, &has_parameter_sets)) {
      if (has_parameter_sets)
        gst_rtsp_sink_server_replace_cached_buffer(&server->cached_parameter_sets, buffer);
      if (is_keyframe)
        gst_rtsp_sink_server_replace_cached_buffer(&server->cached_keyframe, buffer);
    }
  }
  g_mutex_unlock(&server->lock);

  return gst_rtsp_sink_server_broadcast_buffer(server, buffer);
}

gboolean
gst_rtsp_sink_server_pop_buffer(GstRTSPSinkServer *server, GstBuffer **buffer)
{
  g_return_val_if_fail(server != NULL, FALSE);
  g_return_val_if_fail(buffer != NULL, FALSE);

  *buffer = NULL;
  return FALSE;
}

void
gst_rtsp_sink_server_unlock(GstRTSPSinkServer *server)
{
  if (server != NULL) {
    g_mutex_lock(&server->lock);
    server->running = FALSE;
    server->flushing = TRUE;
    if (server->state != GST_RTSP_SERVER_STATE_CLOSED)
      gst_rtsp_sink_server_update_state(server, GST_RTSP_SERVER_STATE_PAUSED);
    g_cond_broadcast(&server->cond);
    g_mutex_unlock(&server->lock);
  }
}

void
gst_rtsp_sink_server_set_state(GstRTSPSinkServer *server, GstRTSPSinkServerState state)
{
  g_return_if_fail(server != NULL);

  g_mutex_lock(&server->lock);
  gst_rtsp_sink_server_update_state(server, state);
  g_mutex_unlock(&server->lock);
}
