"""Run the actual staged executable against RTSP; keep all writes in DESTDIR."""
import json
import os
import pathlib
import re
import sqlite3
import subprocess
import sys
import time
import urllib.parse
import urllib.request
import yaml

root, rtsp, engine, gpu = sys.argv[1:]
root = pathlib.Path(root)
config = yaml.safe_load((root / 'etc/skai-edge/config.yaml').read_text())
config['video']['rtsp_url'] = rtsp
config['web'].update(bind='127.0.0.1', port=0)
# DESTDIR is staging, not a chroot; map the configured absolute target paths.
for section, field in [('web', 'root'), ('storage', 'database_path'),
                       ('storage', 'alert_directory'), ('recording', 'directory')]:
    config[section][field] = str(root / config[section][field].lstrip('/'))
config['detector']['engine'] = engine
config['webrtc'].update(enabled=False, host_interfaces=[])
config['whip']['enabled'] = False
path = root / 'outside/runtime.yaml'
path.write_text(yaml.safe_dump(config))
log_path = root / 'outside/runtime.log'
environment = dict(os.environ)
environment.pop('LD_LIBRARY_PATH', None)
with log_path.open('w') as log:
    process = subprocess.Popen([str(root / 'usr/local/bin/skai-edge'), '--config', str(path)],
        cwd=root / 'outside', env=environment, stdout=log, stderr=subprocess.STDOUT)
    try:
        deadline = time.monotonic() + 25
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError('installed executable exited: ' + log_path.read_text())
            match = re.search(r'HTTP server started on port (\d+)', log_path.read_text())
            if match:
                base = 'http://127.0.0.1:' + match[1]
                with urllib.request.urlopen(base + '/api/v1/status', timeout=2) as response:
                    status = json.load(response)
                if (status['video']['fps'] or 0) > 0 and (gpu == '0' or (status['detector']['fps'] or 0) > 0):
                    break
            time.sleep(.1)
        else:
            raise RuntimeError('installed RTSP/inference did not become ready: ' + log_path.read_text())
        web = root / 'usr/local/share/skai-edge/web'
        for asset in web.iterdir():
            route = '/' if asset.name == 'index.html' else '/' + asset.name
            with urllib.request.urlopen(base + route, timeout=2) as response:
                assert response.read() == asset.read_bytes(), asset
        process.terminate()
        assert process.wait(timeout=10) == 0
        assert 'skai-edge stopped' in log_path.read_text()
        database = config['storage']['database_path']
        with sqlite3.connect('file:' + urllib.parse.quote(database) + '?mode=ro', uri=True) as db:
            assert db.execute('PRAGMA integrity_check').fetchone()[0] == 'ok'
            assert db.execute('SELECT MAX(version) FROM schema_migrations').fetchone()[0] == 1
        print('PASS installed RTSP, UI assets, SQLite and clean shutdown without LD_LIBRARY_PATH')
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait()
