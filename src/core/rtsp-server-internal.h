#pragma once

#include "rtspsink-server.h"
#include <gio/gio.h>

typedef struct _GstRTSPSinkRequest GstRTSPSinkRequest;
typedef struct _GstRTSPSinkClient GstRTSPSinkClient;

typedef enum
{
  GST_RTSP_CLIENT_STATE_IDLE = 0,
  GST_RTSP_CLIENT_STATE_READY,
  GST_RTSP_CLIENT_STATE_PLAYING,
  GST_RTSP_CLIENT_STATE_PAUSED,
  GST_RTSP_CLIENT_STATE_CLOSED,
} GstRTSPSinkClientState;

GstRTSPSinkClient *gst_rtsp_sink_client_new(GstRTSPSinkServer *server,
                                            GSocketConnection *connection);
GstRTSPSinkClient *gst_rtsp_sink_client_ref(GstRTSPSinkClient *client);
void gst_rtsp_sink_client_free(GstRTSPSinkClient *client);
void gst_rtsp_sink_client_stop(GstRTSPSinkClient *client);
gpointer gst_rtsp_sink_client_run(gpointer data);
gboolean gst_rtsp_sink_client_send_response(GstRTSPSinkClient *client,
                                            const gchar *response);
gboolean gst_rtsp_sink_client_send_cached_start(GstRTSPSinkClient *client);
gboolean gst_rtsp_sink_client_send_buffer(GstRTSPSinkClient *client,
                                          const GstBuffer *buffer);
gboolean gst_rtsp_sink_client_is_playing(const GstRTSPSinkClient *client);
void gst_rtsp_sink_client_set_state(GstRTSPSinkClient *client,
                                    GstRTSPSinkClientState state);
GstRTSPSinkClientState gst_rtsp_sink_client_get_state(const GstRTSPSinkClient *client);
const gchar *gst_rtsp_sink_client_get_session_id(const GstRTSPSinkClient *client);
guint gst_rtsp_sink_client_get_udp_server_port(const GstRTSPSinkClient *client);
void gst_rtsp_sink_client_touch(GstRTSPSinkClient *client);

gboolean gst_rtsp_sink_server_add_client(GstRTSPSinkServer *server,
                                         GstRTSPSinkClient *client);
void gst_rtsp_sink_server_remove_client(GstRTSPSinkServer *server,
                                        GstRTSPSinkClient *client);
gboolean gst_rtsp_sink_server_broadcast_buffer(GstRTSPSinkServer *server,
                                               GstBuffer *buffer);
