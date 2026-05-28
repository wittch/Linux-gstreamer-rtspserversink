#pragma once

#include "rtsp-parser.h"
#include "rtsp-server-internal.h"

gchar *gst_rtsp_sink_handle_request(GstRTSPSinkServer *server,
                                    GstRTSPSinkClient *client,
                                    const GstRTSPSinkRequest *request);
