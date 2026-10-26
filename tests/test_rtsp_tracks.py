#!/usr/bin/python3
"""Integration checks: run with /usr/bin/python3 tests/test_rtsp_tracks.py BUILD_PLUGIN."""
import socket
import struct
import sys
import time
import unittest
from pathlib import Path

import gi

gi.require_version('Gst', '1.0')
from gi.repository import Gst

Gst.init(None)
plugin = Path(sys.argv.pop(1) if len(sys.argv) > 1 else
              'builddir/plugin/gstrtspserversink.so').resolve()
Gst.Plugin.load_file(str(plugin))


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


class RTSP:
    def __init__(self, port):
        self.sock = socket.create_connection(('127.0.0.1', port), 3)
        self.sock.settimeout(5)
        self.file = self.sock.makefile('rb')
        self.url = f'rtsp://127.0.0.1:{port}/live'
        self.cseq = 0
        self.session = None

    def exact(self, n):
        data = self.file.read(n)
        assert len(data) == n, (len(data), n)
        return data

    def frame(self):
        first = self.exact(1)
        if first == b'$':
            channel, size = struct.unpack('!BH', self.exact(3))
            return channel, self.exact(size)
        line = first + self.file.readline()
        headers = {}
        while True:
            row = self.file.readline()
            if row == b'\r\n':
                break
            assert row, 'connection closed'
            key, value = row.decode().split(':', 1)
            headers[key.lower()] = value.strip()
        body = self.exact(int(headers.get('content-length', 0)))
        return int(line.split()[1]), (headers, body)

    def request(self, method, suffix='', **headers):
        self.cseq += 1
        if self.session:
            headers.setdefault('Session', self.session)
        rows = [f'{method} {self.url}{suffix} RTSP/1.0', f'CSeq: {self.cseq}']
        rows.extend(f'{k}: {v}' for k, v in headers.items())
        self.sock.sendall(('\r\n'.join(rows) + '\r\n\r\n').encode())
        while True:
            code, result = self.frame()
            if code >= 100:
                break
        if 'session' in result[0]:
            self.session = result[0]['session'].split(';')[0]
        return code, result

    def close(self):
        self.file.close()
        self.sock.close()


class TracksTest(unittest.TestCase):
    def setUp(self):
        self.pipelines = []
        self.clients = []
        self.sockets = []

    def tearDown(self):
        for client in self.clients:
            client.close()
        for sock in self.sockets:
            sock.close()
        for pipe in reversed(self.pipelines):
            pipe.set_state(Gst.State.NULL)

    def pipeline(self, description):
        pipe = Gst.parse_launch(description)
        self.pipelines.append(pipe)
        self.assertNotEqual(pipe.set_state(Gst.State.PLAYING), Gst.StateChangeReturn.FAILURE)
        return pipe

    def check_bus(self, pipe):
        msg = pipe.get_bus().pop_filtered(Gst.MessageType.ERROR)
        if msg:
            self.fail(str(msg.parse_error()))

    def start(self, audio='opus', finite=False):
        port = free_port()
        limit = 'num-buffers=20' if finite else ''
        desc = (f'rtspserversink name=serve address=127.0.0.1 port={port} path=/live '
                f'videotestsrc is-live=true {limit} ! video/x-raw,width=160,height=120 ! '
                'x264enc tune=zerolatency key-int-max=10 ! h264parse ! '
                'rtph264pay pt=96 config-interval=1 ! serve.sink ')
        if audio:
            encode = {'opus': 'opusenc ! rtpopuspay pt=97',
                      'pcma': 'alawenc ! rtppcmapay pt=8',
                      'pcmu': 'mulawenc ! rtppcmupay pt=0'}[audio]
            desc += (f'audiotestsrc is-live=true {limit} ! audioconvert ! audioresample ! '
                     f'{encode} ! serve.audio')
        pipe = self.pipeline(desc)
        client = RTSP(port)
        self.clients.append(client)
        deadline = time.monotonic() + 5
        while True:
            code, (_, body) = client.request('DESCRIBE', Accept='application/sdp')
            if code == 200:
                break
            self.assertEqual(code, 503)
            self.check_bus(pipe)
            self.assertLess(time.monotonic(), deadline)
            time.sleep(.05)
        return pipe, client, body.decode()

    def test_tcp_tracks_and_session(self):
        pipe, client, sdp = self.start()
        self.assertIn('m=video 0 RTP/AVP 96', sdp)
        self.assertIn('a=rtpmap:96 H264/90000', sdp)
        self.assertIn('m=audio 0 RTP/AVP 97', sdp)
        self.assertIn('a=rtpmap:97 OPUS/48000/2', sdp)
        self.assertNotIn('encoding-params=', sdp)
        self.assertEqual(client.request('SETUP', '/trackID=9',
                         Transport='RTP/AVP/TCP;unicast;interleaved=0-1')[0], 404)
        self.assertEqual(client.request('SETUP', '/trackID=0',
                         Transport='RTP/AVP/TCP;unicast;interleaved=4-5')[0], 200)
        session = client.session
        self.assertEqual(client.request('SETUP', '/trackID=1', Session='wrong',
                         Transport='RTP/AVP/TCP;unicast;interleaved=6-7')[0], 454)
        self.assertEqual(client.request('SETUP', '/trackID=1',
                         Transport='RTP/AVP/TCP;unicast;interleaved=4-5')[0], 461)
        self.assertEqual(client.request('SETUP', '/trackID=1',
                         Transport='RTP/AVP/TCP;unicast;interleaved=6-7')[0], 200)
        self.assertEqual(session, client.session)
        self.assertEqual(client.request('PLAY')[0], 200)
        seen = {}
        deadline = time.monotonic() + 5
        while set(seen) != {4, 5, 6, 7}:
            channel, packet = client.frame()
            self.assertIn(channel, (4, 5, 6, 7))
            if channel % 2 == 0:
                self.assertEqual(packet[1] & 127, 96 if channel == 4 else 97)
            else:
                self.assertEqual(packet[1], 200)
                # Validate the entire RTCP compound packet, including SDES length.
                offset = 0
                while offset < len(packet):
                    size = (struct.unpack_from('!H', packet, offset + 2)[0] + 1) * 4
                    self.assertGreaterEqual(size, 4)
                    offset += size
                self.assertEqual(offset, len(packet))
            seen[channel] = packet
            self.assertLess(time.monotonic(), deadline)
        self.assertEqual(seen[5][38:38 + seen[5][37]],
                         seen[7][38:38 + seen[7][37]])
        def ntp(packet):
            sec, fraction = struct.unpack_from('!II', packet, 8)
            return sec + fraction / 2**32
        self.assertLess(abs(ntp(seen[5]) - ntp(seen[7])), 1)
        self.assertNotEqual(seen[4][8:12], seen[6][8:12])
        self.assertEqual(seen[4][8:12], seen[5][4:8])
        self.assertEqual(seen[6][8:12], seen[7][4:8])
        self.assertEqual(client.request('PAUSE')[0], 200)
        self.assertEqual(client.request('PLAY')[0], 200)
        self.assertEqual(client.request('TEARDOWN')[0], 200)
        self.check_bus(pipe)

    def test_udp_tracks(self):
        pipe, client, _ = self.start()
        pairs = []
        for i in range(2):
            pair = []
            for _ in range(2):
                sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                sock.bind(('127.0.0.1', 0))
                sock.settimeout(5)
                self.sockets.append(sock)
                pair.append(sock)
            pairs.append(pair)
            ports = [s.getsockname()[1] for s in pair]
            self.assertEqual(client.request('SETUP', f'/trackID={i}',
                             Transport=f'RTP/AVP;unicast;client_port={ports[0]}-{ports[1]}')[0], 200)
        self.assertEqual(client.request('PLAY')[0], 200)
        for i, (rtp, rtcp) in enumerate(pairs):
            packet = rtp.recv(65535)
            self.assertEqual(packet[1] & 127, 96 + i)
            report = rtcp.recv(65535)
            self.assertEqual(report[1], 200)
            self.assertEqual(packet[8:12], report[4:8])
        self.check_bus(pipe)

    def test_decode_both_transports(self):
        producer, client, _ = self.start()
        for transport in ('tcp', 'udp'):
            with self.subTest(transport=transport):
                receiver = self.pipeline(
                    f'rtspsrc location={client.url} protocols={transport} latency=100 name=src '
                    'src. ! application/x-rtp,media=video ! rtph264depay ! avdec_h264 ! '
                    'fakesink name=video sync=false signal-handoffs=true '
                    'src. ! application/x-rtp,media=audio ! rtpopusdepay ! opusdec ! '
                    'fakesink name=audio sync=false signal-handoffs=true')
                counts = {'video': 0, 'audio': 0}
                def handoff(sink, buffer, pad, name):
                    counts[name] += 1
                for name in counts:
                    receiver.get_by_name(name).connect('handoff', handoff, name)
                deadline = time.monotonic() + 8
                while min(counts.values()) < 10:
                    self.check_bus(receiver)
                    self.check_bus(producer)
                    self.assertLess(time.monotonic(), deadline, counts)
                    time.sleep(.05)
                receiver.set_state(Gst.State.NULL)

    def test_video_only_and_restart(self):
        pipe, client, sdp = self.start(audio=None)
        self.assertNotIn('m=audio', sdp)
        self.assertEqual(client.request('SETUP', '/trackID=1',
                         Transport='RTP/AVP/TCP;unicast;interleaved=2-3')[0], 404)
        client.close()
        self.clients.remove(client)
        pipe.set_state(Gst.State.NULL)
        self.assertNotEqual(pipe.set_state(Gst.State.PLAYING), Gst.StateChangeReturn.FAILURE)
        time.sleep(.2)
        self.check_bus(pipe)

    def test_g711_and_eos(self):
        for codec in ('pcma', 'pcmu'):
            with self.subTest(codec=codec):
                pipe, client, sdp = self.start(audio=codec, finite=True)
                self.assertIn(f'{codec.upper()}/8000', sdp)
                message = pipe.get_bus().timed_pop_filtered(5 * Gst.SECOND,
                    Gst.MessageType.EOS | Gst.MessageType.ERROR)
                self.assertIsNotNone(message)
                self.assertEqual(message.type, Gst.MessageType.EOS)

    def test_stop_with_connected_client(self):
        pipe, client, _ = self.start()
        self.assertEqual(client.request('SETUP', '/trackID=1',
                         Transport='RTP/AVP/TCP;unicast;interleaved=2-3')[0], 200)
        self.assertEqual(client.request('PLAY')[0], 200)
        started = time.monotonic()
        pipe.set_state(Gst.State.NULL)
        self.assertLess(time.monotonic() - started, 3)

    def test_eos_waits_for_audio(self):
        port = free_port()
        pipe = self.pipeline(
            f'rtspserversink name=serve address=127.0.0.1 port={port} '
            'videotestsrc is-live=true num-buffers=3 ! '
            'video/x-raw,width=160,height=120 ! x264enc tune=zerolatency ! '
            'rtph264pay ! serve.sink '
            'audiotestsrc is-live=true num-buffers=60 ! '
            'audioconvert ! opusenc ! rtpopuspay pt=97 ! serve.audio')
        bus = pipe.get_bus()
        self.assertIsNone(bus.timed_pop_filtered(400 * Gst.MSECOND,
            Gst.MessageType.EOS | Gst.MessageType.ERROR))
        message = bus.timed_pop_filtered(5 * Gst.SECOND,
            Gst.MessageType.EOS | Gst.MessageType.ERROR)
        self.assertIsNotNone(message)
        self.assertEqual(message.type, Gst.MessageType.EOS)

    def test_request_pad_lifecycle(self):
        sink = Gst.ElementFactory.make('rtspserversink')
        pad = sink.request_pad_simple('audio')
        self.assertIsNotNone(pad)
        sink.release_request_pad(pad)
        self.assertIsNone(sink.get_static_pad('audio'))
        pad = sink.request_pad_simple('audio')
        self.assertIsNotNone(pad)
        sink.release_request_pad(pad)


if __name__ == '__main__':
    unittest.main(verbosity=2)
