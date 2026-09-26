"""Test-only timestamped RTSP source. Never linked into the production runtime."""
import argparse
import json
import signal
import time
import gi

gi.require_version('Gst', '1.0')
gi.require_version('GstRtspServer', '1.0')
from gi.repository import Gst, GstRtspServer, GLib


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--nonce', type=int, required=True)
    args = parser.parse_args()
    Gst.init(None)
    width, height, fps = 1280, 720, 30
    base = bytes([48, 96, 144]) * width * height
    loop = GLib.MainLoop()
    server = GstRtspServer.RTSPServer.new()
    server.set_address('127.0.0.1')
    server.set_service('0')
    factory = GstRtspServer.RTSPMediaFactory.new()
    factory.set_shared(True)
    factory.set_launch('( appsrc name=clock_source is-live=true format=time block=true '
        'caps=video/x-raw,format=RGB,width=1280,height=720,framerate=30/1 ! '
        'videoconvert ! x264enc tune=zerolatency speed-preset=ultrafast bitrate=2000 '
        'key-int-max=30 ! video/x-h264,profile=constrained-baseline ! '
        'rtph264pay name=pay0 pt=96 config-interval=1 )')

    def configure(_, media):
        source = media.get_element().get_by_name('clock_source')
        start, sequence = time.monotonic(), 0

        def push(appsrc, _length):
            nonlocal sequence
            time.sleep(max(0, start + sequence / fps - time.monotonic()))
            stamp = int(time.time() * 1000)
            payload = bytes([0xA5]) + args.nonce.to_bytes(4, 'big') + stamp.to_bytes(6, 'big')
            checksum = 0
            for byte in payload:
                checksum ^= byte
                for _ in range(8):
                    checksum = ((checksum << 1) ^ (7 if checksum & 128 else 0)) & 255
            payload += bytes([checksum])
            pixels = bytearray(base)
            strip = b''.join(bytes([255 if byte & (1 << bit) else 0]) * 24
                             for byte in payload for bit in range(7, -1, -1))
            for row in range(32):
                offset = row * width * 3
                pixels[offset:offset + len(strip)] = strip
            # A moving box makes frozen frames apparent and avoids a static stream.
            x = (sequence * 8) % (width - 64)
            for row in range(300, 364):
                offset = (row * width + x) * 3
                pixels[offset:offset + 64 * 3] = bytes([224, 224, 224]) * 64
            buffer = Gst.Buffer.new_allocate(None, len(pixels), None)
            buffer.fill(0, bytes(pixels))
            buffer.pts = sequence * Gst.SECOND // fps
            buffer.duration = Gst.SECOND // fps
            sequence += 1
            appsrc.emit('push-buffer', buffer)
        source.connect('need-data', push)
    factory.connect('media-configure', configure)
    server.get_mount_points().add_factory('/profile', factory)
    source_id = server.attach(None)
    if not source_id or server.get_bound_port() <= 0:
        raise RuntimeError('cannot bind profiling RTSP fixture')
    print(json.dumps({'url': 'rtsp://127.0.0.1:' + str(server.get_bound_port()) + '/profile',
                      'width': width, 'height': height, 'fps': fps, 'nonce': args.nonce}), flush=True)
    for sig in [signal.SIGINT, signal.SIGTERM]:
        GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, sig, lambda: (loop.quit(), False)[1])
    loop.run()
    GLib.source_remove(source_id)

if __name__ == '__main__':
    main()
