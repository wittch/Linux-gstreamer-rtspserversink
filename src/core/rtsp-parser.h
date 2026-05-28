#pragma once

#include <gst/gst.h>

typedef struct _GstRTSPSinkRequest
{
  gchar *method;
  gchar *uri;
  gchar *version;
  gchar *cseq;
  gchar *session;
  gchar *transport;
  gchar *authorization;
  gchar *content_length;
  gchar *host;
  gchar *body;
} GstRTSPSinkRequest;

void gst_rtsp_sink_request_clear(GstRTSPSinkRequest *request);
gboolean gst_rtsp_sink_parse_request(const gchar *message, GstRTSPSinkRequest *request);
