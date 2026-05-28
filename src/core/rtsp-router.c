#include "rtsp-router.h"

#include "sdp-builder.h"

#include <string.h>

static const gchar *
gst_rtsp_sink_reason_phrase(guint status_code)
{
  switch (status_code) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 454: return "Session Not Found";
    case 455: return "Method Not Valid in This State";
    case 461: return "Unsupported Transport";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    default: return "Error";
  }
}

static gchar *
gst_rtsp_sink_build_response(guint status_code, guint cseq, const gchar *headers, const gchar *body)
{
  gsize body_len = body != NULL ? strlen(body) : 0;

  if (body != NULL) {
    return g_strdup_printf("RTSP/1.0 %u %s\r\nCSeq: %u\r\nContent-Length: %zu\r\n%s\r\n%s",
                           status_code,
                           gst_rtsp_sink_reason_phrase(status_code),
                           cseq,
                           body_len,
                           headers != NULL ? headers : "",
                           body);
  }

  return g_strdup_printf("RTSP/1.0 %u %s\r\nCSeq: %u\r\n%s\r\n",
                         status_code,
                         gst_rtsp_sink_reason_phrase(status_code),
                         cseq,
                         headers != NULL ? headers : "");
}

static guint
gst_rtsp_sink_request_cseq(const GstRTSPSinkRequest *request)
{
  if (request == NULL || request->cseq == NULL)
    return 0;
  return (guint) g_ascii_strtoull(request->cseq, NULL, 10);
}

static gboolean
gst_rtsp_sink_transport_is_tcp(const gchar *transport)
{
  return transport != NULL &&
         (g_strrstr(transport, "TCP") != NULL || g_strrstr(transport, "interleaved") != NULL);
}

static gboolean
gst_rtsp_sink_request_path_matches(const GstRTSPSinkServerConfig *config,
                                   const GstRTSPSinkRequest *request)
{
  const gchar *path = config != NULL && config->path != NULL ? config->path : "/stream";
  const gchar *uri = request != NULL && request->uri != NULL ? request->uri : "";

  return g_strrstr(uri, path) != NULL;
}

static gboolean
gst_rtsp_sink_request_authorized(const GstRTSPSinkServerConfig *config,
                                 const GstRTSPSinkRequest *request)
{
  gchar *decoded = NULL;
  gchar *expected = NULL;
  gsize decoded_len = 0;
  gboolean ok = FALSE;
  const gchar *user;
  const gchar *password;

  if (config == NULL || !config->auth_enabled)
    return TRUE;

  if (request == NULL || request->authorization == NULL)
    return FALSE;

  if (!g_str_has_prefix(request->authorization, "Basic "))
    return FALSE;

  decoded = (gchar *) g_base64_decode(request->authorization + 6, &decoded_len);
  if (decoded == NULL || decoded_len == 0)
    goto done;

  user = config->auth_user != NULL ? config->auth_user : "admin";
  password = config->auth_password != NULL ? config->auth_password : "admin";
  expected = g_strdup_printf("%s:%s", user, password);
  ok = decoded_len == strlen(expected) && memcmp(decoded, expected, decoded_len) == 0;

done:
  g_free(decoded);
  g_free(expected);
  return ok;
}

static gboolean
gst_rtsp_sink_route_request(GstRTSPSinkServerState state, const gchar *method)
{
  if (method == NULL || state == GST_RTSP_SERVER_STATE_CLOSED)
    return FALSE;

  if (g_strcmp0(method, "OPTIONS") == 0 ||
      g_strcmp0(method, "DESCRIBE") == 0 ||
      g_strcmp0(method, "SETUP") == 0 ||
      g_strcmp0(method, "PLAY") == 0 ||
      g_strcmp0(method, "PAUSE") == 0 ||
      g_strcmp0(method, "TEARDOWN") == 0 ||
      g_strcmp0(method, "GET_PARAMETER") == 0)
    return TRUE;

  return FALSE;
}

static gchar *
gst_rtsp_sink_control_base(const GstRTSPSinkServer *server, const GstRTSPSinkRequest *request)
{
  const GstRTSPSinkServerConfig *config = gst_rtsp_sink_server_get_config(server);
  const gchar *host = request != NULL && request->host != NULL ? request->host : "127.0.0.1";
  guint port = config != NULL ? config->port : 8554;
  const gchar *path = (config != NULL && config->path != NULL) ? config->path : "/stream";

  return g_strdup_printf("rtsp://%s:%u%s/", host, port, path);
}

static gchar *
gst_rtsp_sink_extract_transport_value(const gchar *transport, const gchar *prefix)
{
  const gchar *value;
  gchar *copy;
  gchar *semi;

  if (transport == NULL || prefix == NULL)
    return NULL;

  value = g_strrstr(transport, prefix);
  if (value == NULL)
    return NULL;

  copy = g_strdup(value + strlen(prefix));
  semi = strchr(copy, ';');
  if (semi != NULL)
    *semi = '\0';
  return copy;
}

static gchar *
gst_rtsp_sink_build_transport_header(const GstRTSPSinkServer *server,
                                     GstRTSPSinkClient *client,
                                     const GstRTSPSinkRequest *request)
{
  const GstRTSPSinkServerConfig *config = gst_rtsp_sink_server_get_config(server);
  const GstRTSPSinkRtpConfig *rtp = gst_rtsp_sink_server_get_rtp_config(server);
  gboolean use_tcp = gst_rtsp_sink_transport_is_tcp(request != NULL ? request->transport : NULL);
  guint server_port = gst_rtsp_sink_client_get_udp_server_port(client);
  gchar *client_port = NULL;
  gchar *interleaved = NULL;
  gchar *result = NULL;

  if (use_tcp) {
    interleaved = gst_rtsp_sink_extract_transport_value(request != NULL ? request->transport : NULL,
                                                        "interleaved=");
    if (interleaved == NULL)
      interleaved = g_strdup("0-1");
    result = g_strdup_printf("Transport: RTP/AVP/TCP;unicast;interleaved=%s;mode=play;ssrc=%08x\r\n",
                              interleaved,
                              rtp != NULL ? rtp->ssrc : 0);
    g_free(interleaved);
    return result;
  }

  if (request != NULL && request->transport != NULL) {
    const gchar *client_port_start = g_strstr_len(request->transport, -1, "client_port=");
    if (client_port_start != NULL) {
      client_port = g_strdup(client_port_start + strlen("client_port="));
      gchar *semicolon = strchr(client_port, ';');
      if (semicolon != NULL)
        *semicolon = '\0';
    }
  }

  if (client_port == NULL)
    client_port = g_strdup("5000-5001");

  if (server_port == 0 && config != NULL && config->port != 0)
    server_port = config->port + 2;

  result = g_strdup_printf("Transport: RTP/AVP;unicast;client_port=%s;server_port=%u-%u;mode=play;ssrc=%08x\r\n",
                           client_port,
                           server_port,
                           server_port + 1,
                           rtp != NULL ? rtp->ssrc : 0);
  g_free(client_port);
  return result;
}

static gboolean
gst_rtsp_sink_request_session_matches(const GstRTSPSinkServer *server,
                                      const GstRTSPSinkRequest *request)
{
  const gchar *session = gst_rtsp_sink_server_get_session_id(server);

  if (request == NULL || request->session == NULL || session == NULL)
    return TRUE;

  return g_strcmp0(request->session, session) == 0;
}

gchar *
gst_rtsp_sink_handle_request(GstRTSPSinkServer *server,
                             GstRTSPSinkClient *client,
                             const GstRTSPSinkRequest *request)
{
  guint cseq;
  const gchar *method;
  const GstRTSPSinkServerConfig *config;

  if (server == NULL || request == NULL || request->method == NULL)
    return g_strdup("RTSP/1.0 400 Bad Request\r\nCSeq: 0\r\n\r\n");

  method = request->method;
  config = gst_rtsp_sink_server_get_config(server);
  cseq = gst_rtsp_sink_request_cseq(request);
  if (cseq == 0)
    return g_strdup("RTSP/1.0 400 Bad Request\r\nCSeq: 0\r\n\r\n");

  if (g_strcmp0(method, "OPTIONS") != 0 && !gst_rtsp_sink_request_path_matches(config, request))
    return gst_rtsp_sink_build_response(404, cseq, "", NULL);

  if (g_strcmp0(method, "OPTIONS") != 0 && !gst_rtsp_sink_request_authorized(config, request))
    return gst_rtsp_sink_build_response(401,
                                        cseq,
                                        "WWW-Authenticate: Basic realm=\"GstRTSPSink\"\r\n",
                                        NULL);

  if (g_strcmp0(method, "SETUP") != 0 && !gst_rtsp_sink_request_session_matches(server, request))
    return gst_rtsp_sink_build_response(454, cseq, "", NULL);

  if (!gst_rtsp_sink_route_request(gst_rtsp_sink_server_get_state(server), method))
    return gst_rtsp_sink_build_response(455, cseq, "", NULL);

  if (g_strcmp0(method, "OPTIONS") == 0) {
    return gst_rtsp_sink_build_response(200,
                                        cseq,
                                        "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, GET_PARAMETER\r\n",
                                        NULL);
  }

  if (g_strcmp0(method, "DESCRIBE") == 0) {
    gchar *sdp = gst_rtsp_sink_build_sdp(config, request->host);
    gchar *content_base = gst_rtsp_sink_control_base(server, request);
    gchar *response = g_strdup_printf("Content-Type: application/sdp\r\nContent-Base: %s\r\n",
                                      content_base);
    g_free(content_base);
    gchar *body_response = gst_rtsp_sink_build_response(200, cseq, response, sdp);
    g_free(response);
    g_free(sdp);
    return body_response;
  }

  if (g_strcmp0(method, "SETUP") == 0) {
    gchar *headers;

    if (config != NULL && config->transport_mode == GST_RTSP_SINK_TRANSPORT_UDP && gst_rtsp_sink_transport_is_tcp(request->transport))
      return gst_rtsp_sink_build_response(461, cseq, "", NULL);
    if (config != NULL && config->transport_mode == GST_RTSP_SINK_TRANSPORT_TCP && !gst_rtsp_sink_transport_is_tcp(request->transport))
      return gst_rtsp_sink_build_response(461, cseq, "", NULL);

    headers = gst_rtsp_sink_build_transport_header(server, client, request);
    gchar *session = g_strdup_printf("Session: %s;timeout=60\r\n", gst_rtsp_sink_server_get_session_id(server));
    gchar *response_headers = g_strconcat(headers, session, NULL);
    gchar *response = gst_rtsp_sink_build_response(200, cseq, response_headers, NULL);
    g_free(headers);
    g_free(session);
    g_free(response_headers);
    return response;
  }

  if (g_strcmp0(method, "PLAY") == 0) {
    gchar *base = gst_rtsp_sink_control_base(server, request);
    const GstRTSPSinkRtpConfig *rtp = gst_rtsp_sink_server_get_rtp_config(server);
    gchar *headers = g_strdup_printf("Session: %s\r\nRange: npt=0.000-\r\nRTP-Info: url=%strack0;seq=%u;rtptime=%u\r\n",
                                     gst_rtsp_sink_server_get_session_id(server),
                                     base,
                                     rtp->sequence_number_base,
                                     rtp->timestamp_base);
    g_free(base);
    gchar *response = gst_rtsp_sink_build_response(200, cseq, headers, NULL);
    g_free(headers);
    return response;
  }

  if (g_strcmp0(method, "PAUSE") == 0) {
    gchar *headers = g_strdup_printf("Session: %s\r\n", gst_rtsp_sink_server_get_session_id(server));
    gchar *response = gst_rtsp_sink_build_response(200, cseq, headers, NULL);
    g_free(headers);
    return response;
  }

  if (g_strcmp0(method, "GET_PARAMETER") == 0) {
    gchar *headers = g_strdup_printf("Session: %s\r\n", gst_rtsp_sink_server_get_session_id(server));
    gchar *response = gst_rtsp_sink_build_response(200, cseq, headers, NULL);
    g_free(headers);
    return response;
  }

  if (g_strcmp0(method, "TEARDOWN") == 0) {
    gchar *headers = g_strdup_printf("Session: %s\r\n", gst_rtsp_sink_server_get_session_id(server));
    gchar *response = gst_rtsp_sink_build_response(200, cseq, headers, NULL);
    g_free(headers);
    return response;
  }

  return gst_rtsp_sink_build_response(501, cseq, "", NULL);
}
