# gst-rtsp-server-sink

`rtspserversink` is a `GstBin` sink element with a `GstBaseSink` child per track that relays `application/x-rtp` into a single shared RTSP stream directly from a GStreamer pipeline.

## Requirements

- Meson
- Ninja
- GStreamer 1.24 development files
- A working local GStreamer runtime with `gst-launch-1.0` and `gst-inspect-1.0`

On Ubuntu-like systems, that usually means the Meson/Ninja toolchain plus the GStreamer base, good, and bad plugin packages.

## Build

Configure and build:

```sh
meson setup builddir
meson compile -C builddir
```

Install for the current user so GStreamer CLI tools find the plugin automatically:

```sh
meson configure builddir -Dpluginsdir="${XDG_DATA_HOME:-$HOME/.local/share}/gstreamer-1.0/plugins"
meson install -C builddir
gst-inspect-1.0 rtspserversink
```

No `GST_PLUGIN_PATH` or `LD_LIBRARY_PATH` export is needed. GStreamer searches
this user plugin directory by default (see the [GStreamer documentation](https://gstreamer.freedesktop.org/documentation/gstreamer/running.html)).

For a system-wide installation instead:

```sh
meson configure builddir -Dpluginsdir=
meson compile -C builddir
sudo meson install -C builddir --no-rebuild
```

The default installation directory comes from GStreamer's pkg-config `pluginsdir`;
`--prefix` alone does not override it. Packagers can set `-Dpluginsdir=/desired/path`
and use Meson's `--destdir` for staging.

Without installing, inspect the build using a CLI option:

```sh
gst-inspect-1.0 --gst-plugin-path="$PWD/builddir/plugin" rtspserversink
```

Rebuild and reinstall after code changes:

```sh
meson compile -C builddir
meson install -C builddir
```

## How To Use

`rtspserversink` is a sink element. Put it at the end of a normal GStreamer pipeline and feed its `sink` pad RTP video from `rtph264pay` or `rtph265pay`.
An optional `audio` request pad accepts RTP Opus or G.711 audio.

The common runtime pattern is:

```sh
gst-launch-1.0 ... ! rtspserversink
```

By default, the sink listens on port `8554`. Set `port` and `path` explicitly for predictable RTSP URLs.

## Basic H.264 Example

Start a shared RTSP stream:

```sh
gst-launch-1.0 -q \
  videotestsrc is-live=true ! \
  x264enc tune=zerolatency key-int-max=15 ! \
  h264parse ! \
  rtph264pay pt=96 ! \
  rtspserversink port=9562 path=/live
```

Open the stream from a client:

```text
rtsp://127.0.0.1:9562/live
```

GStreamer client example:

```sh
gst-launch-1.0 -v \
  rtspsrc location=rtsp://127.0.0.1:9562/live protocols=tcp latency=0 ! \
  rtph264depay ! \
  h264parse ! \
  avdec_h264 ! \
  autovideosink
```

## Basic H.265 Example

Software HEVC example:

```sh
gst-launch-1.0 -q \
  videotestsrc is-live=true ! \
  x265enc tune=zerolatency key-int-max=15 speed-preset=ultrafast ! \
  h265parse ! \
  rtph265pay pt=96 ! \
  rtspserversink port=9562 path=/live
```

GStreamer client example:

```sh
gst-launch-1.0 -v \
  rtspsrc location=rtsp://127.0.0.1:9562/live protocols=tcp latency=0 ! \
  rtph265depay ! \
  h265parse ! \
  avdec_h265 ! \
  autovideosink
```

## Video + Audio Example

Send the ball test pattern and sine-wave audio through one RTSP session:

```sh
gst-launch-1.0 -e \
  rtspserversink name=server port=8554 path=/live \
  videotestsrc pattern=18 is-live=true ! \
    videoconvert ! x264enc tune=zerolatency key-int-max=30 ! \
    h264parse ! rtph264pay pt=96 config-interval=1 ! server.sink \
  audiotestsrc is-live=true wave=sine ! \
    audioconvert ! audioresample ! opusenc ! \
    rtpopuspay pt=97 ! server.audio
```

Open `rtsp://127.0.0.1:8554/live` in a client, or use this GStreamer receiver:

```sh
gst-launch-1.0 -e \
  rtspsrc location=rtsp://127.0.0.1:8554/live protocols=tcp latency=200 name=src \
  src. ! application/x-rtp,media=video ! queue ! \
    rtph264depay ! avdec_h264 ! videoconvert ! autovideosink \
  src. ! application/x-rtp,media=audio ! queue ! \
    rtpopusdepay ! opusdec ! audioconvert ! audioresample ! autoaudiosink
```

Use the server's IP instead of `127.0.0.1` for a remote receiver. Both TCP
interleaving and UDP support both tracks. SDP exposes video as `trackID=0` and
audio as `trackID=1`; RTCP sender reports map both tracks to a common timebase.

For G.711, replace `opusenc ! rtpopuspay pt=97` with
`alawenc ! rtppcmapay pt=8` (PCMA) or `mulawenc ! rtppcmupay pt=0` (PCMU),
and use the matching depayloader/decoder on the receiver.

This version supports one required video track and one optional audio track.
AAC and audio-only presentations are not supported. Create or release the audio
pad in NULL/READY, before streaming; in `gst-launch-1.0`, `server.audio` requests
it automatically. DESCRIBE returns 503 until all requested tracks have negotiated
caps, so start the producer before connecting a receiver.

Existing video-only pipelines retain the `sink` pad. The element now derives from
`GstBin` to handle both tracks' preroll and EOS; its server properties plus `sync`
and `async` remain available. Other inherited `GstBaseSink` properties are no
longer properties of the outer element. `sync=false` remains the default; use
`sync=true` to pace non-live sources against the pipeline clock.

## Supported Sink Caps

RTP video:

```text
application/x-rtp, media=video, clock-rate=90000, encoding-name={ H264, H265 }
```

Audio RTP on the optional `audio` pad:

```text
application/x-rtp, media=audio, clock-rate=48000, encoding-name=OPUS
application/x-rtp, media=audio, clock-rate=8000, encoding-name={ PCMA, PCMU }
```

## Integration Tests

The tests need Python with PyGObject/GStreamer introspection, the `x264enc`,
`opusenc`, G.711, RTP/RTSP and `avdec_h264` plugins. On this Ubuntu host:

```sh
meson compile -C builddir
/usr/bin/python3 tests/test_rtsp_tracks.py builddir/plugin/gstrtspserversink.so
```

They check SDP and clock rates, per-track TCP/UDP RTP and RTCP, session/channel
validation, decoded audio and video on both transports, video-only restart,
request-pad lifecycle, and EOS aggregation. They use loopback ports and fakesinks;
no display or sound device is needed.

## Hardware Encoder Examples

### VA-API / AMD Example

This path was validated locally with `vah264enc`:

```sh
gst-launch-1.0 -q \
  videotestsrc is-live=true ! \
  "video/x-raw,format=NV12" ! \
  vah264enc ! \
  h264parse ! \
  rtph264pay pt=96 ! \
  rtspserversink port=9562 path=/live
```

Notes:

- Quote caps strings if they contain parentheses or if your shell is `zsh`
- `videotestsrc` produces system-memory frames, so this confirms hardware encoding but not a zero-copy path

### NVIDIA CUDA NVENC Example

If `nvcudah264enc` or `nvcudah265enc` works on your machine, prefer those over the older `nvh264enc` / `nvh265enc` elements.

H.264:

```sh
gst-launch-1.0 -q \
  videotestsrc is-live=true ! \
  "video/x-raw,format=NV12,width=1280,height=720,framerate=30/1" ! \
  nvcudah264enc ! \
  h264parse ! \
  rtph264pay pt=96 ! \
  rtspserversink port=9562 path=/live
```

H.265:

```sh
gst-launch-1.0 -q \
  videotestsrc is-live=true ! \
  "video/x-raw,format=NV12,width=1280,height=720,framerate=30/1" ! \
  nvcudah265enc ! \
  h265parse ! \
  rtph265pay pt=96 ! \
  rtspserversink port=9562 path=/live
```

## Troubleshooting

### `zsh: no matches found`

If your shell expands caps strings such as:

```text
video/x-raw(memory:DMABuf),format=NV12
```

quote them:

```sh
"video/x-raw(memory:DMABuf),format=NV12"
```

### `WARNING: erroneous pipeline: syntax error`

Caps must be written as caps, not as standalone assignments:

- Wrong: `! format=NV12 !`
- Right: `! "video/x-raw,format=NV12" !`

Also make sure `\` is the last character on the line. A trailing space after `\` breaks shell line continuation.

### Confirming The Local Plugin Is Being Used

Inspect the installed plugin and check its `Filename` field:

```sh
gst-inspect-1.0 rtspserversink
```

If the element is not found, run the installation steps above. To test a build
without reinstalling, pass `--gst-plugin-path="$PWD/builddir/plugin"` to
`gst-inspect-1.0` or `gst-launch-1.0`.

## Validation Notes

Validated locally on 2026-05-27:

- `GST_PLUGIN_PATH=$PWD/builddir/plugin gst-inspect-1.0 rtspserversink` reports `application/x-rtp, media=video, clock-rate=90000, encoding-name={ H264, H265 }`
- `x264enc ! h264parse ! rtph264pay ! rtspserversink` streamed successfully
- `x265enc ! h265parse ! rtph265pay ! rtspserversink` streamed successfully
- `rtspsrc ! rtph265depay ! h265parse ! avdec_h265` successfully negotiated, depayloaded, parsed, and decoded the HEVC RTSP stream
- `vah264enc ! h264parse ! rtph264pay ! rtspserversink` negotiated successfully on the local AMD VA path

Additional host-specific NVIDIA notes from this machine:

- GPU: `NVIDIA GeForce RTX 5050`
- `nvh264enc` / `nvh265enc` preset initialization failed locally
- Newer `nvcuda*` encoders are the preferred direction for NVIDIA validation on this host
