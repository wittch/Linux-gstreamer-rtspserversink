#include "gstrtspsink.h"

#include "rtspsink-server.h"

#include <gst/gst.h>

struct _GstRTSPSink
{
  GstBaseSink parent;

  GMutex lock;
  gchar *address;
  guint port;
  gchar *path;
  guint backlog;
  guint max_clients;
  guint latency_ms;
  gboolean drop_slow_clients;
  gchar *auth_mode;
  gchar *username;
  gchar *password;
  gchar *realm;
  gboolean enable_udp;
  gboolean enable_tcp_interleaved;
  gboolean started;
  GstRTSPSinkServer *server;
};

G_DEFINE_TYPE (GstRTSPSink, gst_rtsp_sink, GST_TYPE_BASE_SINK)

enum
{
  PROP_0,
  PROP_ADDRESS,
  PROP_PORT,
  PROP_PATH,
  PROP_BACKLOG,
  PROP_MAX_CLIENTS,
  PROP_LATENCY,
  PROP_DROP_SLOW_CLIENTS,
  PROP_AUTH_MODE,
  PROP_USERNAME,
  PROP_PASSWORD,
  PROP_REALM,
  PROP_ENABLE_UDP,
  PROP_ENABLE_TCP_INTERLEAVED,
  N_PROPERTIES,
};

static GParamSpec *properties[N_PROPERTIES];

static GstStaticPadTemplate sink_template =
GST_STATIC_PAD_TEMPLATE (
    "sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (
        "video/x-h264, "
        "stream-format=(string){ avc, avc3, byte-stream }, "
        "alignment=(string){ au, nal }")
    );

static gboolean
gst_rtsp_sink_validate_path (const gchar * path)
{
  return path != NULL && (path[0] == '\0' || path[0] == '/');
}

static void
gst_rtsp_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstRTSPSink *self = GST_RTSP_SINK (object);

  g_mutex_lock (&self->lock);

  switch (prop_id) {
    case PROP_ADDRESS:
      g_free (self->address);
      self->address = g_value_dup_string (value);
      break;
    case PROP_PORT:
      self->port = g_value_get_uint (value);
      break;
    case PROP_PATH:
      g_free (self->path);
      self->path = g_value_dup_string (value);
      break;
    case PROP_BACKLOG:
      self->backlog = g_value_get_uint (value);
      break;
    case PROP_MAX_CLIENTS:
      self->max_clients = g_value_get_uint (value);
      break;
    case PROP_LATENCY:
      self->latency_ms = g_value_get_uint (value);
      break;
    case PROP_DROP_SLOW_CLIENTS:
      self->drop_slow_clients = g_value_get_boolean (value);
      break;
    case PROP_AUTH_MODE:
      g_free (self->auth_mode);
      self->auth_mode = g_value_dup_string (value);
      break;
    case PROP_USERNAME:
      g_free (self->username);
      self->username = g_value_dup_string (value);
      break;
    case PROP_PASSWORD:
      g_free (self->password);
      self->password = g_value_dup_string (value);
      break;
    case PROP_REALM:
      g_free (self->realm);
      self->realm = g_value_dup_string (value);
      break;
    case PROP_ENABLE_UDP:
      self->enable_udp = g_value_get_boolean (value);
      break;
    case PROP_ENABLE_TCP_INTERLEAVED:
      self->enable_tcp_interleaved = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }

  g_mutex_unlock (&self->lock);
}

static void
gst_rtsp_sink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstRTSPSink *self = GST_RTSP_SINK (object);

  g_mutex_lock (&self->lock);

  switch (prop_id) {
    case PROP_ADDRESS:
      g_value_set_string (value, self->address);
      break;
    case PROP_PORT:
      g_value_set_uint (value, self->port);
      break;
    case PROP_PATH:
      g_value_set_string (value, self->path);
      break;
    case PROP_BACKLOG:
      g_value_set_uint (value, self->backlog);
      break;
    case PROP_MAX_CLIENTS:
      g_value_set_uint (value, self->max_clients);
      break;
    case PROP_LATENCY:
      g_value_set_uint (value, self->latency_ms);
      break;
    case PROP_DROP_SLOW_CLIENTS:
      g_value_set_boolean (value, self->drop_slow_clients);
      break;
    case PROP_AUTH_MODE:
      g_value_set_string (value, self->auth_mode);
      break;
    case PROP_USERNAME:
      g_value_set_string (value, self->username);
      break;
    case PROP_PASSWORD:
      g_value_set_string (value, self->password);
      break;
    case PROP_REALM:
      g_value_set_string (value, self->realm);
      break;
    case PROP_ENABLE_UDP:
      g_value_set_boolean (value, self->enable_udp);
      break;
    case PROP_ENABLE_TCP_INTERLEAVED:
      g_value_set_boolean (value, self->enable_tcp_interleaved);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }

  g_mutex_unlock (&self->lock);
}

static void
gst_rtsp_sink_finalize (GObject * object)
{
  GstRTSPSink *self = GST_RTSP_SINK (object);

  gst_rtsp_sink_server_free (self->server);
  g_clear_pointer (&self->address, g_free);
  g_clear_pointer (&self->path, g_free);
  g_clear_pointer (&self->auth_mode, g_free);
  g_clear_pointer (&self->username, g_free);
  g_clear_pointer (&self->password, g_free);
  g_clear_pointer (&self->realm, g_free);
  g_mutex_clear (&self->lock);

  G_OBJECT_CLASS (gst_rtsp_sink_parent_class)->finalize (object);
}

static gboolean
gst_rtsp_sink_start (GstBaseSink * base_sink)
{
  GstRTSPSink *self = GST_RTSP_SINK (base_sink);
  GstRTSPSinkServerConfig config = { 0, };
  GError *error = NULL;
  gboolean started;

  g_mutex_lock (&self->lock);
  config.address = g_strdup (self->address);
  config.port = self->port;
  config.path = g_strdup (self->path);
  config.backlog = self->backlog;
  config.max_clients = self->max_clients;
  config.latency_ms = self->latency_ms;
  config.drop_slow_clients = self->drop_slow_clients;
  config.auth_mode = g_strdup (self->auth_mode);
  config.username = g_strdup (self->username);
  config.password = g_strdup (self->password);
  config.realm = g_strdup (self->realm);
  config.enable_udp = self->enable_udp;
  config.enable_tcp_interleaved = self->enable_tcp_interleaved;
  g_mutex_unlock (&self->lock);

  started = gst_rtsp_sink_server_start (self->server, &config, &error);
  g_free (config.address);
  g_free (config.path);
  g_free (config.auth_mode);
  g_free (config.username);
  g_free (config.password);
  g_free (config.realm);

  if (!started) {
    GST_ELEMENT_ERROR (self, RESOURCE, OPEN_READ_WRITE,
        ("Failed to start RTSP listener"),
        ("%s", error != NULL ? error->message : "unknown error"));
    g_clear_error (&error);
    return FALSE;
  }

  g_mutex_lock (&self->lock);
  self->started = TRUE;
  g_mutex_unlock (&self->lock);

  return TRUE;
}

static gboolean
gst_rtsp_sink_stop (GstBaseSink * base_sink)
{
  GstRTSPSink *self = GST_RTSP_SINK (base_sink);

  gst_rtsp_sink_server_stop (self->server);

  g_mutex_lock (&self->lock);
  self->started = FALSE;
  g_mutex_unlock (&self->lock);

  return TRUE;
}

static gboolean
gst_rtsp_sink_set_caps (GstBaseSink * base_sink, GstCaps * caps)
{
  GstRTSPSink *self = GST_RTSP_SINK (base_sink);
  GError *error = NULL;

  if (!gst_rtsp_sink_server_set_h264_caps (self->server, caps, &error)) {
    GST_ELEMENT_ERROR (self, STREAM, FORMAT,
        ("Unsupported H264 caps for RTSP sink"),
        ("%s", error != NULL ? error->message : "unknown error"));
    g_clear_error (&error);
    return FALSE;
  }

  GST_INFO_OBJECT (self, "configured caps %" GST_PTR_FORMAT, caps);
  return TRUE;
}

static GstFlowReturn
gst_rtsp_sink_render (GstBaseSink * base_sink, GstBuffer * buffer)
{
  GstRTSPSink *self = GST_RTSP_SINK (base_sink);

  if (G_UNLIKELY (!self->started)) {
    GST_ELEMENT_ERROR (self, RESOURCE, FAILED,
        ("RTSP sink received a buffer before start completed"),
        ("Buffer PTS %" GST_TIME_FORMAT,
            GST_TIME_ARGS (GST_BUFFER_PTS (buffer))));
    return GST_FLOW_ERROR;
  }

  gst_rtsp_sink_server_push_buffer (self->server, buffer);
  return GST_FLOW_OK;
}

static gboolean
gst_rtsp_sink_unlock (GstBaseSink * base_sink)
{
  GstRTSPSink *self = GST_RTSP_SINK (base_sink);

  GST_DEBUG_OBJECT (self, "unlock requested");
  return TRUE;
}

static gboolean
gst_rtsp_sink_unlock_stop (GstBaseSink * base_sink)
{
  GstRTSPSink *self = GST_RTSP_SINK (base_sink);

  GST_DEBUG_OBJECT (self, "unlock stop requested");
  return TRUE;
}

static GstStateChangeReturn
gst_rtsp_sink_change_state (GstElement * element, GstStateChange transition)
{
  GstRTSPSink *self = GST_RTSP_SINK (element);

  if (transition == GST_STATE_CHANGE_NULL_TO_READY) {
    g_mutex_lock (&self->lock);

    if (!gst_rtsp_sink_validate_path (self->path)) {
      g_mutex_unlock (&self->lock);
      GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS,
          ("The RTSP path must be empty or start with '/'"),
          ("Configured path: %s", GST_STR_NULL (self->path)));
      return GST_STATE_CHANGE_FAILURE;
    }

    g_mutex_unlock (&self->lock);
  }

  return GST_ELEMENT_CLASS (gst_rtsp_sink_parent_class)->change_state (element,
      transition);
}

static void
gst_rtsp_sink_class_init (GstRTSPSinkClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBaseSinkClass *base_sink_class = GST_BASE_SINK_CLASS (klass);

  gobject_class->set_property = gst_rtsp_sink_set_property;
  gobject_class->get_property = gst_rtsp_sink_get_property;
  gobject_class->finalize = gst_rtsp_sink_finalize;

  properties[PROP_ADDRESS] =
      g_param_spec_string ("address", "Address",
      "Address to bind the RTSP listener to", "0.0.0.0",
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_PORT] =
      g_param_spec_uint ("port", "Port",
      "TCP port to expose the RTSP listener on", 1, 65535, 8554,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_PATH] =
      g_param_spec_string ("path", "Path",
      "Single RTSP path exported by this element; empty exposes the RTSP root", "",
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_BACKLOG] =
      g_param_spec_uint ("backlog", "Backlog",
      "Listen backlog for pending TCP clients", 1, G_MAXUINT, 16,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_MAX_CLIENTS] =
      g_param_spec_uint ("max-clients", "Max Clients",
      "Maximum number of simultaneous RTSP clients", 1, G_MAXUINT, 16,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_LATENCY] =
      g_param_spec_uint ("latency", "Latency",
      "Internal target latency in milliseconds", 0, G_MAXUINT, 200,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_DROP_SLOW_CLIENTS] =
      g_param_spec_boolean ("drop-slow-clients", "Drop Slow Clients",
      "Disconnect clients that cannot keep up with the stream", TRUE,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_AUTH_MODE] =
      g_param_spec_string ("auth-mode", "Auth Mode",
      "Reserved authentication mode; only 'none' is currently implemented", "none",
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_USERNAME] =
      g_param_spec_string ("username", "Username",
      "Reserved username for future RTSP authentication support", "",
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_PASSWORD] =
      g_param_spec_string ("password", "Password",
      "Reserved password for future RTSP authentication support", "",
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_REALM] =
      g_param_spec_string ("realm", "Realm",
      "Reserved realm for future RTSP authentication support", "GStreamer RTSP Sink",
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_ENABLE_UDP] =
      g_param_spec_boolean ("enable-udp", "Enable UDP",
      "Allow RTP/AVP UDP transport during SETUP", TRUE,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_ENABLE_TCP_INTERLEAVED] =
      g_param_spec_boolean ("enable-tcp-interleaved", "Enable TCP Interleaved",
      "Allow RTP/AVP/TCP interleaved transport during SETUP", TRUE,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (gobject_class, N_PROPERTIES, properties);

  gst_element_class_set_static_metadata (element_class,
      "RTSP sink", "Sink/Network",
      "Publishes a single shared RTSP stream from a pipeline sink",
      "Codex");
  gst_element_class_add_static_pad_template (element_class, &sink_template);
  element_class->change_state = gst_rtsp_sink_change_state;

  base_sink_class->start = gst_rtsp_sink_start;
  base_sink_class->stop = gst_rtsp_sink_stop;
  base_sink_class->set_caps = gst_rtsp_sink_set_caps;
  base_sink_class->render = gst_rtsp_sink_render;
  base_sink_class->unlock = gst_rtsp_sink_unlock;
  base_sink_class->unlock_stop = gst_rtsp_sink_unlock_stop;
}

static void
gst_rtsp_sink_init (GstRTSPSink * self)
{
  g_mutex_init (&self->lock);
  self->address = g_strdup ("0.0.0.0");
  self->port = 8554;
  self->path = g_strdup ("");
  self->backlog = 16;
  self->max_clients = 16;
  self->latency_ms = 200;
  self->drop_slow_clients = TRUE;
  self->auth_mode = g_strdup ("none");
  self->username = g_strdup ("");
  self->password = g_strdup ("");
  self->realm = g_strdup ("GStreamer RTSP Sink");
  self->enable_udp = TRUE;
  self->enable_tcp_interleaved = TRUE;
  self->started = FALSE;
  self->server = gst_rtsp_sink_server_new ();

  gst_base_sink_set_sync (GST_BASE_SINK (self), FALSE);
}
