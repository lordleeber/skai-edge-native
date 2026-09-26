#!/usr/bin/env python3
"""Portable protocol fixture for the smoke script; never used by production."""
import argparse
import json
import pathlib
import signal
import sqlite3
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer

import yaml

parser = argparse.ArgumentParser()
parser.add_argument('--config')
parser.add_argument('--driver', action='store_true')
args = parser.parse_args()
config = yaml.safe_load(pathlib.Path(args.config).read_text()) if args.config else {}
mode = config.get('video', {}).get('rtsp_url', '').split('/')[-1]
started = time.monotonic()
if args.config:
    root = pathlib.Path(config['storage']['database_path']).parent
    snapshot = root / 'snapshot.jpg'
    snapshot.write_bytes(b'fixture snapshot')
    with sqlite3.connect(config['storage']['database_path']) as db:
        db.executescript('CREATE TABLE alerts(id TEXT, snapshot_path TEXT); '
                         'CREATE TABLE detections(alert_id TEXT, class_name TEXT);')
        if mode not in ('no-alert', 'no-durable-alert'):
            db.execute('INSERT INTO alerts VALUES (?, ?)', ('new-alert', str(snapshot)))
            db.execute('INSERT INTO detections VALUES (?, ?)', ('new-alert', 'chair'))


class Handler(BaseHTTPRequestHandler):
    loaded_url = ''
    recording_active = True

    def log_message(self, *_):
        pass

    def do_GET(self):
        if self.path == '/api/v1/status':
            body = {'status': 'running', 'fixture_mode': mode, 'video': {'fps': 25},
                    'detector': {'fps': None if mode == 'no-inference' else 25,
                                 'last_inference_ms': 10}}
        elif self.path == '/api/v1/metrics':
            body = {'rtsp': {'health': 'connected', 'frames_received': 100},
                    'webrtc': {'peers': [{'peer_state': 'connected', 'ice_state': 'completed',
                                         'selected_interface': 'lo', 'bytes_sent': 5000}]}}
        elif self.path == '/api/v1/alerts':
            body = {'items': [] if mode == 'no-alert' else
                    [{'id': 'new-alert', 'snapshot_path': str(snapshot),
                      'detections': [{'class_name': 'chair'}]}]}
        elif self.path == '/api/v1/recordings':
            body = {'active': Handler.recording_active, 'access_units_written': 50,
                    'state': 'recording' if Handler.recording_active else 'stopped'}
        elif self.path == '/health':
            if mode == 'delayed-health' and time.monotonic() - started < 0.5:
                self.respond({'state': 'STARTING'}, 503)
                return
            body = {'state': 'RUNNING'}
        else:
            body = 'SKAI Edge Console'
        self.respond(body)

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get('Content-Length', '0'))) or '{}')
        if args.driver:
            if self.path == '/session':
                value = {'sessionId': 'fixture', 'capabilities': {'browserName': 'firefox'}}
            elif self.path.endswith('/url'):
                Handler.loaded_url = body['url']
                value = None
            elif self.path.endswith('/execute/async'):
                info = json.load(urllib.request.urlopen(Handler.loaded_url + '/api/v1/status'))
                value = {'valid': info.get('fixture_mode') != 'bad-sdp', 'status': 201, 'deleted': True}
            elif self.path.endswith('/execute/sync'):
                info = json.load(urllib.request.urlopen(Handler.loaded_url + '/api/v1/status'))
                value = {'title': 'SKAI Edge Console', 'width': 640, 'height': 480,
                         'time': time.monotonic(), 'frames': 0 if
                         info.get('fixture_mode') == 'no-browser' else int(time.monotonic() * 25)}
            else:
                value = None
            self.respond({'value': value})
        else:
            Handler.recording_active = False
            directory = pathlib.Path(config['recording']['directory'])
            directory.mkdir(parents=True, exist_ok=True)
            (directory / 'fixture.mp4').write_text('broken' if mode == 'bad-media' else 'good')
            self.respond({'state': 'stopped', 'active': False, 'access_units_written': 50})

    def do_DELETE(self):
        self.respond({'value': None})

    def respond(self, body, status=200):
        payload = (json.dumps(body) if isinstance(body, dict) else body).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)


server = HTTPServer(('127.0.0.1', 0), Handler)
print(f'Listening on 127.0.0.1:{server.server_port}' if args.driver else
      f'HTTP server started on port {server.server_port}\nskai-edge ready', flush=True)
if args.config:
    def stop(*_):
        if mode == 'no-shutdown':
            return
        print('skai-edge stopped', flush=True)
        raise SystemExit(0)
    signal.signal(signal.SIGTERM, stop)
server.serve_forever()
