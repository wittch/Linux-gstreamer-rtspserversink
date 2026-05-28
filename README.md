# gst-rtsp-server-sink

Custom GStreamer sink/plugin for RTSP server style streaming.

## What It Supports Now

- `GstBaseSink`-based `rtspserversink`
- Input caps:
  - `video/x-h264`
  - `video/x-h265`
- Transport policy:
  - RTP over UDP
  - RTP over TCP interleaved
- RTSP 1.0 control flow:
  - `OPTIONS`
  - `DESCRIBE`
  - `SETUP`
  - `PLAY`
  - `PAUSE`
  - `TEARDOWN`
  - `GET_PARAMETER`
- TCP listener on the configured `port`
- Per-client RTSP request loop with UDP and TCP interleaved RTP delivery
- Basic auth gate with path validation
- SPS/PPS/VPS and keyframe warm-start caching

## Build

```sh
meson setup builddir
meson compile -C builddir
```

## Inspect

```sh
GST_PLUGIN_PATH=$PWD/builddir/plugin gst-inspect-1.0 rtspserversink
```

## Example

H.264:

```sh
GST_PLUGIN_PATH=$PWD/builddir/plugin \
gst-launch-1.0 -q \
  videotestsrc num-buffers=1 is-live=true ! \
  x264enc tune=zerolatency key-int-max=1 ! \
  h264parse ! \
  rtspserversink port=9562 path=/live
```

H.265:

```sh
GST_PLUGIN_PATH=$PWD/builddir/plugin \
gst-launch-1.0 -q \
  videotestsrc num-buffers=1 is-live=true ! \
  x265enc tune=zerolatency speed-preset=ultrafast ! \
  h265parse ! \
  rtspserversink port=9563 path=/live
```

## Notes

- The implementation currently targets RTSP 1.0 only.
- Audio, multicast, and recording are still out of scope.
- Refer to `PLAN.md` for the remaining expansion checklist.
