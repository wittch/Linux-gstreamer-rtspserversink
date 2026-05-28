#pragma once

#include <gst/gst.h>

G_BEGIN_DECLS

typedef struct _GstRTSPSinkServer GstRTSPSinkServer;

typedef enum
{
  GST_RTSP_SINK_TRANSPORT_UDP = 0,
  GST_RTSP_SINK_TRANSPORT_TCP = 1,
  GST_RTSP_SINK_TRANSPORT_BOTH = 2,
} GstRTSPSinkTransportMode;

typedef enum
{
  GST_RTSP_SINK_CODEC_H264 = 0,
  GST_RTSP_SINK_CODEC_H265 = 1,
} GstRTSPSinkCodec;

typedef enum
{
  GST_RTSP_SERVER_STATE_IDLE = 0,
  GST_RTSP_SERVER_STATE_READY,
  GST_RTSP_SERVER_STATE_PLAYING,
  GST_RTSP_SERVER_STATE_PAUSED,
  GST_RTSP_SERVER_STATE_STOPPING,
  GST_RTSP_SERVER_STATE_CLOSED,
} GstRTSPSinkServerState;

typedef struct _GstRTSPSinkRtpConfig
{
  guint payload_type;
  guint clock_rate;
  guint mtu;
  gboolean marker_per_access_unit;
  guint16 sequence_number_base;
  guint32 timestamp_base;
  guint32 ssrc;
} GstRTSPSinkRtpConfig;

typedef struct _GstRTSPSinkServerConfig
{
  guint port;
  gchar *path;
  GstRTSPSinkTransportMode transport_mode;
  gboolean auth_enabled;
  gchar *auth_user;
  gchar *auth_password;
  GstRTSPSinkCodec codec;
  gchar *stream_format;
  gchar *alignment;
  GstRTSPSinkRtpConfig rtp;
} GstRTSPSinkServerConfig;

GstRTSPSinkServerConfig *gst_rtsp_sink_server_config_new(void);
void gst_rtsp_sink_server_config_free(GstRTSPSinkServerConfig *config);

GstRTSPSinkServer *gst_rtsp_sink_server_new(const GstRTSPSinkServerConfig *config);
void gst_rtsp_sink_server_free(GstRTSPSinkServer *server);

gboolean gst_rtsp_sink_validate_input_caps(const GstCaps *caps,
                                           GstRTSPSinkCodec *codec,
                                           gchar **stream_format,
                                           gchar **alignment);
const GstRTSPSinkRtpConfig *gst_rtsp_sink_server_get_rtp_config(const GstRTSPSinkServer *server);
const GstRTSPSinkServerConfig *gst_rtsp_sink_server_get_config(const GstRTSPSinkServer *server);
const gchar *gst_rtsp_sink_server_get_session_id(const GstRTSPSinkServer *server);
GstRTSPSinkServerState gst_rtsp_sink_server_get_state(const GstRTSPSinkServer *server);
void gst_rtsp_sink_server_set_state(GstRTSPSinkServer *server, GstRTSPSinkServerState state);
gboolean gst_rtsp_sink_server_get_warm_start_buffers(GstRTSPSinkServer *server,
                                                     GstBuffer **parameter_sets,
                                                     GstBuffer **keyframe);
gboolean gst_rtsp_sink_server_enqueue_warm_start(GstRTSPSinkServer *server);
void gst_rtsp_sink_server_update_segment(GstRTSPSinkServer *server, const GstSegment *segment);
void gst_rtsp_sink_server_reset_rtp_state(GstRTSPSinkServer *server,
                                          guint16 sequence_number_base,
                                          guint32 timestamp_base,
                                          guint32 ssrc);
gboolean gst_rtsp_sink_server_start(GstRTSPSinkServer *server, GError **error);
void gst_rtsp_sink_server_stop(GstRTSPSinkServer *server);

gboolean gst_rtsp_sink_server_set_caps(GstRTSPSinkServer *server, const GstCaps *caps);
gboolean gst_rtsp_sink_server_push_buffer(GstRTSPSinkServer *server, GstBuffer *buffer);
gboolean gst_rtsp_sink_server_pop_buffer(GstRTSPSinkServer *server, GstBuffer **buffer);
void gst_rtsp_sink_server_unlock(GstRTSPSinkServer *server);

G_END_DECLS
