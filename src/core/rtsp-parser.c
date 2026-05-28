#include "rtsp-parser.h"

#include <string.h>

static void
gst_rtsp_sink_request_set_field(gchar **field, const gchar *value)
{
  g_free(*field);
  *field = value != NULL ? g_strdup(value) : NULL;
}

void
gst_rtsp_sink_request_clear(GstRTSPSinkRequest *request)
{
  if (request == NULL)
    return;

  g_free(request->method);
  g_free(request->uri);
  g_free(request->version);
  g_free(request->cseq);
  g_free(request->session);
  g_free(request->transport);
  g_free(request->authorization);
  g_free(request->content_length);
  g_free(request->host);
  g_free(request->body);
  memset(request, 0, sizeof(*request));
}

gboolean
gst_rtsp_sink_parse_request(const gchar *message, GstRTSPSinkRequest *request)
{
  gchar **lines = NULL;
  gchar **tokens = NULL;
  gboolean ok = FALSE;

  if (message == NULL || request == NULL)
    return FALSE;

  lines = g_strsplit(message, "\r\n", -1);
  if (lines == NULL || lines[0] == NULL)
    goto done;

  tokens = g_strsplit(lines[0], " ", 3);
  if (tokens == NULL || tokens[0] == NULL || tokens[1] == NULL || tokens[2] == NULL)
    goto done;

  gst_rtsp_sink_request_set_field(&request->method, tokens[0]);
  gst_rtsp_sink_request_set_field(&request->uri, tokens[1]);
  gst_rtsp_sink_request_set_field(&request->version, tokens[2]);

  for (guint i = 1; lines[i] != NULL; i++) {
    const gchar *line = lines[i];
    const gchar *colon = strchr(line, ':');

    if (line[0] == '\0')
      break;
    if (colon == NULL)
      continue;

    gchar *name = g_strndup(line, colon - line);
    const gchar *value = g_strstrip((gchar *) colon + 1);

    if (g_ascii_strcasecmp(name, "CSeq") == 0)
      gst_rtsp_sink_request_set_field(&request->cseq, value);
    else if (g_ascii_strcasecmp(name, "Session") == 0)
      gst_rtsp_sink_request_set_field(&request->session, value);
    else if (g_ascii_strcasecmp(name, "Transport") == 0)
      gst_rtsp_sink_request_set_field(&request->transport, value);
    else if (g_ascii_strcasecmp(name, "Authorization") == 0)
      gst_rtsp_sink_request_set_field(&request->authorization, value);
    else if (g_ascii_strcasecmp(name, "Content-Length") == 0)
      gst_rtsp_sink_request_set_field(&request->content_length, value);
    else if (g_ascii_strcasecmp(name, "Host") == 0)
      gst_rtsp_sink_request_set_field(&request->host, value);

    g_free(name);
  }

  ok = TRUE;

done:
  g_strfreev(lines);
  g_strfreev(tokens);
  return ok;
}
