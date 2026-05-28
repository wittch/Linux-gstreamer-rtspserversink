#include "gstrtspsink.h"

#include "rtspsink-server.h"

struct _GstRTSPSink
{
  GstBaseSink parent;

  guint port;
  gchar *path;
  GstRTSPSinkTransportMode transport_mode;
  gboolean auth_enabled;
  GstRTSPSinkCodec codec;
  gchar *stream_format;
  gchar *alignment;

  GstRTSPSinkServer *server;
  GstCaps *configured_caps;
};

G_DEFINE_TYPE(GstRTSPSink, gst_rtsp_sink, GST_TYPE_BASE_SINK)

enum
{
  PROP_0,
  PROP_PORT,
  PROP_PATH,
  PROP_TRANSPORT_MODE,
  PROP_AUTH_ENABLED,
  N_PROPERTIES,
};

static GParamSpec *properties[N_PROPERTIES];
static const GEnumValue transport_mode_values[] = {
  { GST_RTSP_SINK_TRANSPORT_UDP, "GST_RTSP_SINK_TRANSPORT_UDP", "udp" },
  { GST_RTSP_SINK_TRANSPORT_TCP, "GST_RTSP_SINK_TRANSPORT_TCP", "tcp" },
  { GST_RTSP_SINK_TRANSPORT_BOTH, "GST_RTSP_SINK_TRANSPORT_BOTH", "both" },
  { 0, NULL, NULL },
};

static gboolean
gst_rtsp_sink_start(GstBaseSink *sink)
{
  GstRTSPSink *self = GST_RTSP_SINK(sink);

  if (self->server != NULL)
    return TRUE;

  GstRTSPSinkServerConfig *config = gst_rtsp_sink_server_config_new();
  config->port = self->port;
  g_free(config->path);
  config->path = g_strdup(self->path);
  config->transport_mode = self->transport_mode;
  config->auth_enabled = self->auth_enabled;
  config->codec = self->codec;
  g_free(config->stream_format);
  config->stream_format = g_strdup(self->stream_format);
  g_free(config->alignment);
  config->alignment = g_strdup(self->alignment);

  self->server = gst_rtsp_sink_server_new(config);
  gst_rtsp_sink_server_config_free(config);
  if (self->server == NULL) {
    GST_ERROR_OBJECT(self, "failed to create internal RTSP server");
    return FALSE;
  }

  if (!gst_rtsp_sink_server_start(self->server, NULL)) {
    GST_ERROR_OBJECT(self, "failed to start internal RTSP server");
    return FALSE;
  }

  if (self->configured_caps != NULL && !gst_rtsp_sink_server_set_caps(self->server, self->configured_caps)) {
    GST_ERROR_OBJECT(self, "failed to apply pending caps");
    return FALSE;
  }

  return TRUE;
}

static gboolean
gst_rtsp_sink_stop(GstBaseSink *sink)
{
  GstRTSPSink *self = GST_RTSP_SINK(sink);

  if (self->server != NULL) {
    gst_rtsp_sink_server_stop(self->server);
    gst_rtsp_sink_server_free(self->server);
    self->server = NULL;
  }

  g_clear_pointer(&self->configured_caps, gst_caps_unref);
  return TRUE;
}

static gboolean
gst_rtsp_sink_set_caps(GstBaseSink *sink, GstCaps *caps)
{
  GstRTSPSink *self = GST_RTSP_SINK(sink);
  GstRTSPSinkCodec codec = GST_RTSP_SINK_CODEC_H264;
  gchar *stream_format = NULL;
  gchar *alignment = NULL;

  if (!gst_rtsp_sink_validate_input_caps(caps, &codec, &stream_format, &alignment)) {
    gchar *caps_str = caps != NULL ? gst_caps_to_string(caps) : NULL;
    GST_ERROR_OBJECT(self, "unsupported input caps: %s", caps_str != NULL ? caps_str : "(null)");
    g_free(caps_str);
    g_free(stream_format);
    g_free(alignment);
    return FALSE;
  }

  self->codec = codec;
  g_free(self->stream_format);
  self->stream_format = stream_format;
  g_free(self->alignment);
  self->alignment = alignment;

  g_clear_pointer(&self->configured_caps, gst_caps_unref);
  if (caps != NULL)
    self->configured_caps = gst_caps_copy(caps);

  if (self->server != NULL) {
    gboolean ok = gst_rtsp_sink_server_set_caps(self->server, caps);
    if (!ok) {
      GST_ERROR_OBJECT(self, "internal caps validation failed");
      return FALSE;
    }
  }

  return TRUE;
}

static GstFlowReturn
gst_rtsp_sink_render(GstBaseSink *sink, GstBuffer *buffer)
{
  GstRTSPSink *self = GST_RTSP_SINK(sink);

  if (self->server == NULL)
  {
    GST_DEBUG_OBJECT(self, "dropping buffer while server is not running");
    return GST_FLOW_FLUSHING;
  }

  return gst_rtsp_sink_server_push_buffer(self->server, buffer) ? GST_FLOW_OK : GST_FLOW_ERROR;
}

static void
gst_rtsp_sink_set_property(GObject *object,
                           guint prop_id,
                           const GValue *value,
                           GParamSpec *pspec)
{
  GstRTSPSink *self = GST_RTSP_SINK(object);

  switch (prop_id) {
    case PROP_PORT:
      self->port = g_value_get_uint(value);
      break;
    case PROP_PATH:
      g_free(self->path);
      self->path = g_value_dup_string(value);
      break;
    case PROP_TRANSPORT_MODE:
      self->transport_mode = (GstRTSPSinkTransportMode) g_value_get_enum(value);
      break;
    case PROP_AUTH_ENABLED:
      self->auth_enabled = g_value_get_boolean(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void
gst_rtsp_sink_get_property(GObject *object,
                           guint prop_id,
                           GValue *value,
                           GParamSpec *pspec)
{
  GstRTSPSink *self = GST_RTSP_SINK(object);

  switch (prop_id) {
    case PROP_PORT:
      g_value_set_uint(value, self->port);
      break;
    case PROP_PATH:
      g_value_set_string(value, self->path);
      break;
    case PROP_TRANSPORT_MODE:
      g_value_set_enum(value, self->transport_mode);
      break;
    case PROP_AUTH_ENABLED:
      g_value_set_boolean(value, self->auth_enabled);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void
gst_rtsp_sink_finalize(GObject *object)
{
  GstRTSPSink *self = GST_RTSP_SINK(object);

  g_clear_pointer(&self->path, g_free);
  g_clear_pointer(&self->stream_format, g_free);
  g_clear_pointer(&self->alignment, g_free);
  g_clear_pointer(&self->configured_caps, gst_caps_unref);
  g_clear_pointer(&self->server, gst_rtsp_sink_server_free);

  G_OBJECT_CLASS(gst_rtsp_sink_parent_class)->finalize(object);
}

static void
gst_rtsp_sink_class_init(GstRTSPSinkClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GstBaseSinkClass *sink_class = GST_BASE_SINK_CLASS(klass);

  object_class->set_property = gst_rtsp_sink_set_property;
  object_class->get_property = gst_rtsp_sink_get_property;
  object_class->finalize = gst_rtsp_sink_finalize;

  sink_class->start = gst_rtsp_sink_start;
  sink_class->stop = gst_rtsp_sink_stop;
  sink_class->set_caps = gst_rtsp_sink_set_caps;
  sink_class->render = gst_rtsp_sink_render;

  properties[PROP_PORT] =
      g_param_spec_uint("port",
                        "Port",
                        "RTSP listen port",
                        1,
                        G_MAXUINT16,
                        8554,
                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_PATH] =
      g_param_spec_string("path",
                          "Path",
                          "RTSP mount path",
                          "/stream",
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_TRANSPORT_MODE] =
      g_param_spec_enum("transport-mode",
                        "Transport Mode",
                        "RTSP transport mode",
                        g_enum_register_static("GstRTSPSinkTransportMode", transport_mode_values),
                        GST_RTSP_SINK_TRANSPORT_BOTH,
                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_AUTH_ENABLED] =
      g_param_spec_boolean("auth-enabled",
                           "Auth Enabled",
                           "Require RTSP authentication",
                           FALSE,
                           G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties(object_class, N_PROPERTIES, properties);

  gst_element_class_set_static_metadata(GST_ELEMENT_CLASS(klass),
                                        "RTSP Server Sink",
                                        "Sink/Network",
                                        "Custom RTSP server sink",
                                        "Codex");

  GstCaps *caps = gst_caps_from_string(
      "video/x-h264,stream-format=(string){ byte-stream, avc },alignment=(string){ au, nal };"
      "video/x-h265,stream-format=(string){ byte-stream, hvc1 },alignment=(string){ au, nal }");

  gst_element_class_add_pad_template(
      GST_ELEMENT_CLASS(klass),
      gst_pad_template_new("sink", GST_PAD_SINK, GST_PAD_ALWAYS, caps));
  gst_caps_unref(caps);
}

static void
gst_rtsp_sink_init(GstRTSPSink *self)
{
  self->port = 8554;
  self->path = g_strdup("/stream");
  self->transport_mode = GST_RTSP_SINK_TRANSPORT_BOTH;
  self->auth_enabled = FALSE;
  self->codec = GST_RTSP_SINK_CODEC_H264;
  self->stream_format = g_strdup("byte-stream");
  self->alignment = g_strdup("au");
}
