#include "rtspsink-server.h"

GstRTSPSinkServerConfig *
gst_rtsp_sink_server_config_new(void)
{
  GstRTSPSinkServerConfig *config = g_new0(GstRTSPSinkServerConfig, 1);

  config->port = 8554;
  config->path = g_strdup("/stream");
  config->transport_mode = GST_RTSP_SINK_TRANSPORT_BOTH;
  config->auth_enabled = FALSE;
  config->auth_user = g_strdup("admin");
  config->auth_password = g_strdup("admin");
  config->codec = GST_RTSP_SINK_CODEC_H264;
  config->stream_format = g_strdup("byte-stream");
  config->alignment = g_strdup("au");
  config->rtp.payload_type = 96;
  config->rtp.clock_rate = 90000;
  config->rtp.mtu = 1200;
  config->rtp.marker_per_access_unit = TRUE;
  config->rtp.sequence_number_base = 0;
  config->rtp.timestamp_base = 0;
  config->rtp.ssrc = 0;

  return config;
}

void
gst_rtsp_sink_server_config_free(GstRTSPSinkServerConfig *config)
{
  if (config == NULL)
    return;

  g_free(config->path);
  g_free(config->auth_user);
  g_free(config->auth_password);
  g_free(config->stream_format);
  g_free(config->alignment);
  g_free(config);
}
