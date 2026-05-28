#ifndef __RTSP_SINK_SERVER_INTERNAL_H__
#define __RTSP_SINK_SERVER_INTERNAL_H__

#include "../rtspsink-server.h"

#include <gio/gio.h>

#ifndef GST_CAT_DEFAULT
#define GST_CAT_DEFAULT gst_rtsp_sink_debug
#endif

GST_DEBUG_CATEGORY_EXTERN (gst_rtsp_sink_debug);

#define RTP_CLOCK_RATE 90000
#define RTP_PAYLOAD_TYPE 96
#define RTP_MAX_PAYLOAD 1400
#define RTP_QUEUE_MAX_PACKETS 128

typedef enum
{
  GST_RTSP_SINK_TRANSPORT_NONE,
  GST_RTSP_SINK_TRANSPORT_UDP,
  GST_RTSP_SINK_TRANSPORT_TCP_INTERLEAVED,
} GstRTSPSinkTransport;

typedef enum
{
  GST_RTSP_SINK_STATE_INIT,
  GST_RTSP_SINK_STATE_READY,
  GST_RTSP_SINK_STATE_PLAYING,
  GST_RTSP_SINK_STATE_PAUSED,
  GST_RTSP_SINK_STATE_CLOSED,
} GstRTSPSinkSessionState;

typedef struct _GstRTSPSinkRequest
{
  gchar *method;
  gchar *uri;
  gchar *version;
  GHashTable *headers;
  guint content_length;
  guint8 *body;
} GstRTSPSinkRequest;

typedef struct _TransportSetup
{
  GstRTSPSinkTransport transport;
  gboolean is_unicast;
  gboolean mode_play;
  guint8 rtp_channel;
  guint8 rtcp_channel;
  guint16 client_rtp_port;
  guint16 client_rtcp_port;
} TransportSetup;

typedef struct _RtpPacketInfo
{
  const guint8 *data;
  gsize size;
  guint8 payload_type;
  guint32 timestamp;
  guint16 seqnum;
  guint32 ssrc;
  gsize header_len;
  gsize payload_len;
  gboolean marker;
} RtpPacketInfo;

typedef struct _GstRTSPQueuedRtpPacket
{
  guint8 *data;
  gsize size;
  RtpPacketInfo info;
  gboolean have_info;
} GstRTSPQueuedRtpPacket;

typedef struct _GstRTSPSinkClient
{
  struct _GstRTSPSinkServer *server;
  GSocketConnection *connection;
  GDataInputStream *input;
  GOutputStream *output;
  GThread *thread;
  GMutex write_lock;
  gchar *session_id;
  gboolean closed;
  GstRTSPSinkSessionState state;
  GstRTSPSinkTransport transport;
  guint8 rtp_channel;
  guint8 rtcp_channel;
  guint16 client_rtp_port;
  guint16 client_rtcp_port;
  GSocket *server_rtp_socket;
  GSocket *server_rtcp_socket;
  GSocketAddress *client_rtp_addr;
  GSocketAddress *client_rtcp_addr;
  gint64 last_keepalive_monotonic_us;
  guint16 seqnum;
  guint32 ssrc;
  gboolean have_rtp_info;
  guint16 last_seq_sent;
  guint32 last_rtptime;
  guint64 packet_count;
  guint64 octet_count;
  gint64 last_rtcp_monotonic_us;
  gchar *rtcp_cname;
  gboolean rtcp_bye_sent;
} GstRTSPSinkClient;

struct _GstRTSPSinkServer
{
  GMutex lock;
  GCond cond;
  GSocketListener *listener;
  GCancellable *cancellable;
  GThread *accept_thread;
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
  gboolean stopping;
  guint active_pushers;
  GList *clients;
  guint next_session_id;

  GstCaps *rtp_caps;
  gchar *rtp_media;
  gchar *rtp_encoding_name;
  guint rtp_payload_type;
  guint rtp_clock_rate;
  guint32 rtp_ssrc;
  gboolean have_rtp_ssrc;
  gchar *rtp_fmtp;
  gboolean have_latest_rtp;
  guint16 latest_seqnum;
  guint32 latest_rtptime;
  GQueue *warm_start_packets;
  guint warm_start_max_packets;
  GAsyncQueue *rtp_queue;
  GThread *rtp_thread;
  guint rtp_queue_max_packets;
  guint64 rtp_packets_dropped;

  gchar *sdp;
};

GstRTSPSinkClient * gst_rtsp_sink_client_new (GstRTSPSinkServer *server,
    GSocketConnection *connection);
void gst_rtsp_sink_client_free (GstRTSPSinkClient *client);
gboolean gst_rtsp_sink_client_write_bytes (GstRTSPSinkClient *client,
    const guint8 *data, gsize size, gboolean flush);
gboolean gst_rtsp_sink_client_write_text (GstRTSPSinkClient *client,
    const gchar *text);
void gst_rtsp_sink_client_reset_transport (GstRTSPSinkClient *client);
void gst_rtsp_sink_client_touch_keepalive (GstRTSPSinkClient *client);
GSocketAddress * gst_rtsp_sink_client_remote_address_with_port
    (GstRTSPSinkClient *client, guint16 port);
GstRTSPQueuedRtpPacket * gst_rtsp_sink_rtp_queue_packet_new (const guint8 *data,
    gsize size, const RtpPacketInfo *info);
void gst_rtsp_sink_rtp_queue_packet_free (GstRTSPQueuedRtpPacket *packet);

gboolean gst_rtsp_sink_parse_request (GstRTSPSinkClient *client,
    GstRTSPSinkRequest *request);
void gst_rtsp_sink_request_clear (GstRTSPSinkRequest *request);
const gchar * gst_rtsp_sink_request_get_header (const GstRTSPSinkRequest *request,
    const gchar *name);
gboolean gst_rtsp_sink_request_get_cseq (const GstRTSPSinkRequest *request,
    guint *cseq);
gchar * gst_rtsp_sink_extract_path (const gchar *uri);
gboolean gst_rtsp_sink_path_matches (const gchar *request_path,
    const gchar *expected_path);
gboolean gst_rtsp_sink_parse_transport_header (const gchar *transport_header,
    GstRTSPSinkServer *server, TransportSetup *setup);
gchar * gst_rtsp_sink_parse_session_header (const gchar *session_header);
gboolean gst_rtsp_sink_bind_udp_socket_pair (GSocketAddress *remote_address,
    GSocket **rtp_socket, GSocket **rtcp_socket, guint16 *rtp_port,
    guint16 *rtcp_port, GError **error);

gchar * gst_rtsp_sink_handle_request (GstRTSPSinkClient *client,
    const GstRTSPSinkRequest *request);
gchar * gst_rtsp_sink_build_rtsp_url (GstRTSPSinkServer *server);

void gst_rtsp_sink_apply_config (GstRTSPSinkServer *server,
    const GstRTSPSinkServerConfig *config);

void gst_rtsp_sink_sdp_update_unlocked (GstRTSPSinkServer *server);
gboolean gst_rtsp_sink_server_set_rtp_caps_internal (GstRTSPSinkServer *server,
    GstCaps *caps, GError **error);
void gst_rtsp_sink_server_reset_codec_state_unlocked (GstRTSPSinkServer *server);
gboolean gst_rtsp_sink_server_replay_warm_start (GstRTSPSinkServer *server,
    GstRTSPSinkClient *client);
void gst_rtsp_sink_server_broadcast_rtp (GstRTSPSinkServer *server,
    const GstRTSPQueuedRtpPacket *packet);
void gst_rtsp_sink_server_push_buffer_internal (GstRTSPSinkServer *server,
    GstBuffer *buffer);
void gst_rtsp_sink_server_flush_pending_rtp_unlocked (GstRTSPSinkServer *server);
void gst_rtsp_sink_client_maybe_send_rtcp (GstRTSPSinkClient *client,
    gboolean force_sender_report, gboolean send_bye);

#endif
