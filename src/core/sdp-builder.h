#pragma once

#include "rtspsink-server.h"

gchar *gst_rtsp_sink_build_sdp(const GstRTSPSinkServerConfig *config, const gchar *host);
