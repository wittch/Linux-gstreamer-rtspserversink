#include "sdp-builder.h"

gchar *
gst_rtsp_sink_build_sdp(const GstRTSPSinkServerConfig *config, const gchar *host)
{
  guint payload_type;
  guint clock_rate;
  const gchar *origin_host;

  if (config == NULL)
    return NULL;

  payload_type = config->rtp.payload_type;
  clock_rate = config->rtp.clock_rate;
  origin_host = (host != NULL && *host != '\0') ? host : "127.0.0.1";

  if (config->codec == GST_RTSP_SINK_CODEC_H265) {
    return g_strdup_printf(
        "v=0\r\n"
        "o=- 0 0 IN IP4 %s\r\n"
        "s=GstRTSPSink\r\n"
        "t=0 0\r\n"
        "a=control:*\r\n"
        "m=video 0 RTP/AVP %u\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=rtpmap:%u H265/%u\r\n"
        "a=control:track0\r\n",
        origin_host,
        payload_type,
        payload_type,
        clock_rate);
  }

  return g_strdup_printf(
      "v=0\r\n"
      "o=- 0 0 IN IP4 %s\r\n"
      "s=GstRTSPSink\r\n"
      "t=0 0\r\n"
      "a=control:*\r\n"
      "m=video 0 RTP/AVP %u\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=rtpmap:%u H264/%u\r\n"
      "a=fmtp:%u packetization-mode=1\r\n"
      "a=control:track0\r\n",
      origin_host,
      payload_type,
      payload_type,
      clock_rate,
      payload_type);
}
