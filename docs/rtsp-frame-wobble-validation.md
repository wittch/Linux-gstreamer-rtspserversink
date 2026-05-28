# RTSP Frame Wobble Validation

This document turns the current investigation plan into an executable checklist.
It focuses on separating control-plane correctness from media-plane faults by
capturing both `pcap` and `GST_DEBUG` logs while reproducing the wobble with
GStreamer and VLC clients.

## Scope

The goal is to decide whether the symptom comes from:

1. stale RTP being replayed at `PLAY`
2. SDP metadata not matching the actual RTP stream
3. TCP interleaved framing errors
4. UDP datagram loss or reordering
5. SSRC / timestamp continuity problems
6. client-specific decoder behavior

## Baseline Reproduction

Use the same upstream pipeline each time so the RTP source stays stable:

```sh
GST_PLUGIN_PATH=$PWD/builddir/plugin \
GST_DEBUG=rtspserversink:6 \
GST_DEBUG_NO_COLOR=1 \
GST_DEBUG_FILE=server.log \
gst-launch-1.0 -q \
  videotestsrc is-live=true ! \
  x264enc tune=zerolatency key-int-max=15 ! \
  h264parse ! \
  rtph264pay pt=96 ! \
  rtspserversink port=9562 path=/live
```

If the issue only shows up with H.265, keep the same structure and swap in the
HEVC encoder/payloader pair.

## Client Matrix

Run one client at a time against the same server instance.

### GStreamer client

```sh
gst-launch-1.0 -v \
  rtspsrc location=rtsp://127.0.0.1:9562/live protocols=tcp latency=0 ! \
  rtph264depay ! \
  h264parse ! \
  avdec_h264 ! \
  autovideosink
```

For UDP transport, drop the `protocols=tcp` setting and keep the rest of the
pipeline unchanged.

### VLC client

```text
rtsp://127.0.0.1:9562/live
```

Use the same stream URI and compare whether VLC shows black video, wobble, or
normal playback.

## Capture Matrix

Capture the server/client exchange during each repro window.

### TCP interleaved

Capture the whole loopback exchange and inspect the RTSP TCP stream:

```sh
tcpdump -i lo -s 0 -w tcp-interleaved.pcap host 127.0.0.1
```

What to verify in the capture:

1. `SETUP` advertises the same interleaved channel pair that the client uses.
2. RTP packets are wrapped in `$ <channel> <16-bit length>`.
3. The length field matches the actual RTP packet size.
4. No stale RTP is sent before the first `PLAY` response.

### UDP

Capture the loopback exchange again, then filter for RTP/RTCP in Wireshark:

```sh
tcpdump -i lo -s 0 -w udp.pcap host 127.0.0.1
```

What to verify in the capture:

1. UDP datagrams stay aligned to whole RTP packets.
2. RTP sequence numbers advance monotonically.
3. RTCP SR/SDES packets are present when UDP transport is used.
4. Packet loss or out-of-order delivery is not visible on loopback.

## Log Signals

The code already emits the key breadcrumbs needed for this investigation.
Search the server log for:

- `rtp-caps configured`
- `rtp-sdp`
- `SETUP complete`
- `PLAY entered`
- `rtp-queue-flush`
- `rtp-recv`
- `rtp-send`

Use these lines to line up control-plane events with the packet trace.

## Hypothesis Checks

### 1. Stale RTP before or at `PLAY`

False if:

- the first packet after `PLAY` has a sequence number and timestamp that match
  the live stream
- the queue flush log shows stale packets were removed before the client joined

True if:

- packets with older sequence numbers or timestamps are still emitted after
  `PLAY`
- the first visible RTP burst predates the `PLAY` response

### 2. SDP mismatch

False if:

- `a=rtpmap` advertises the same payload type and clock rate that appear in the
  actual RTP packets
- `a=fmtp` matches the codec-specific parameters used by the payloader

True if:

- the SDP says one codec or payload type while the pcap shows another
- `clock-rate`, `encoding-name`, or `fmtp` differ from the live RTP caps

### 3. TCP interleaved framing error

False if:

- every interleaved frame starts with `$`
- the channel number matches the `SETUP` response
- the 16-bit length field equals the RTP payload length

True if:

- framing bytes are malformed
- the client and server disagree on the channel pair
- the interleaved length does not match the RTP packet size

### 4. UDP boundary or ordering issue

False if:

- each datagram contains exactly one RTP packet
- no packet loss or reordering appears in the pcap

True if:

- RTP packets are split across datagrams
- packets arrive out of order or are missing

### 5. SSRC continuity problem

False if:

- the same SSRC is seen in the server logs and in the pcap for the duration of
  the session

True if:

- the SSRC changes midstream
- the client joins with a stale SSRC that no longer matches the live sender

### 6. Client decoder incompatibility

False if:

- both GStreamer and VLC fail in the same way

True if:

- one client decodes correctly and the other only shows black video or wobble

## Decision Template

Fill this in after each repro:

| Hypothesis | Result | Evidence |
| --- | --- | --- |
| Stale RTP at `PLAY` | `true / false / unknown` | `server.log`, `pcap` |
| SDP mismatch | `true / false / unknown` | `DESCRIBE` response, `pcap` |
| TCP interleaved framing | `true / false / unknown` | `tcpdump` on TCP stream |
| UDP boundary / ordering | `true / false / unknown` | `udp.pcap` |
| SSRC continuity | `true / false / unknown` | `rtp-recv`, `rtp-send` logs |
| Client compatibility | `true / false / unknown` | GStreamer vs VLC comparison |

## Practical Notes

- Keep the server log and the capture file from the same repro attempt.
- Do not mix GStreamer and VLC in the same capture unless you are explicitly
  checking how the second join behaves.
- If the symptom changes between TCP and UDP, treat that as a transport issue
  first, not a codec issue.
- If the symptom only appears on the second join, focus on stale queue flush and
  `RTP-Info` continuity.
