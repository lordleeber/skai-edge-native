"""Own an isolated skai-edge process and retain evidence for Jetson acceptance."""
import argparse
import fcntl
import ipaddress
import json
import math
import os
import pathlib
import platform
import re
import secrets
import shutil
import signal
import socket
import sqlite3
import subprocess
import time
import urllib.parse
import urllib.request

try:
    import yaml
except ImportError:
    raise SystemExit('PyYAML is required: install python3-yaml for the smoke runner')
from smoke_browser import BrowserProbe


def wait_for(check, timeout, process=None, label='evidence'):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process and process.poll() is not None:
            raise RuntimeError('owned skai-edge process exited before verification')
        try:
            value = check()
            if value:
                return value
        except (OSError, ValueError, KeyError):
            pass
        time.sleep(0.1)
    raise RuntimeError('timed out waiting for ' + label)


def stop_process(process, timeout):
    if process.poll() is not None:
        return process.returncode == 0
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return process.wait() == 0
    try:
        return process.wait(timeout=timeout) == 0
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()
        return False


def prepare_config(source, root, alert_class):
    config = yaml.safe_load(source.read_text())
    if not isinstance(config, dict) or urllib.parse.urlsplit(
            config.get('video', {}).get('rtsp_url', '')).scheme != 'rtsp':
        raise RuntimeError('configuration must provide an RTSP input')
    engine = pathlib.Path(config.get('detector', {}).get('engine', 'models/yolo11s.engine'))
    if not engine.is_file() or engine.stat().st_size == 0:
        raise RuntimeError('configured TensorRT engine is missing or empty')
    if not config.get('webrtc', {}).get('host_interfaces'):
        raise RuntimeError('configure webrtc.host_interfaces for the Jetson LAN')
    config.setdefault('logging', {})['level'] = 'info'
    config.setdefault('web', {}).update(bind='0.0.0.0', port=0)
    config['storage'] = {'database_path': str(root / 'alerts.db'),
                         'alert_directory': str(root / 'alerts')}
    config.setdefault('recording', {}).update(enabled=True, directory=str(root / 'recordings'))
    config.setdefault('webrtc', {}).update(enabled=True, max_peers=3)
    config.setdefault('whip', {})['enabled'] = False
    # Acceptance needs a real detection; do not fabricate an alert or write SQLite directly.
    if alert_class:
        config['alerts'] = [{'class': alert_class, 'confidence': 0.1,
                             'consecutive_frames': 1, 'cooldown_seconds': 60}]
    if not config.get('alerts'):
        raise RuntimeError('configure alerts or choose --alert-class visible in the RTSP scene')
    path = root / 'config.yaml'
    path.write_text(yaml.safe_dump(config))
    path.chmod(0o600)
    return config, path


def local_ipv4_addresses():
    local = set()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as descriptor:
        for _, name in socket.if_nameindex():
            try:
                data = fcntl.ioctl(descriptor, 0x8915, name.encode().ljust(256, b'\0'))
                local.add(socket.inet_ntoa(data[20:24]))
            except OSError:
                pass
    return local


def validate_host(host, skip_device_check):
    if not skip_device_check and (host.is_loopback or str(host) not in local_ipv4_addresses()):
        raise RuntimeError('--host must be a non-loopback IPv4 address assigned to this Jetson')


def remote_driver(driver_url, host):
    try:
        address = ipaddress.IPv4Address(urllib.parse.urlsplit(driver_url).hostname)
    except ValueError:
        return False
    local = local_ipv4_addresses() | {str(host)}
    return (address.is_private and not address.is_loopback and not address.is_unspecified and
            not address.is_multicast and str(address) not in local)


def confirm_lan(path, observed_url):
    report = json.loads(path.read_text())
    url = urllib.parse.urlsplit(report.get('url', ''))
    host = ipaddress.IPv4Address(url.hostname)
    run_id = report.get('run_id', '')
    if (not report.get('automated_passed') or not report.get('checks', {}).get('jetson') or
            not isinstance(run_id, str) or not re.fullmatch('[0-9a-f]{32}', run_id) or
            url.fragment != 'smoke=' + run_id or report.get('url') != observed_url or host.is_loopback or
            host.is_unspecified or host.is_multicast):
        raise RuntimeError('confirmation requires passed Jetson checks and the exact observed URL with run ID')
    report.update(lan_browser='manual_confirmed', status='passed',
                  lan_confirmation_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()))
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(report, indent=2) + '\n')
    temporary.replace(path)
    print('PASS operator-confirmed LAN browser video')
    return 0


def observe_live_video(browser, api, process, peer, seconds):
    first_media = previous_media = browser.media()
    first_rtsp = previous_rtsp = api('/api/v1/metrics')['rtsp']['frames_received']
    first_bytes = previous_bytes = peer['bytes_sent']
    start = time.monotonic()
    samples = 0
    while time.monotonic() - start < seconds or not samples:
        # Observe at least one useful interval, including for short portable runs.
        time.sleep(0.5)
        if process.poll() is not None:
            raise RuntimeError('owned skai-edge process exited during observation')
        media = browser.media()
        if (media['title'] != 'SKAI Edge Console' or media['width'] <= 0 or media['height'] <= 0 or
                media['time'] <= previous_media['time'] or media['frames'] <= previous_media['frames']):
            raise RuntimeError('video stopped advancing during observation')
        metrics = api('/api/v1/metrics')
        rtsp = metrics['rtsp']
        if rtsp['health'] != 'connected' or rtsp['frames_received'] <= previous_rtsp:
            raise RuntimeError('RTSP stopped advancing during observation')
        current = next((p for p in metrics['webrtc']['peers']
                        if p['session_id'] == peer['session_id']), None)
        if (not current or current['peer_state'] != 'connected' or
                current['ice_state'] not in ['connected', 'completed'] or
                current['selected_interface'] != peer['selected_interface'] or
                current['bytes_sent'] <= previous_bytes):
            raise RuntimeError('peer stopped sending during observation')
        previous_media, previous_rtsp, previous_bytes = media, rtsp['frames_received'], current['bytes_sent']
        samples += 1
    return {'elapsed_seconds': time.monotonic() - start, 'samples': samples,
            'session_id': peer['session_id'], 'media_start': first_media, 'media_end': previous_media,
            'rtsp_frames_start': first_rtsp, 'rtsp_frames_end': previous_rtsp,
            'bytes_sent_start': first_bytes, 'bytes_sent_end': previous_bytes}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('config', type=pathlib.Path, nargs='?')
    parser.add_argument('--confirm-lan-report', type=pathlib.Path)
    parser.add_argument('--observed-url', help='exact smoke URL whose video the operator observed')
    parser.add_argument('--binary', default='build/skai-edge')
    parser.add_argument('--output-dir', type=pathlib.Path)
    parser.add_argument('--host', help='Jetson LAN IPv4 address used by the browser')
    parser.add_argument('--alert-class', help='real class expected in this camera scene')
    parser.add_argument('--webdriver-url', help='existing local or LAN Firefox WebDriver endpoint')
    parser.add_argument('--geckodriver', default=shutil.which('geckodriver'))
    parser.add_argument('--openh264-dir', type=pathlib.Path, help='installed Linux Firefox GMP codec version directory')
    parser.add_argument('--openh264-abi', help='Firefox codec ABI, e.g. aarch64-gcc3 or x86_64-gcc3')
    parser.add_argument('--timeout', type=float, default=60)
    parser.add_argument('--seconds', type=float, default=5)
    parser.add_argument('--shutdown-timeout', type=float, default=15)
    parser.add_argument('--skip-device-check', action='store_true', help='marks acceptance incomplete')
    parser.add_argument('--ffmpeg', default='ffmpeg')
    parser.add_argument('--ffprobe', default='ffprobe')
    args = parser.parse_args()
    if args.confirm_lan_report:
        try:
            return confirm_lan(args.confirm_lan_report, args.observed_url)
        except (OSError, ValueError, RuntimeError) as error:
            print('FAIL ' + str(error))
            return 1
    if not args.config or not args.host:
        parser.error('provide CONFIG and --host for a smoke run')
    if not all(math.isfinite(value) and value > 0 for value in
               [args.timeout, args.seconds, args.shutdown_timeout]):
        parser.error('timeouts and duration must be finite and positive')
    host = ipaddress.IPv4Address(args.host)
    if host.is_unspecified or host.is_multicast:
        parser.error('--host must identify the Jetson, not a wildcard or multicast address')
    root = (args.output_dir or pathlib.Path('build') /
            ('smoke-jetson-' + time.strftime('%Y%m%d-%H%M%S') + '-' + str(os.getpid()))).resolve()
    root.mkdir(parents=True, exist_ok=False)
    root.chmod(0o700)
    report = {'run_id': secrets.token_hex(16), 'checks': {},
              'automated_passed': False, 'lan_browser': 'manual_pending'}
    edge = driver = browser = None
    handles = []

    def passed(name, evidence=True):
        report['checks'][name] = evidence
        print('PASS ' + name, flush=True)

    def interrupted(*_):
        raise RuntimeError('smoke run interrupted')

    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    try:
        if not args.skip_device_check:
            if platform.machine() != 'aarch64' or not pathlib.Path('/etc/nv_tegra_release').is_file():
                raise RuntimeError('Jetson/L4T is required; portable harness uses --skip-device-check')
            passed('jetson', pathlib.Path('/etc/nv_tegra_release').read_text().splitlines()[0])
        validate_host(host, args.skip_device_check)
        config, config_path = prepare_config(args.config, root, args.alert_class)
        if not args.webdriver_url and not args.geckodriver:
            raise RuntimeError('provide geckodriver or --webdriver-url for real browser checks')
        for tool in [args.binary, args.ffmpeg, args.ffprobe]:
            if not shutil.which(tool):
                raise RuntimeError('required executable unavailable: ' + tool)
        log_path = root / 'edge.log'
        log = log_path.open('w'); handles.append(log)
        edge = subprocess.Popen([args.binary, '--config', str(config_path)],
                                stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        report['edge_pid'] = edge.pid
        match = wait_for(lambda: re.search(r'HTTP server started on port (\d+)',
                                          log_path.read_text()), args.timeout, edge)
        base = 'http://' + str(host) + ':' + match[1]
        local = 'http://127.0.0.1:' + match[1]
        report['url'] = base + '/#smoke=' + report['run_id']
        print('Browser URL: ' + report['url'], flush=True)

        def api(path, method='GET'):
            request = urllib.request.Request(local + path, method=method,
                                             data=b'{}' if method == 'POST' else None,
                                             headers={'Content-Type': 'application/json'})
            with urllib.request.urlopen(request, timeout=3) as response:
                return json.load(response)

        def inference_ready():
            value = api('/api/v1/status')
            detector = value['detector']
            return value if (detector['fps'] or 0) > 0 and detector['last_inference_ms'] is not None else None

        status = wait_for(inference_ready, args.timeout, edge, 'TensorRT inference')
        passed('tensorrt_inference', status['detector'])
        metrics = api('/api/v1/metrics')
        if metrics['rtsp']['health'] != 'connected' or metrics['rtsp']['frames_received'] <= 0:
            raise RuntimeError('configured RTSP stream has no connected decoded frames')
        passed('rtsp', metrics['rtsp']['frames_received'])
        passed('web_api', status['status'])
        wait_for(lambda: api('/health')['state'] == 'RUNNING', args.timeout, edge, 'runtime health')
        passed('runtime_health')
        alerts = wait_for(lambda: api('/api/v1/alerts')['items'], args.timeout, edge, 'new persisted alert')
        alert = alerts[0]
        if not pathlib.Path(alert['snapshot_path']).is_file():
            raise RuntimeError('new alert lacks its persisted snapshot')
        passed('new_alert', alert['id'])
        if not args.webdriver_url:
            if not args.openh264_dir:
                plugins = list((pathlib.Path.home() / '.mozilla/firefox').glob(
                    '*/gmp-gmpopenh264/*/libgmpopenh264.so'))
                if plugins:
                    args.openh264_dir = max(plugins, key=lambda p: p.stat().st_mtime).parent
                    args.openh264_abi = platform.machine() + '-gcc3'
            driver_path = root / 'geckodriver.log'
            driver_log = driver_path.open('w'); handles.append(driver_log)
            driver = subprocess.Popen([args.geckodriver, '--host', '127.0.0.1', '--port', '0'],
                stdout=driver_log, stderr=subprocess.STDOUT, start_new_session=True)
            port = wait_for(lambda: re.search(r'Listening on 127\.0\.0\.1:(\d+)',
                                             driver_path.read_text()), args.timeout, driver)[1]
            args.webdriver_url = 'http://127.0.0.1:' + port
        browser = BrowserProbe(args.webdriver_url, args.timeout, args.openh264_dir, args.openh264_abi)
        browser.open(report['url'])
        passed('browser_ui_and_video', browser.verify())
        peers = api('/api/v1/metrics')['webrtc']['peers']
        selected = [p for p in peers if p['peer_state'] == 'connected' and
                    p['selected_interface'] in config['webrtc']['host_interfaces'] and
                    p['bytes_sent'] > 0]
        if not selected:
            raise RuntimeError('ICE diagnostics lack a connected peer on the configured interface')
        passed('ice_host_interface', selected[0]['selected_interface'])
        passed('live_observation', observe_live_video(browser, api, edge, selected[0], args.seconds))
        api('/api/v1/recording/stop', 'POST')
        def recording_stopped():
            value = api('/api/v1/recordings')
            return value if not value['active'] else None

        recording = wait_for(recording_stopped, args.timeout, edge, 'recorder finalization')
        if recording['access_units_written'] <= 0:
            raise RuntimeError('recorder did not write access units and stop')
        browser.close(); browser = None
        if not stop_process(edge, args.shutdown_timeout) or 'skai-edge stopped' not in log_path.read_text():
            raise RuntimeError('clean shutdown failed (forced termination or nonzero exit)')
        passed('clean_shutdown')
        with sqlite3.connect('file:' + urllib.parse.quote(str(root / 'alerts.db')) + '?mode=ro',
                             uri=True) as db:
            if db.execute('PRAGMA integrity_check').fetchone()[0] != 'ok':
                raise RuntimeError('SQLite integrity check failed')
            row = db.execute('SELECT snapshot_path FROM alerts WHERE id=?', (alert['id'],)).fetchone()
            classes = [r[0] for r in db.execute('SELECT class_name FROM detections WHERE alert_id=?',
                                               (alert['id'],))]
            if (not row or row[0] != alert['snapshot_path'] or not classes or
                    sorted(classes) != sorted(item['class_name'] for item in alert['detections'])):
                raise RuntimeError('new API alert was not durable in SQLite after shutdown')
        passed('sqlite_persistence', {'id': alert['id'], 'classes': classes})
        files = list((root / 'recordings').glob('*.mp4'))
        if not files:
            raise RuntimeError('no finalized MP4 recording')
        for path in files:
            subprocess.run([args.ffmpeg, '-v', 'error', '-xerror', '-i', str(path),
                            '-map', '0:v:0', '-f', 'null', '-'], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=args.timeout)
            probe = subprocess.run([args.ffprobe, '-v', 'error', '-count_frames', '-select_streams',
                'v:0', '-show_entries', 'stream=codec_name,width,height,nb_read_frames',
                '-of', 'json', str(path)], check=True, capture_output=True, timeout=args.timeout)
            streams = json.loads(probe.stdout)['streams']
            if not streams or int(streams[0].get('nb_read_frames', 0)) <= 0:
                raise RuntimeError('MP4 has no decoded video frames')
        passed('recording_decode_to_eos', len(files))
        if remote_driver(args.webdriver_url, host) and not host.is_loopback:
            report['lan_browser'] = 'remote_webdriver_passed'
        report['automated_passed'] = True
    except Exception as error:
        report['error'] = str(error)
        print('FAIL ' + str(error), flush=True)
    finally:
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        if browser:
            try: browser.close()
            except Exception: pass
        for process in [edge, driver]:
            if process:
                stop_process(process, args.shutdown_timeout)
        for handle in handles:
            handle.close()
        report['status'] = ('passed' if report['automated_passed'] and
                            report['lan_browser'] == 'remote_webdriver_passed' and
                            not args.skip_device_check else 'incomplete' if
                            report['automated_passed'] else 'failed')
        (root / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        print('Report: ' + str(root / 'report.json'), flush=True)
    return {'passed': 0, 'failed': 1, 'incomplete': 2}[report['status']]


if __name__ == '__main__':
    raise SystemExit(main())
