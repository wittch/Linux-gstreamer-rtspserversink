#include "rtsp-server-internal.h"

typedef struct
{
  guint8 profile_idc;
  guint8 constraints;
  guint8 level_idc;
  gboolean have_chroma_format_idc;
  guint chroma_format_idc;
  gboolean have_bit_depths;
  guint bit_depth_luma;
  guint bit_depth_chroma;
} H264SpsSummary;

typedef struct
{
  const guint8 *data;
  gsize size;
  gsize bit;
  gboolean overflow;
} H264BitReader;

static void
bit_reader_init (H264BitReader *reader, const guint8 *data, gsize size)
{
  reader->data = data;
  reader->size = size;
  reader->bit = 0;
  reader->overflow = FALSE;
}

static guint32
bit_reader_get_bits (H264BitReader *reader, guint n_bits)
{
  guint32 value = 0;
  guint i;

  for (i = 0; i < n_bits; i++) {
    gsize byte_index;
    guint bit_index;

    if (reader->bit >= reader->size * 8) {
      reader->overflow = TRUE;
      return value;
    }

    byte_index = reader->bit / 8;
    bit_index = 7 - (reader->bit % 8);
    value = (value << 1) | ((reader->data[byte_index] >> bit_index) & 0x01);
    reader->bit++;
  }

  return value;
}

static guint32
bit_reader_get_ue (H264BitReader *reader)
{
  guint leading_zero_bits = 0;
  guint32 suffix;

  while (TRUE) {
    guint32 bit = bit_reader_get_bits (reader, 1);

    if (reader->overflow)
      return 0;
    if (bit == 1)
      break;
    leading_zero_bits++;
    if (leading_zero_bits > 31) {
      reader->overflow = TRUE;
      return 0;
    }
  }

  if (leading_zero_bits == 0)
    return 0;

  suffix = bit_reader_get_bits (reader, leading_zero_bits);
  if (reader->overflow)
    return 0;

  return ((1U << leading_zero_bits) - 1U) + suffix;
}

static gboolean
profile_has_extended_sps_fields (guint8 profile_idc)
{
  switch (profile_idc) {
    case 44:
    case 83:
    case 86:
    case 100:
    case 110:
    case 118:
    case 122:
    case 128:
    case 134:
    case 135:
    case 138:
    case 139:
    case 244:
      return TRUE;
    default:
      return FALSE;
  }
}

static gboolean
profile_is_high_444_family (guint8 profile_idc)
{
  switch (profile_idc) {
    case 44:
    case 138:
    case 139:
    case 244:
      return TRUE;
    default:
      return FALSE;
  }
}

static gchar *
make_profile_level_id_from_sps (const guint8 *data, gsize size)
{
  if (size < 4)
    return NULL;

  return g_strdup_printf ("%02X%02X%02X", data[1], data[2], data[3]);
}

static void
log_sps_summary (const H264SpsSummary *summary)
{
  if (summary->have_chroma_format_idc) {
    GST_DEBUG ("parsed SPS: profile_idc=0x%02X constraints=0x%02X "
        "level_idc=0x%02X chroma_format_idc=%u bit_depth_luma=%u "
        "bit_depth_chroma=%u",
        summary->profile_idc, summary->constraints, summary->level_idc,
        summary->chroma_format_idc,
        summary->have_bit_depths ? summary->bit_depth_luma : 8,
        summary->have_bit_depths ? summary->bit_depth_chroma : 8);
  } else {
    GST_DEBUG ("parsed SPS: profile_idc=0x%02X constraints=0x%02X "
        "level_idc=0x%02X chroma_format_idc=implicit-4:2:0",
        summary->profile_idc, summary->constraints, summary->level_idc);
  }
}

static gboolean
parse_sps_summary (const guint8 *data, gsize size, H264SpsSummary *summary)
{
  g_autoptr(GByteArray) rbsp = NULL;
  H264BitReader reader;
  gsize i;
  guint zero_count = 0;

  if (summary == NULL || size < 4)
    return FALSE;

  memset (summary, 0, sizeof (*summary));
  summary->profile_idc = data[1];
  summary->constraints = data[2];
  summary->level_idc = data[3];

  rbsp = g_byte_array_new ();
  for (i = 1; i < size; i++) {
    guint8 byte = data[i];

    if (zero_count == 2 && byte == 0x03) {
      zero_count = 0;
      continue;
    }

    g_byte_array_append (rbsp, &byte, 1);
    if (byte == 0x00)
      zero_count++;
    else
      zero_count = 0;
  }

  if (rbsp->len < 4)
    return FALSE;

  bit_reader_init (&reader, rbsp->data + 3, rbsp->len - 3);
  (void) bit_reader_get_ue (&reader);
  if (reader.overflow)
    return FALSE;

  if (profile_has_extended_sps_fields (summary->profile_idc)) {
    summary->have_chroma_format_idc = TRUE;
    summary->chroma_format_idc = bit_reader_get_ue (&reader);
    if (reader.overflow)
      return FALSE;

    if (summary->chroma_format_idc == 3)
      (void) bit_reader_get_bits (&reader, 1);
    if (reader.overflow)
      return FALSE;

    summary->have_bit_depths = TRUE;
    summary->bit_depth_luma = bit_reader_get_ue (&reader) + 8;
    summary->bit_depth_chroma = bit_reader_get_ue (&reader) + 8;
    if (reader.overflow)
      return FALSE;
  }

  return TRUE;
}

static void
maybe_warn_about_vlc_compatibility (const H264SpsSummary *summary)
{
  if (summary->have_chroma_format_idc && summary->chroma_format_idc != 1) {
    GST_WARNING ("H264 SPS is not 4:2:0: profile_idc=0x%02X "
        "chroma_format_idc=%u bit_depth_luma=%u bit_depth_chroma=%u; "
        "some VLC builds may decode without displaying video",
        summary->profile_idc, summary->chroma_format_idc,
        summary->have_bit_depths ? summary->bit_depth_luma : 8,
        summary->have_bit_depths ? summary->bit_depth_chroma : 8);
  }

  if (profile_is_high_444_family (summary->profile_idc)) {
    GST_WARNING ("H264 SPS is in the high-4:4:4 profile family "
        "(profile_idc=0x%02X); some VLC builds may decode without "
        "displaying video",
        summary->profile_idc);
  }
}

static gboolean
store_parameter_set (GByteArray **dst, const guint8 *data, gsize size,
    const gchar *label, gboolean log_mismatch)
{
  if (*dst != NULL && (*dst)->len == size &&
      memcmp ((*dst)->data, data, size) == 0)
    return FALSE;

  if (log_mismatch && *dst != NULL && (*dst)->len > 0) {
    GST_WARNING ("%s updated from live stream; replacing previous value "
        "(old_len=%u new_len=%" G_GSIZE_FORMAT ")",
        label, (*dst)->len, size);
  }

  if (*dst == NULL)
    *dst = g_byte_array_new ();
  else
    g_byte_array_set_size (*dst, 0);

  g_byte_array_append (*dst, data, size);
  return TRUE;
}

static void
refresh_h264_sdp_metadata_unlocked (GstRTSPSinkServer *server)
{
  gchar *sps_b64 = NULL;
  gchar *pps_b64 = NULL;

  g_clear_pointer (&server->h264_profile_level_id, g_free);
  g_clear_pointer (&server->h264_sprop_parameter_sets, g_free);

  if (server->h264_sps != NULL && server->h264_sps->len > 0)
    server->h264_profile_level_id = make_profile_level_id_from_sps
        (server->h264_sps->data, server->h264_sps->len);

  if (server->h264_sps == NULL || server->h264_sps->len == 0 ||
      server->h264_pps == NULL || server->h264_pps->len == 0) {
    GST_DEBUG ("H264 SDP metadata incomplete: sps_len=%u pps_len=%u "
        "profile-level-id=%s",
        server->h264_sps != NULL ? server->h264_sps->len : 0,
        server->h264_pps != NULL ? server->h264_pps->len : 0,
        GST_STR_NULL (server->h264_profile_level_id));
    return;
  }

  sps_b64 = g_base64_encode (server->h264_sps->data, server->h264_sps->len);
  pps_b64 = g_base64_encode (server->h264_pps->data, server->h264_pps->len);
  server->h264_sprop_parameter_sets = g_strdup_printf ("%s,%s", sps_b64,
      pps_b64);
  g_free (sps_b64);
  g_free (pps_b64);

  GST_DEBUG ("H264 SDP metadata refreshed: sps_len=%u pps_len=%u "
      "profile-level-id=%s sprop-parameter-sets=%s",
      server->h264_sps->len, server->h264_pps->len,
      GST_STR_NULL (server->h264_profile_level_id),
      GST_STR_NULL (server->h264_sprop_parameter_sets));
}

static void
refresh_h265_sdp_metadata_unlocked (GstRTSPSinkServer *server)
{
  g_clear_pointer (&server->h265_sprop_vps, g_free);
  g_clear_pointer (&server->h265_sprop_sps, g_free);
  g_clear_pointer (&server->h265_sprop_pps, g_free);

  if (server->h265_vps != NULL && server->h265_vps->len > 0)
    server->h265_sprop_vps = g_base64_encode (server->h265_vps->data,
        server->h265_vps->len);
  if (server->h265_sps != NULL && server->h265_sps->len > 0)
    server->h265_sprop_sps = g_base64_encode (server->h265_sps->data,
        server->h265_sps->len);
  if (server->h265_pps != NULL && server->h265_pps->len > 0)
    server->h265_sprop_pps = g_base64_encode (server->h265_pps->data,
        server->h265_pps->len);

  GST_DEBUG ("H265 SDP metadata refreshed: vps_len=%u sps_len=%u pps_len=%u "
      "sprop-vps=%s sprop-sps=%s sprop-pps=%s",
      server->h265_vps != NULL ? server->h265_vps->len : 0,
      server->h265_sps != NULL ? server->h265_sps->len : 0,
      server->h265_pps != NULL ? server->h265_pps->len : 0,
      GST_STR_NULL (server->h265_sprop_vps),
      GST_STR_NULL (server->h265_sprop_sps),
      GST_STR_NULL (server->h265_sprop_pps));
}

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
build_rtp_fmtp_from_caps (GstCaps *caps)
{
  const GstStructure *s;
  GString *fmtp;
  gint i;

  if (caps == NULL || gst_caps_is_empty (caps))
    return g_strdup ("");

  s = gst_caps_get_structure (caps, 0);
  fmtp = g_string_new ("");

  for (i = 0; i < gst_structure_n_fields (s); i++) {
    const gchar *field_name = gst_structure_nth_field_name (s, i);
    const GValue *value;
    gchar *serialized;

    if (field_name == NULL || is_ignored_rtp_fmtp_field (field_name))
      continue;

    value = gst_structure_get_value (s, field_name);
    if (value == NULL)
      continue;

    if (G_VALUE_HOLDS_STRING (value))
      serialized = g_strdup (g_value_get_string (value));
    else
      serialized = gst_value_serialize (value);
    if (serialized == NULL)
      continue;

    append_fmtp_field (fmtp, field_name, serialized);
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
  server->rtp_fmtp = fmtp;
  gst_rtsp_sink_sdp_update_unlocked (server);
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
        "a=rtpmap:%u %s/%u\r\n"
        "%s"
        "a=control:stream=0\r\n",
        server->address != NULL ? server->address : "0.0.0.0",
        server->rtp_media, server->rtp_payload_type, server->rtp_payload_type,
        server->rtp_encoding_name, server->rtp_clock_rate, fmtp);
  } else {
    server->sdp = g_strdup_printf (
        "v=0\r\n"
        "o=- 0 0 IN IP4 %s\r\n"
        "s=GStreamer RTSP Sink\r\n"
        "t=0 0\r\n"
        "a=control:*\r\n"
        "m=video 0 RTP/AVP %u\r\n"
        "a=control:stream=0\r\n",
        server->address != NULL ? server->address : "0.0.0.0",
        server->rtp_payload_type != 0 ? server->rtp_payload_type :
        RTP_PAYLOAD_TYPE);
    fmtp = g_strdup ("");
  }

  g_free (fmtp);
}

static void
note_h264_nal_unlocked (GstRTSPSinkServer *server, const guint8 *nal,
    gsize nal_size)
{
  guint8 nal_type;

  if (nal_size == 0)
    return;

  nal_type = nal[0] & 0x1f;
  if (nal_type == 7) {
    H264SpsSummary summary;

    if (store_parameter_set (&server->h264_sps, nal, nal_size, "H264 SPS",
            TRUE)) {
      refresh_h264_sdp_metadata_unlocked (server);
      if (parse_sps_summary (nal, nal_size, &summary)) {
        log_sps_summary (&summary);
        maybe_warn_about_vlc_compatibility (&summary);
      } else {
        GST_DEBUG ("unable to parse SPS RBSP details for diagnostics "
            "(len=%" G_GSIZE_FORMAT ")", nal_size);
      }
      gst_rtsp_sink_sdp_update_unlocked (server);
    }
  } else if (nal_type == 8) {
    if (store_parameter_set (&server->h264_pps, nal, nal_size, "H264 PPS",
            TRUE)) {
      refresh_h264_sdp_metadata_unlocked (server);
      gst_rtsp_sink_sdp_update_unlocked (server);
    }
  }
}

static void
note_h265_nal_unlocked (GstRTSPSinkServer *server, const guint8 *nal,
    gsize nal_size)
{
  guint8 nal_type;
  gboolean changed = FALSE;

  if (nal_size < 2)
    return;

  nal_type = (nal[0] >> 1) & 0x3f;
  switch (nal_type) {
    case 32:
      changed = store_parameter_set (&server->h265_vps, nal, nal_size,
          "H265 VPS", TRUE);
      break;
    case 33:
      changed = store_parameter_set (&server->h265_sps, nal, nal_size,
          "H265 SPS", TRUE);
      break;
    case 34:
      changed = store_parameter_set (&server->h265_pps, nal, nal_size,
          "H265 PPS", TRUE);
      break;
    default:
      break;
  }

  if (changed) {
    refresh_h265_sdp_metadata_unlocked (server);
    gst_rtsp_sink_sdp_update_unlocked (server);
  }
}

void
gst_rtsp_sink_server_note_nal_unlocked (GstRTSPSinkServer *server,
    const guint8 *nal, gsize nal_size)
{
  if (server->codec == GST_RTSP_SINK_CODEC_H264)
    note_h264_nal_unlocked (server, nal, nal_size);
  else if (server->codec == GST_RTSP_SINK_CODEC_H265)
    note_h265_nal_unlocked (server, nal, nal_size);
}

void
gst_rtsp_sink_server_note_nal (GstRTSPSinkServer *server, const guint8 *nal,
    gsize nal_size)
{
  g_mutex_lock (&server->lock);
  gst_rtsp_sink_server_note_nal_unlocked (server, nal, nal_size);
  g_mutex_unlock (&server->lock);
}

gboolean
gst_rtsp_sink_parse_avcc_codec_data (GstRTSPSinkServer *server,
    GstBuffer *codec_data)
{
  GstMapInfo map;
  const guint8 *p;
  gsize remaining;
  guint num_sps;
  guint i;
  gboolean stored_first_sps = FALSE;
  guint num_pps = 0;

  if (codec_data == NULL)
    return TRUE;
  if (!gst_buffer_map (codec_data, &map, GST_MAP_READ))
    return FALSE;
  if (map.size < 7 || map.data[0] != 1) {
    gst_buffer_unmap (codec_data, &map);
    return FALSE;
  }

  server->nal_length_size = (map.data[4] & 0x03) + 1;

  p = map.data + 5;
  remaining = map.size - 5;
  num_sps = p[0] & 0x1f;
  p++;
  remaining--;

  for (i = 0; i < num_sps && remaining >= 2; i++) {
    guint16 size = (p[0] << 8) | p[1];

    p += 2;
    remaining -= 2;
    if (remaining < size)
      break;
    if (!stored_first_sps) {
      gst_rtsp_sink_server_note_nal_unlocked (server, p, size);
      stored_first_sps = TRUE;
    } else {
      GST_WARNING ("AVCC codec_data contains multiple SPS entries; "
          "only the first SPS is advertised in SDP");
    }
    p += size;
    remaining -= size;
  }

  if (remaining >= 1) {
    guint j;

    num_pps = p[0];
    p++;
    remaining--;
    for (j = 0; j < num_pps && remaining >= 2; j++) {
      guint16 size = (p[0] << 8) | p[1];

      p += 2;
      remaining -= 2;
      if (remaining < size)
        break;
      gst_rtsp_sink_server_note_nal_unlocked (server, p, size);
      p += size;
      remaining -= size;
    }
  }

  if (server->h264_profile_level_id != NULL) {
    gchar *avcc_profile_level_id = g_strdup_printf ("%02X%02X%02X",
        map.data[1], map.data[2], map.data[3]);

    if (g_strcmp0 (avcc_profile_level_id, server->h264_profile_level_id) != 0) {
      GST_WARNING ("AVCC codec_data header profile-level-id %s does not "
          "match SPS-derived profile-level-id %s; SDP will advertise the "
          "SPS-derived value",
          avcc_profile_level_id, server->h264_profile_level_id);
    } else {
      GST_DEBUG ("AVCC codec_data header profile-level-id matches SPS: %s",
          server->h264_profile_level_id);
    }
    g_free (avcc_profile_level_id);
  } else {
    GST_DEBUG ("AVCC codec_data did not yield an SPS-derived "
        "profile-level-id (sps_count=%u pps_count=%u)",
        num_sps, num_pps);
  }

  gst_buffer_unmap (codec_data, &map);
  return TRUE;
}

static gboolean
hvcc_store_first_array_nal (GstRTSPSinkServer *server, guint8 nal_type,
    const guint8 *data, gsize size, gboolean *seen)
{
  if (*seen) {
    switch (nal_type) {
      case 32:
        GST_WARNING ("HVCC codec_data contains multiple VPS entries; only "
            "the first VPS is advertised in SDP");
        break;
      case 33:
        GST_WARNING ("HVCC codec_data contains multiple SPS entries; only "
            "the first SPS is advertised in SDP");
        break;
      case 34:
        GST_WARNING ("HVCC codec_data contains multiple PPS entries; only "
            "the first PPS is advertised in SDP");
        break;
      default:
        break;
    }
    return TRUE;
  }

  gst_rtsp_sink_server_note_nal_unlocked (server, data, size);
  *seen = TRUE;
  return TRUE;
}

gboolean
gst_rtsp_sink_parse_hvcc_codec_data (GstRTSPSinkServer *server,
    GstBuffer *codec_data)
{
  GstMapInfo map;
  const guint8 *p;
  gsize remaining;
  guint8 num_of_arrays;
  guint i;
  gboolean seen_vps = FALSE;
  gboolean seen_sps = FALSE;
  gboolean seen_pps = FALSE;

  if (codec_data == NULL)
    return TRUE;
  if (!gst_buffer_map (codec_data, &map, GST_MAP_READ))
    return FALSE;
  if (map.size < 23 || map.data[0] != 1) {
    gst_buffer_unmap (codec_data, &map);
    return FALSE;
  }

  GST_DEBUG ("parsing HVCC codec_data (len=%" G_GSIZE_FORMAT ")", map.size);
  server->nal_length_size = (map.data[21] & 0x03) + 1;
  num_of_arrays = map.data[22];
  p = map.data + 23;
  remaining = map.size - 23;

  for (i = 0; i < num_of_arrays; i++) {
    guint8 nal_type;
    guint16 num_nalus;
    guint j;

    if (remaining < 3) {
      gst_buffer_unmap (codec_data, &map);
      return FALSE;
    }

    nal_type = p[0] & 0x3f;
    num_nalus = GST_READ_UINT16_BE (p + 1);
    p += 3;
    remaining -= 3;

    for (j = 0; j < num_nalus; j++) {
      guint16 nal_size;

      if (remaining < 2) {
        gst_buffer_unmap (codec_data, &map);
        return FALSE;
      }
      nal_size = GST_READ_UINT16_BE (p);
      p += 2;
      remaining -= 2;
      if (remaining < nal_size) {
        gst_buffer_unmap (codec_data, &map);
        return FALSE;
      }

      switch (nal_type) {
        case 32:
          hvcc_store_first_array_nal (server, nal_type, p, nal_size, &seen_vps);
          break;
        case 33:
          hvcc_store_first_array_nal (server, nal_type, p, nal_size, &seen_sps);
          break;
        case 34:
          hvcc_store_first_array_nal (server, nal_type, p, nal_size, &seen_pps);
          break;
        default:
          break;
      }

      p += nal_size;
      remaining -= nal_size;
    }
  }

  GST_DEBUG ("HVCC parsed: nal_length_size=%u vps_len=%u sps_len=%u pps_len=%u",
      server->nal_length_size,
      server->h265_vps != NULL ? server->h265_vps->len : 0,
      server->h265_sps != NULL ? server->h265_sps->len : 0,
      server->h265_pps != NULL ? server->h265_pps->len : 0);
  gst_buffer_unmap (codec_data, &map);
  return TRUE;
}
