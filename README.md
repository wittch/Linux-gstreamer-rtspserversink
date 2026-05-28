# gst-rtsp-server-sink

`rtspserversink` is a `GstBaseSink` element that relays `application/x-rtp` into a single shared RTSP stream directly from a GStreamer pipeline.

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

Inspect the locally built plugin:

```sh
GST_PLUGIN_PATH=$PWD/builddir/plugin gst-inspect-1.0 rtspserversink
```

Rebuild after code changes:

```sh
meson compile -C builddir
```

## How To Use

`rtspserversink` is a sink element. Put it at the end of a normal GStreamer pipeline and feed it `application/x-rtp` from a payloader such as `rtph264pay` or `rtph265pay`.

The common runtime pattern is:

```sh
GST_PLUGIN_PATH=$PWD/builddir/plugin gst-launch-1.0 ...
```

By default, the sink listens on port `8554`. Set `port` and `path` explicitly for predictable RTSP URLs.

## Example: H.264 RTP Relay

Start a shared RTSP stream:

```sh
GST_PLUGIN_PATH=$PWD/builddir/plugin \
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

## Example: H.265 RTP Relay

Software HEVC example:

```sh
GST_PLUGIN_PATH=$PWD/builddir/plugin \
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

## Supported Sink Caps

RTP video payloads:

```text
application/x-rtp, media=video, clock-rate=90000, encoding-name={ H264, H265 }
```

## Hardware Encoder Examples

### VA-API / AMD Example

This path was validated locally with `vah264enc`:

```sh
GST_PLUGIN_PATH=$PWD/builddir/plugin \
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
GST_PLUGIN_PATH=$PWD/builddir/plugin \
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
GST_PLUGIN_PATH=$PWD/builddir/plugin \
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

Always inspect with the build-tree plugin path:

```sh
GST_PLUGIN_PATH=$PWD/builddir/plugin gst-inspect-1.0 rtspserversink
```

If `gst-inspect-1.0 rtspserversink` fails without `GST_PLUGIN_PATH`, you are testing the system environment instead of the local build.

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

## Frame Wobble Investigation

If you are debugging the black-screen or wobble symptom, use the dedicated
validation playbook:

- [RTSP frame wobble validation](docs/rtsp-frame-wobble-validation.md)

To run the repro loop automatically, use the helper script:

```sh
bash scripts/rtsp-wobble-repro.sh --client gst
```
