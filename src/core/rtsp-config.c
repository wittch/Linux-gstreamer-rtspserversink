#include "rtsp-server-internal.h"

void
gst_rtsp_sink_apply_config (GstRTSPSinkServer *server,
    const GstRTSPSinkServerConfig *config)
{
  g_clear_pointer (&server->address, g_free);
  g_clear_pointer (&server->path, g_free);
  g_clear_pointer (&server->auth_mode, g_free);
  g_clear_pointer (&server->username, g_free);
  g_clear_pointer (&server->password, g_free);
  g_clear_pointer (&server->realm, g_free);

  server->address = g_strdup (config->address);
  server->port = config->port;
  server->path = g_strdup (config->path != NULL ? config->path : "");
  server->backlog = config->backlog;
  server->max_clients = config->max_clients;
  server->latency_ms = config->latency_ms;
  server->drop_slow_clients = config->drop_slow_clients;
  server->auth_mode = g_strdup (config->auth_mode != NULL ?
      config->auth_mode : "none");
  server->username = g_strdup (config->username != NULL ? config->username : "");
  server->password = g_strdup (config->password != NULL ? config->password : "");
  server->realm = g_strdup (config->realm != NULL ?
      config->realm : "GStreamer RTSP Sink");
  server->enable_udp = config->enable_udp;
  server->enable_tcp_interleaved = config->enable_tcp_interleaved;
}
