#include "gstrtspsink.h"

#include "rtspsink-server.h"

#include <gst/gst.h>
#include <gst/base/gstbasesink.h>

struct _GstRTSPSink
{
  GstBin parent;
  GstElement *video_sink;
  GstElement *audio_sink;
  gboolean sync;
  gboolean async;

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

GST_DEBUG_CATEGORY (gst_rtsp_sink_debug);

G_DEFINE_TYPE (GstRTSPSink, gst_rtsp_sink, GST_TYPE_BIN)

enum
{
  PROP_0,
  PROP_SYNC,
  PROP_ASYNC,
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
        "application/x-rtp, "
        "media=(string)video, "
        "clock-rate=(int)90000, "
        "encoding-name=(string){ H264, H265 }")
    );

static GstStaticPadTemplate audio_template =
GST_STATIC_PAD_TEMPLATE ("audio", GST_PAD_SINK, GST_PAD_REQUEST,
    GST_STATIC_CAPS ("application/x-rtp, media=(string)audio, "
        "encoding-name=(string)OPUS, clock-rate=(int)48000; "
        "application/x-rtp, media=(string)audio, "
        "encoding-name=(string){ PCMA, PCMU }, clock-rate=(int)8000"));

/* One BaseSink per track preserves segment, flushing, preroll and EOS handling.
 * GstBin aggregates the child EOS messages and exports their sink pads. */
typedef struct {
  GstBaseSink parent;
  GstRTSPSinkServer *server;
  guint track_id;
} GstRTSPTrackSink;
typedef struct { GstBaseSinkClass parent; } GstRTSPTrackSinkClass;
G_DEFINE_TYPE (GstRTSPTrackSink, gst_rtsp_track_sink, GST_TYPE_BASE_SINK)

static gboolean
track_set_caps (GstBaseSink *base, GstCaps *caps)
{
  GstRTSPTrackSink *track = (GstRTSPTrackSink *) base;
  GError *error = NULL;
  if (!gst_rtsp_sink_server_set_caps (track->server, track->track_id, caps, &error)) {
    GST_ELEMENT_ERROR (base, STREAM, FORMAT, ("Unsupported RTP caps"),
        ("%s", error != NULL ? error->message : "unknown error"));
    g_clear_error (&error);
    return FALSE;
  }
  return TRUE;
}

static GstFlowReturn
track_render (GstBaseSink *base, GstBuffer *buffer)
{
  GstRTSPTrackSink *track = (GstRTSPTrackSink *) base;
  gint64 presentation_time_us = g_get_real_time ();
  GstClock *clock = gst_element_get_clock (GST_ELEMENT (base));
  GstClockTime running = gst_segment_to_running_time (&base->segment,
      GST_FORMAT_TIME, GST_BUFFER_PTS (buffer));
  if (clock != NULL && GST_CLOCK_TIME_IS_VALID (running)) {
    GstClockTime target = gst_element_get_base_time (GST_ELEMENT (base)) + running;
    GstClockTime now = gst_clock_get_time (clock);
    presentation_time_us += ((gint64) target - (gint64) now) / GST_USECOND;
  }
  gst_clear_object (&clock);
  gst_rtsp_sink_server_push_buffer (track->server, track->track_id, buffer,
      presentation_time_us);
  return GST_FLOW_OK;
}

static GstFlowReturn
track_render_list (GstBaseSink *base, GstBufferList *list)
{
  for (guint i = 0; i < gst_buffer_list_length (list); i++)
    track_render (base, gst_buffer_list_get (list, i));
  return GST_FLOW_OK;
}

static void
gst_rtsp_track_sink_class_init (GstRTSPTrackSinkClass *klass)
{
  GstElementClass *element = GST_ELEMENT_CLASS (klass);
  GstBaseSinkClass *base = GST_BASE_SINK_CLASS (klass);
  GstCaps *caps = gst_caps_from_string ("application/x-rtp");
  GstPadTemplate *templ = gst_pad_template_new ("sink", GST_PAD_SINK,
      GST_PAD_ALWAYS, caps);
  gst_element_class_add_pad_template (element, templ);
  gst_caps_unref (caps);
  gst_element_class_set_static_metadata (element, "RTSP track sink",
      "Sink/Network", "Internal RTP track input", "Codex");
  base->set_caps = track_set_caps;
  base->render = track_render;
  base->render_list = track_render_list;
}

static void
gst_rtsp_track_sink_init (GstRTSPTrackSink *track)
{
  gst_base_sink_set_sync (GST_BASE_SINK (track), FALSE);
}

static GstElement *
add_track (GstRTSPSink *self, guint track_id, GstPadTemplate *templ)
{
  GstRTSPTrackSink *track = g_object_new (gst_rtsp_track_sink_get_type (),
      "name", track_id == 0 ? "video-track" : "audio-track",
      "sync", self->sync, "async", self->async, NULL);
  GstPad *target, *ghost;
  track->server = self->server;
  track->track_id = track_id;
  gst_bin_add (GST_BIN (self), GST_ELEMENT (track));
  target = gst_element_get_static_pad (GST_ELEMENT (track), "sink");
  ghost = gst_ghost_pad_new_from_template (track_id == 0 ? "sink" : "audio",
      target, templ);
  gst_object_unref (target);
  gst_element_add_pad (GST_ELEMENT (self), ghost);
  return GST_ELEMENT (track);
}

static GstPad *
gst_rtsp_sink_request_new_pad (GstElement *element, GstPadTemplate *templ,
    const gchar *name, const GstCaps *caps)
{
  GstRTSPSink *self = GST_RTSP_SINK (element);
  (void) caps;
  if (GST_STATE (self) > GST_STATE_READY || self->audio_sink != NULL ||
      g_strcmp0 (GST_PAD_TEMPLATE_NAME_TEMPLATE (templ), "audio") != 0 ||
      (name != NULL && g_strcmp0 (name, "audio") != 0))
    return NULL;
  self->audio_sink = add_track (self, 1, templ);
  gst_element_sync_state_with_parent (self->audio_sink);
  gst_rtsp_sink_server_enable_audio (self->server, TRUE);
  return gst_element_get_static_pad (element, "audio");
}

static void
gst_rtsp_sink_release_pad (GstElement *element, GstPad *pad)
{
  GstRTSPSink *self = GST_RTSP_SINK (element);
  if (GST_STATE (self) > GST_STATE_READY || self->audio_sink == NULL ||
      g_strcmp0 (GST_PAD_NAME (pad), "audio") != 0)
    return;
  gst_element_set_state (self->audio_sink, GST_STATE_NULL);
  gst_element_remove_pad (element, pad);
  gst_bin_remove (GST_BIN (self), self->audio_sink);
  self->audio_sink = NULL;
  gst_rtsp_sink_server_enable_audio (self->server, FALSE);
}

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
    case PROP_SYNC:
    case PROP_ASYNC:
      if (prop_id == PROP_SYNC)
        self->sync = g_value_get_boolean (value);
      else
        self->async = g_value_get_boolean (value);
      g_object_set_property (G_OBJECT (self->video_sink), pspec->name, value);
      if (self->audio_sink != NULL)
        g_object_set_property (G_OBJECT (self->audio_sink), pspec->name, value);
      break;
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
    case PROP_SYNC:
      g_value_set_boolean (value, self->sync);
      break;
    case PROP_ASYNC:
      g_value_set_boolean (value, self->async);
      break;
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
gst_rtsp_sink_dispose (GObject *object)
{
  GstRTSPSink *self = GST_RTSP_SINK (object);
  if (self->audio_sink != NULL) {
    GstPad *pad = gst_element_get_static_pad (GST_ELEMENT (self), "audio");
    if (pad != NULL) {
      gst_rtsp_sink_release_pad (GST_ELEMENT (self), pad);
      gst_object_unref (pad);
    }
  }
  G_OBJECT_CLASS (gst_rtsp_sink_parent_class)->dispose (object);
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
gst_rtsp_sink_start (GstRTSPSink *self)
{
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
gst_rtsp_sink_stop (GstRTSPSink *self)
{

  gst_rtsp_sink_server_stop (self->server);

  g_mutex_lock (&self->lock);
  self->started = FALSE;
  g_mutex_unlock (&self->lock);

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

  if (transition == GST_STATE_CHANGE_READY_TO_PAUSED && !gst_rtsp_sink_start (self))
    return GST_STATE_CHANGE_FAILURE;
  GstStateChangeReturn result =
      GST_ELEMENT_CLASS (gst_rtsp_sink_parent_class)->change_state (element, transition);
  if (transition == GST_STATE_CHANGE_PAUSED_TO_READY ||
      (transition == GST_STATE_CHANGE_READY_TO_PAUSED && result == GST_STATE_CHANGE_FAILURE))
    gst_rtsp_sink_stop (self);
  return result;
}

static void
gst_rtsp_sink_class_init (GstRTSPSinkClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  GST_DEBUG_CATEGORY_INIT (gst_rtsp_sink_debug, "rtspserversink", 0,
      "RTSP RTP relay sink");

  gobject_class->set_property = gst_rtsp_sink_set_property;
  gobject_class->get_property = gst_rtsp_sink_get_property;
  gobject_class->dispose = gst_rtsp_sink_dispose;
  gobject_class->finalize = gst_rtsp_sink_finalize;

  properties[PROP_SYNC] = g_param_spec_boolean ("sync", "Sync",
      "Synchronize both input tracks to the pipeline clock", FALSE,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[PROP_ASYNC] = g_param_spec_boolean ("async", "Async",
      "Wait for preroll on each input track", TRUE,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
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
      "Relays RTP video and optional audio into a shared RTSP stream",
      "Codex");
  gst_element_class_add_static_pad_template (element_class, &sink_template);
  gst_element_class_add_static_pad_template (element_class, &audio_template);
  element_class->change_state = gst_rtsp_sink_change_state;
  element_class->request_new_pad = gst_rtsp_sink_request_new_pad;
  element_class->release_pad = gst_rtsp_sink_release_pad;


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

  self->async = TRUE;
  self->video_sink = add_track (self, 0,
      gst_element_class_get_pad_template (GST_ELEMENT_GET_CLASS (self), "sink"));
}
