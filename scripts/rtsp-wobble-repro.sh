#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: rtsp-wobble-repro.sh [options]

Options:
  --codec h264|h265         Upstream codec pipeline to run (default: h264)
  --client none|gst|vlc     Client to launch after the server starts (default: none)
  --transport tcp|udp       Client transport to use when launching gst (default: tcp)
  --address HOST            RTSP bind address and client host (default: 127.0.0.1)
  --port PORT               RTSP port (default: 9562)
  --path PATH               RTSP path (default: /live)
  --plugin-path PATH        GST_PLUGIN_PATH to use (default: $PWD/builddir/plugin)
  --pcap FILE               Write tcpdump output to FILE (default: ./rtsp-wobble.pcap)
  --server-log FILE         Write server stdout/stderr to FILE (default: ./rtsp-wobble-server.log)
  --client-log FILE         Write client stdout/stderr to FILE (default: ./rtsp-wobble-client.log)
  --no-capture              Skip tcpdump capture
  -h, --help                Show this help

Examples:
  ./scripts/rtsp-wobble-repro.sh --client gst
  ./scripts/rtsp-wobble-repro.sh --client vlc --transport udp
EOF
}

codec="h264"
client="none"
transport="tcp"
address="127.0.0.1"
port="9562"
path="/live"
plugin_path="${PWD}/builddir/plugin"
pcap_file="${PWD}/rtsp-wobble.pcap"
server_log="${PWD}/rtsp-wobble-server.log"
client_log="${PWD}/rtsp-wobble-client.log"
capture=1

while (($# > 0)); do
  case "$1" in
    --codec)
      codec="${2:-}"
      shift 2
      ;;
    --client)
      client="${2:-}"
      shift 2
      ;;
    --transport)
      transport="${2:-}"
      shift 2
      ;;
    --address)
      address="${2:-}"
      shift 2
      ;;
    --port)
      port="${2:-}"
      shift 2
      ;;
    --path)
      path="${2:-}"
      shift 2
      ;;
    --plugin-path)
      plugin_path="${2:-}"
      shift 2
      ;;
    --pcap)
      pcap_file="${2:-}"
      shift 2
      ;;
    --server-log)
      server_log="${2:-}"
      shift 2
      ;;
    --client-log)
      client_log="${2:-}"
      shift 2
      ;;
    --no-capture)
      capture=0
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

rtsp_url="rtsp://${address}:${port}${path}"
server_pid=""
tcpdump_pid=""
client_pid=""

cleanup() {
  local exit_code=$?

  if [[ -n "${client_pid}" ]] && kill -0 "${client_pid}" 2>/dev/null; then
    kill "${client_pid}" 2>/dev/null || true
  fi
  if [[ -n "${tcpdump_pid}" ]] && kill -0 "${tcpdump_pid}" 2>/dev/null; then
    kill "${tcpdump_pid}" 2>/dev/null || true
  fi
  if [[ -n "${server_pid}" ]] && kill -0 "${server_pid}" 2>/dev/null; then
    kill "${server_pid}" 2>/dev/null || true
  fi

  if [[ -n "${client_pid}" ]]; then
    wait "${client_pid}" 2>/dev/null || true
  fi
  if [[ -n "${tcpdump_pid}" ]]; then
    wait "${tcpdump_pid}" 2>/dev/null || true
  fi
  if [[ -n "${server_pid}" ]]; then
    wait "${server_pid}" 2>/dev/null || true
  fi

  exit "${exit_code}"
}

trap cleanup EXIT INT TERM

wait_for_port() {
  local attempt
  for attempt in $(seq 1 100); do
    if (exec 3<>"/dev/tcp/${address}/${port}") 2>/dev/null; then
      exec 3<&-
      exec 3>&-
      return 0
    fi
    sleep 0.1
  done
  return 1
}

server_pipeline=()
case "${codec}" in
  h264)
    server_pipeline=(
      gst-launch-1.0 -q
      videotestsrc is-live=true !
      x264enc tune=zerolatency key-int-max=15 !
      h264parse !
      rtph264pay pt=96 !
      rtspserversink port="${port}" path="${path}" address="${address}"
    )
    ;;
  h265)
    server_pipeline=(
      gst-launch-1.0 -q
      videotestsrc is-live=true !
      x265enc tune=zerolatency key-int-max=15 speed-preset=ultrafast !
      h265parse !
      rtph265pay pt=96 !
      rtspserversink port="${port}" path="${path}" address="${address}"
    )
    ;;
  *)
    echo "Unsupported codec: ${codec}" >&2
    exit 2
    ;;
esac

if [[ ! -d "${plugin_path}" ]]; then
  echo "Plugin path does not exist: ${plugin_path}" >&2
  exit 1
fi

if ! env GST_PLUGIN_PATH="${plugin_path}" gst-inspect-1.0 rtspserversink >/dev/null 2>&1; then
  echo "rtspserversink is not loadable from GST_PLUGIN_PATH=${plugin_path}" >&2
  exit 1
fi

: >"${server_log}"
: >"${client_log}"

echo "Server log: ${server_log}"
echo "Client log: ${client_log}"
echo "RTSP URL: ${rtsp_url}"
echo "Verified: rtspserversink loads from ${plugin_path}"

if [[ "${capture}" -eq 1 ]]; then
  if command -v tcpdump >/dev/null 2>&1; then
    tcpdump -i lo -s 0 -U -w "${pcap_file}" host 127.0.0.1 \
      >"${PWD}/rtsp-wobble-tcpdump.log" 2>&1 &
    tcpdump_pid=$!
    echo "PCAP: ${pcap_file}"
  else
    echo "tcpdump not found; continuing without packet capture" >&2
  fi
fi

env GST_PLUGIN_PATH="${plugin_path}" GST_DEBUG=rtspserversink:6 \
  GST_DEBUG_NO_COLOR=1 "${server_pipeline[@]}" >>"${server_log}" 2>&1 &
server_pid=$!

if ! wait_for_port; then
  echo "RTSP server did not open ${address}:${port}" >&2
  exit 1
fi

case "${client}" in
  none)
    echo "Server and capture are running. Press Ctrl-C to stop."
    wait "${server_pid}"
    ;;
  gst)
    client_pipeline=()
    case "${codec}" in
      h264)
        client_pipeline=(
          gst-launch-1.0 -v
          rtspsrc location="${rtsp_url}" protocols="${transport}" latency=0 !
          rtph264depay !
          h264parse !
          avdec_h264 !
          autovideosink
        )
        ;;
      h265)
        client_pipeline=(
          gst-launch-1.0 -v
          rtspsrc location="${rtsp_url}" protocols="${transport}" latency=0 !
          rtph265depay !
          h265parse !
          avdec_h265 !
          autovideosink
        )
        ;;
    esac
    env GST_DEBUG_NO_COLOR=1 "${client_pipeline[@]}" >>"${client_log}" 2>&1 &
    client_pid=$!
    wait "${client_pid}"
    ;;
  vlc)
    if command -v cvlc >/dev/null 2>&1; then
      cvlc --play-and-exit --intf dummy "${rtsp_url}" >>"${client_log}" 2>&1 &
      client_pid=$!
      wait "${client_pid}"
    elif command -v vlc >/dev/null 2>&1; then
      vlc --play-and-exit --intf dummy "${rtsp_url}" >>"${client_log}" 2>&1 &
      client_pid=$!
      wait "${client_pid}"
    else
      echo "VLC not found. Open this URL manually:" >&2
      echo "${rtsp_url}" >&2
      echo "Suggested command:" >&2
      echo "  cvlc --play-and-exit --intf dummy ${rtsp_url}" >&2
      wait "${server_pid}"
    fi
    ;;
  *)
    echo "Unsupported client: ${client}" >&2
    exit 2
    ;;
esac

echo "Done."
echo "Logs:"
echo "  ${server_log}"
if [[ "${capture}" -eq 1 ]]; then
  echo "  ${pcap_file}"
fi
if [[ "${client}" != "none" ]]; then
  echo "  ${client_log}"
fi
