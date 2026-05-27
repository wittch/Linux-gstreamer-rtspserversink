#ifndef __RTSP_SINK_SERVER_H__
#define __RTSP_SINK_SERVER_H__

#include <gst/gst.h>

G_BEGIN_DECLS

typedef struct _GstRTSPSinkServer GstRTSPSinkServer;

typedef struct _GstRTSPSinkServerConfig
{
  gchar *address;
  guint port;
  gchar *path;
  guint backlog;
  guint max_clients;
  guint latency_ms;
  gboolean drop_slow_clients;
  gchar *auth_mode;
  gchar *username;
  gchar *password;
  gchar *realm;
  gboolean enable_udp;
  gboolean enable_tcp_interleaved;
} GstRTSPSinkServerConfig;

GstRTSPSinkServer * gst_rtsp_sink_server_new (void);
void gst_rtsp_sink_server_free (GstRTSPSinkServer *server);

gboolean gst_rtsp_sink_server_start (GstRTSPSinkServer *server,
    const GstRTSPSinkServerConfig *config, GError **error);
void gst_rtsp_sink_server_stop (GstRTSPSinkServer *server);

gboolean gst_rtsp_sink_server_set_caps (GstRTSPSinkServer *server,
    GstCaps *caps, GError **error);
void gst_rtsp_sink_server_push_buffer (GstRTSPSinkServer *server,
    GstBuffer *buffer);

G_END_DECLS

#endif
