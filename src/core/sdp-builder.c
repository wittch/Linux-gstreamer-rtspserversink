#include "rtsp-server-internal.h"

static void
append_fmtp_field (GString *fmtp, const gchar *key, const gchar *value)
{
  if (value == NULL)
    return;
  if (fmtp->len > 0)
    g_string_append_c (fmtp, ';');
  g_string_append_printf (fmtp, "%s=%s", key, value);
}

static gboolean
is_ignored_rtp_fmtp_field (const gchar *name)
{
  return g_strcmp0 (name, "media") == 0 ||
      g_strcmp0 (name, "encoding-name") == 0 ||
      g_strcmp0 (name, "payload") == 0 ||
      g_strcmp0 (name, "clock-rate") == 0 ||
      g_strcmp0 (name, "ssrc") == 0 ||
      g_strcmp0 (name, "timestamp-offset") == 0 ||
      g_strcmp0 (name, "seqnum-offset") == 0;
}

static gchar *
serialize_structure_field (const GstStructure *s, const gchar *field_name)
{
  const GValue *value;
  gchar *serialized;

  if (s == NULL || field_name == NULL)
    return NULL;

  value = gst_structure_get_value (s, field_name);
  if (value == NULL)
    return NULL;

  if (G_VALUE_HOLDS_STRING (value))
    serialized = g_strdup (g_value_get_string (value));
  else
    serialized = gst_value_serialize (value);

  if (serialized != NULL && serialized[0] == '\0') {
    g_free (serialized);
    return NULL;
  }

  return serialized;
}

static void
append_fmtp_serialized_field (GString *fmtp, const gchar *key,
    const gchar *value)
{
  if (value == NULL || value[0] == '\0')
    return;
  append_fmtp_field (fmtp, key, value);
}

static gchar *
build_rtp_fmtp_from_caps (GstCaps *caps)
{
  const GstStructure *s;
  const gchar *encoding_name = NULL;
  GString *fmtp;
  gint i;

  if (caps == NULL || gst_caps_is_empty (caps))
    return g_strdup ("");

  s = gst_caps_get_structure (caps, 0);
  encoding_name = gst_structure_get_string (s, "encoding-name");
  fmtp = g_string_new ("");

  if (encoding_name != NULL && g_ascii_strcasecmp (encoding_name, "H264") == 0) {
    const gchar *packetization_mode = serialize_structure_field (s,
        "packetization-mode");
    const gchar *sprop_parameter_sets = serialize_structure_field (s,
        "sprop-parameter-sets");
    const gchar *profile_level_id = serialize_structure_field (s,
        "profile-level-id");

    append_fmtp_serialized_field (fmtp, "packetization-mode",
        packetization_mode != NULL ? packetization_mode : "1");
    append_fmtp_serialized_field (fmtp, "profile-level-id",
        profile_level_id);
    append_fmtp_serialized_field (fmtp, "sprop-parameter-sets",
        sprop_parameter_sets);

    g_free ((gchar *) packetization_mode);
    g_free ((gchar *) sprop_parameter_sets);
    g_free ((gchar *) profile_level_id);
  } else if (encoding_name != NULL &&
      g_ascii_strcasecmp (encoding_name, "H265") == 0) {
    const gchar *sprop_vps = serialize_structure_field (s, "sprop-vps");
    const gchar *sprop_sps = serialize_structure_field (s, "sprop-sps");
    const gchar *sprop_pps = serialize_structure_field (s, "sprop-pps");
    const gchar *profile_space = serialize_structure_field (s, "profile-space");
    const gchar *tier_flag = serialize_structure_field (s, "tier-flag");
    const gchar *level_id = serialize_structure_field (s, "level-id");
    const gchar *interop_constraints = serialize_structure_field (s,
        "interop-constraints");

    append_fmtp_serialized_field (fmtp, "profile-space", profile_space);
    append_fmtp_serialized_field (fmtp, "tier-flag", tier_flag);
    append_fmtp_serialized_field (fmtp, "level-id", level_id);
    append_fmtp_serialized_field (fmtp, "interop-constraints",
        interop_constraints);
    append_fmtp_serialized_field (fmtp, "sprop-vps", sprop_vps);
    append_fmtp_serialized_field (fmtp, "sprop-sps", sprop_sps);
    append_fmtp_serialized_field (fmtp, "sprop-pps", sprop_pps);

    g_free ((gchar *) sprop_vps);
    g_free ((gchar *) sprop_sps);
    g_free ((gchar *) sprop_pps);
    g_free ((gchar *) profile_space);
    g_free ((gchar *) tier_flag);
    g_free ((gchar *) level_id);
    g_free ((gchar *) interop_constraints);
  }

  for (i = 0; i < gst_structure_n_fields (s); i++) {
    const gchar *field_name = gst_structure_nth_field_name (s, i);
    gchar *serialized;

    if (field_name == NULL || is_ignored_rtp_fmtp_field (field_name) ||
        g_strcmp0 (field_name, "packetization-mode") == 0 ||
        g_strcmp0 (field_name, "profile-level-id") == 0 ||
        g_strcmp0 (field_name, "sprop-parameter-sets") == 0 ||
        g_strcmp0 (field_name, "sprop-vps") == 0 ||
        g_strcmp0 (field_name, "sprop-sps") == 0 ||
        g_strcmp0 (field_name, "sprop-pps") == 0 ||
        g_strcmp0 (field_name, "profile-space") == 0 ||
        g_strcmp0 (field_name, "tier-flag") == 0 ||
        g_strcmp0 (field_name, "level-id") == 0 ||
        g_strcmp0 (field_name, "interop-constraints") == 0)
      continue;

    serialized = serialize_structure_field (s, field_name);
    append_fmtp_serialized_field (fmtp, field_name, serialized);
    g_free (serialized);
  }

  if (fmtp->len == 0) {
    g_string_free (fmtp, TRUE);
    return g_strdup ("");
  }

  return g_string_free (fmtp, FALSE);
}

static gboolean
rtp_caps_get_string (GstCaps *caps, const gchar *field_name, const gchar **out)
{
  const GstStructure *s;

  if (caps == NULL || gst_caps_is_empty (caps) || out == NULL)
    return FALSE;

  s = gst_caps_get_structure (caps, 0);
  *out = gst_structure_get_string (s, field_name);
  return *out != NULL;
}

static guint
rtp_caps_get_uint (GstCaps *caps, const gchar *field_name, guint fallback)
{
  const GstStructure *s;
  guint value = fallback;

  if (caps == NULL || gst_caps_is_empty (caps))
    return fallback;

  s = gst_caps_get_structure (caps, 0);
  if (!gst_structure_get_uint (s, field_name, &value))
    return fallback;

  return value;
}

static gboolean
rtp_caps_get_uint32 (GstCaps *caps, const gchar *field_name, guint32 *out)
{
  const GstStructure *s;
  guint64 value = 0;

  if (caps == NULL || gst_caps_is_empty (caps) || out == NULL)
    return FALSE;

  s = gst_caps_get_structure (caps, 0);
  if (gst_structure_get_uint64 (s, field_name, &value)) {
    if (value > G_MAXUINT32)
      return FALSE;
    *out = (guint32) value;
    return TRUE;
  }

  if (gst_structure_get_uint (s, field_name, (guint *) out))
    return TRUE;

  return FALSE;
}

gboolean
gst_rtsp_sink_server_set_rtp_caps_internal (GstRTSPSinkServer *server,
    GstCaps *caps, GError **error)
{
  const GstStructure *s;
  const gchar *name;
  const gchar *media = NULL;
  const gchar *encoding_name = NULL;
  gchar *fmtp = NULL;

  g_return_val_if_fail (server != NULL, FALSE);
  g_return_val_if_fail (caps != NULL, FALSE);

  s = gst_caps_get_structure (caps, 0);
  name = gst_structure_get_name (s);
  if (g_strcmp0 (name, "application/x-rtp") != 0) {
    g_set_error (error, GST_STREAM_ERROR, GST_STREAM_ERROR_FORMAT,
        "unsupported caps type '%s'", GST_STR_NULL (name));
    return FALSE;
  }

  if (!rtp_caps_get_string (caps, "media", &media) ||
      g_strcmp0 (media, "video") != 0) {
    g_set_error (error, GST_STREAM_ERROR, GST_STREAM_ERROR_FORMAT,
        "application/x-rtp caps must advertise media=video");
    return FALSE;
  }

  if (!rtp_caps_get_string (caps, "encoding-name", &encoding_name) ||
      encoding_name[0] == '\0') {
    g_set_error (error, GST_STREAM_ERROR, GST_STREAM_ERROR_FORMAT,
        "application/x-rtp caps must include encoding-name");
    return FALSE;
  }

  fmtp = build_rtp_fmtp_from_caps (caps);

  g_mutex_lock (&server->lock);
  gst_rtsp_sink_server_reset_codec_state_unlocked (server);
  server->rtp_caps = gst_caps_copy (caps);
  server->rtp_media = g_strdup (media);
  server->rtp_encoding_name = g_strdup (encoding_name);
  server->rtp_payload_type = rtp_caps_get_uint (caps, "payload",
      RTP_PAYLOAD_TYPE);
  server->rtp_clock_rate = rtp_caps_get_uint (caps, "clock-rate",
      RTP_CLOCK_RATE);
  if (rtp_caps_get_uint32 (caps, "ssrc", &server->rtp_ssrc))
    server->have_rtp_ssrc = TRUE;
  server->rtp_fmtp = fmtp;
  gst_rtsp_sink_sdp_update_unlocked (server);
  GST_DEBUG ("rtp-caps configured media=%s encoding-name=%s payload=%u clock-rate=%u fmtp=%s",
      GST_STR_NULL (server->rtp_media), GST_STR_NULL (server->rtp_encoding_name),
      server->rtp_payload_type, server->rtp_clock_rate,
      GST_STR_NULL (server->rtp_fmtp));
  g_mutex_unlock (&server->lock);

  return TRUE;
}

void
gst_rtsp_sink_sdp_update_unlocked (GstRTSPSinkServer *server)
{
  gchar *fmtp = NULL;

  g_clear_pointer (&server->sdp, g_free);

  if (server->rtp_caps != NULL && server->rtp_encoding_name != NULL &&
      server->rtp_media != NULL) {
    if (server->rtp_fmtp != NULL && server->rtp_fmtp[0] != '\0')
      fmtp = g_strdup_printf ("a=fmtp:%u %s\r\n", server->rtp_payload_type,
          server->rtp_fmtp);
    else
      fmtp = g_strdup ("");

    server->sdp = g_strdup_printf (
        "v=0\r\n"
        "o=- 0 0 IN IP4 %s\r\n"
        "s=GStreamer RTSP Sink\r\n"
        "t=0 0\r\n"
        "a=control:*\r\n"
        "m=%s 0 RTP/AVP %u\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=rtpmap:%u %s/%u\r\n"
        "%s"
        "a=control:trackID=0\r\n",
        server->address != NULL ? server->address : "0.0.0.0",
        server->rtp_media, server->rtp_payload_type, server->rtp_payload_type,
        server->rtp_encoding_name, server->rtp_clock_rate, fmtp);
    GST_DEBUG ("rtp-sdp payload=%u rtpmap=%s/%u fmtp=%s",
        server->rtp_payload_type, server->rtp_encoding_name,
        server->rtp_clock_rate, GST_STR_NULL (server->rtp_fmtp));
  } else {
    server->sdp = g_strdup_printf (
        "v=0\r\n"
        "o=- 0 0 IN IP4 %s\r\n"
        "s=GStreamer RTSP Sink\r\n"
        "t=0 0\r\n"
        "a=control:*\r\n"
        "m=video 0 RTP/AVP %u\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=control:trackID=0\r\n",
        server->address != NULL ? server->address : "0.0.0.0",
        server->rtp_payload_type != 0 ? server->rtp_payload_type :
        RTP_PAYLOAD_TYPE);
    fmtp = g_strdup ("");
  }

  g_free (fmtp);
}
