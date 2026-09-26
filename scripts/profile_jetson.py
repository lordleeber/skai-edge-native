"""Own a controlled RTSP/Jetson/Firefox run and write a reproducible baseline."""
import argparse
import hashlib
import json
import math
import pathlib
import re
import shutil
import signal
import secrets
import sqlite3
import subprocess
import sys
import time
import urllib.parse
import urllib.request
import yaml
from jetson_smoke import stop_process, wait_for
from smoke_browser import BrowserProbe
from profile_values import process_sample, cpu_delta, timing_delta, summarize, validate_window, number, receiver_delays, build_info, require_release


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('config', type=pathlib.Path)
    parser.add_argument('--binary', type=pathlib.Path, default=pathlib.Path('build/profiling/skai-edge'))
    parser.add_argument('--fixture-script', type=pathlib.Path, required=True,
                        help='explicit test-only timestamp RTSP fixture')
    parser.add_argument('--webdriver-url', required=True, help='local Firefox WebDriver URL')
    parser.add_argument('--openh264-dir')
    parser.add_argument('--openh264-abi')
    parser.add_argument('--output-dir', type=pathlib.Path, required=True)
    parser.add_argument('--seconds', type=float, default=60)
    parser.add_argument('--warmup', type=float, default=10)
    parser.add_argument('--interval', type=float, default=1)
    parser.add_argument('--timeout', type=float, default=30)
    parser.add_argument('--skip-device-check', action='store_true', help='portable harness, never a Jetson baseline')
    args = parser.parse_args()
    if (any(not math.isfinite(v) for v in [args.seconds, args.warmup, args.interval, args.timeout]) or
            not 2 <= args.seconds <= 600 or args.warmup < 0 or not .5 <= args.interval <= 5 or args.timeout <= 0):
        parser.error('use seconds 2..600, nonnegative warmup, interval .5..5 and positive timeout')
    if urllib.parse.urlsplit(args.webdriver_url).hostname not in ['127.0.0.1', 'localhost', '::1']:
        parser.error('local WebDriver required so source and browser share a host clock')
    root = args.output_dir.resolve()
    root.mkdir(mode=0o700, parents=True, exist_ok=False)
    report = {'status': 'failed', 'run_id': secrets.token_hex(16), 'samples': [],
              'receiver': 'production Console UI in Firefox on the same Jetson; HTTP loopback and configured host ICE candidates',
              'portable_harness': args.skip_device_check, 'config_modified': False}
    nonce = secrets.randbits(32)
    edge = fixture = browser = None
    handles = []
    def interrupted(*_): raise RuntimeError('profiling interrupted')
    old_term = signal.signal(signal.SIGTERM, interrupted)
    try:
        device = pathlib.Path('/proc/device-tree/model')
        model = device.read_text().strip('\0') if device.exists() else 'unavailable'
        if not args.skip_device_check and ('Orin Nano' not in model or not pathlib.Path('/etc/nv_tegra_release').exists()):
            raise RuntimeError('a real Jetson Orin Nano is required')
        report['source_config_sha256'] = hashlib.sha256(args.config.read_bytes()).hexdigest()
        config = yaml.safe_load(args.config.read_text())
        engine = pathlib.Path(config['detector']['engine']).resolve()
        if not engine.is_file() or not engine.stat().st_size:
            raise RuntimeError('configured TensorRT engine missing')
        report['environment'] = {'device': model, 'engine_sha256': hashlib.sha256(engine.read_bytes()).hexdigest(),
            'kernel': subprocess.check_output(['uname', '-r'], text=True).strip(),
            'binary_sha256': hashlib.sha256(args.binary.resolve().read_bytes()).hexdigest(),
            'l4t': pathlib.Path('/etc/nv_tegra_release').read_text().splitlines()[0] if not args.skip_device_check else None,
            'cpu_governor': pathlib.Path('/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor').read_text().strip() if pathlib.Path('/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor').exists() else None}
        power = subprocess.run(['nvpmodel', '-q'], capture_output=True, text=True) if shutil.which('nvpmodel') else None
        info = build_info(args.binary)
        require_release(info, args.skip_device_check)
        report['environment']['build'] = info
        report['environment']['compiler_version'] = subprocess.check_output([info['CMAKE_CXX_COMPILER'], '--version'], text=True).splitlines()[0] if info.get('CMAKE_CXX_COMPILER') else None
        report['environment']['power_mode'] = power.stdout.strip() if power and power.returncode == 0 else None
        report['environment']['collector_sha256'] = hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest()
        report['environment']['fixture_sha256'] = hashlib.sha256(args.fixture_script.read_bytes()).hexdigest()
        report['environment']['ui_assets_sha256'] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
            for p in pathlib.Path(config.get('web', {}).get('root', 'web')).glob('*')
            if p.is_file() and p.suffix in ['.js', '.css', '.html']}
        report['environment']['values_module_sha256'] = hashlib.sha256(pathlib.Path(__file__).with_name('profile_values.py').read_bytes()).hexdigest()
        report['environment']['browser_script_sha256'] = hashlib.sha256(pathlib.Path(__file__).with_name('profile_browser.js').read_bytes()).hexdigest()
        report['environment']['worktree_dirty'] = bool(subprocess.check_output(['git', 'status', '--porcelain'], text=True).strip())
        report['environment']['git_sha'] = subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip()
        report['workload'] = '1280x720 RGB moving box + timestamp stripe, 30 FPS; synthetic scene'
        report['started_utc'] = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())
        source_log = root / 'source.log'
        handles.append(source_log.open('w'))
        fixture = subprocess.Popen([sys.executable, str(args.fixture_script.resolve()), '--nonce', str(nonce)],
            stdout=handles[-1], stderr=subprocess.STDOUT, start_new_session=True)
        report['fixture_pid'] = fixture.pid
        source = wait_for(lambda: json.loads(lines[0]) if (lines := source_log.read_text().splitlines()) else None, args.timeout, fixture, 'RTSP fixture')
        if urllib.parse.urlsplit(source['url']).scheme != 'rtsp' or source['nonce'] != nonce:
            raise RuntimeError('fixture did not identify its RTSP source and nonce')
        report['source'] = source
        config['video']['rtsp_url'] = source['url']
        config['detector']['engine'] = str(engine)
        config.setdefault('web', {}).update(bind='127.0.0.1', port=0,
            root=str(pathlib.Path(config.get('web', {}).get('root', 'web')).resolve()))
        config['storage'] = {'database_path': str(root / 'alerts.db'), 'alert_directory': str(root / 'alerts')}
        config.setdefault('recording', {}).update(enabled=True, directory=str(root / 'recordings'))
        config.setdefault('webrtc', {}).update(enabled=True, max_peers=1)
        config.setdefault('whip', {})['enabled'] = False
        path = root / 'config.yaml'
        path.write_text(yaml.safe_dump(config)); path.chmod(0o600)
        log = root / 'edge.log'; handles.append(log.open('w'))
        edge = subprocess.Popen([str(args.binary.resolve()), '--config', str(path)],
            stdout=handles[-1], stderr=subprocess.STDOUT, start_new_session=True)
        report['edge_pid'] = edge.pid
        match = wait_for(lambda: re.search(r'HTTP server started on port (\d+)', log.read_text()), args.timeout, edge, 'HTTP')
        base = 'http://127.0.0.1:' + match[1]
        def api(route):
            with urllib.request.urlopen(base + route, timeout=5) as response:
                return json.load(response)
        report['effective_config'] = api('/api/v1/config')
        browser = BrowserProbe(args.webdriver_url, args.timeout, args.openh264_dir, args.openh264_abi)
        capabilities = browser.open(base).get('capabilities', {})
        if not args.skip_device_check:
            browser_pid = capabilities.get('moz:processID', 0)
            if browser_pid <= 0 or 'firefox' not in str((pathlib.Path('/proc') / str(browser_pid) / 'exe').resolve()):
                raise RuntimeError('WebDriver must own a local Firefox process')
        report['browser'] = {k: capabilities.get(k) for k in ['browserName', 'browserVersion', 'moz:processID']}
        setup = pathlib.Path(__file__).with_name('profile_browser.js').read_text()
        result = browser.command('POST', browser.session + '/execute/async', {'args': [nonce], 'script': setup +
            '\nconst done=arguments[arguments.length-1]; startProfile(arguments[0]).then(()=>done({ok:true}),e=>done({error:String(e)}));'})
        if not result.get('ok'): raise RuntimeError('browser setup: ' + str(result))
        def media():
            return browser.command('POST', browser.session + '/execute/async', {'args': [], 'script':
                'const done=arguments[arguments.length-1]; window.__profile.poll().then(done,e=>done({error:String(e)}));'})
        wait_for(lambda: (m if (m := media()).get('frames', 0) >= 2 and m.get('latency') else None),
                 args.timeout, edge, 'decoded timestamp watermark')
        # Warmup is actively observed too; pure sleep would hide a frozen receiver.
        def snapshot():
            metrics, received = api('/api/v1/metrics'), media()
            json.dumps([metrics, received], allow_nan=False)
            peers = metrics['webrtc']['peers']
            peer = next((p for p in peers if p['session_id'] == received['peer']), None)
            if not peer or peer['peer_state'] != 'connected' or received['state'] != 'connected':
                raise RuntimeError('owned WHEP peer is not connected')
            number(metrics['system']['gpu_percent'])
            if received['width'] != source['width'] or received['height'] != source['height']:
                raise RuntimeError('browser dimensions differ from the owned RTSP source')
            if metrics['rtsp']['health'] != 'connected': raise RuntimeError('RTSP source not connected')
            if metrics['encoder_mode'] != 'passthrough': raise RuntimeError('this baseline requires the production passthrough graph')
            recording = api('/api/v1/recordings')
            if not recording['active'] or recording['last_error']: raise RuntimeError('recording is not active')
            clocks = {'cpu0_khz': pathlib.Path('/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq').read_text().strip()
                      if pathlib.Path('/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq').exists() else None,
                      'gpu_hz': pathlib.Path('/sys/devices/platform/gpu.0/devfreq/17000000.gpu/cur_freq').read_text().strip()
                      if pathlib.Path('/sys/devices/platform/gpu.0/devfreq/17000000.gpu/cur_freq').exists() else None}
            return {'metrics': metrics, 'media': received, 'cpu': process_sample(edge.pid),
                    'recording': recording, 'clocks': clocks,
                    'clock_skew_ms': received['clock_skew_ms'],
                    'peer': peer['session_id'], 'frames': received['frames'], 'time': received['time'],
                    'bytes': peer['bytes_sent'], 'source_frames': metrics['rtsp']['frames_received']}
        previous = snapshot()
        warmup_end = time.monotonic() + args.warmup
        while time.monotonic() < warmup_end:
            time.sleep(args.interval)
            current = snapshot(); validate_window(previous, current); previous = current
        first = previous
        report.update(warmup_seconds=args.warmup, requested_seconds=args.seconds, first=first)
        started = time.monotonic()
        while time.monotonic() - started < args.seconds:
            time.sleep(args.interval)
            if edge.poll() is not None or fixture.poll() is not None: raise RuntimeError('owned process exited')
            current = snapshot(); validate_window(previous, current)
            if not current['media']['latency'] or current['media']['invalid']:
                raise RuntimeError('timestamp watermark unavailable or invalid in observation window')
            stages = {k: timing_delta(previous['metrics']['profiling'][k], v)
                      for k, v in current['metrics']['profiling'].items()}
            if stages['inference_gpu'] is None: raise RuntimeError('TensorRT stopped advancing')
            peer_a = next(p for p in previous['metrics']['webrtc']['peers'] if p['session_id'] == previous['peer'])
            peer_b = next(p for p in current['metrics']['webrtc']['peers'] if p['session_id'] == current['peer'])
            row = {'elapsed': time.monotonic() - started, 'cpu': cpu_delta(previous['cpu'], current['cpu']),
                   'stage_interval_mean_ms': stages,
                   'whep_queue_interval_mean_ms': timing_delta(peer_a['media_queue_wait'], peer_b['media_queue_wait']),
                   'receiver_delays': receiver_delays(previous['media']['stats'], current['media']['stats']),
                   'browser_annotation_interval_mean_ms': timing_delta(previous['media']['annotation'], current['media']['annotation']),
                   'probe_interval_mean_ms': timing_delta(previous['media']['probe'], current['media']['probe']),
                   'gpu_percent': current['metrics']['system']['gpu_percent'],
                   'memory_rss_bytes': current['metrics']['system']['memory_rss_bytes'],
                   'ingest_fps': (current['source_frames'] - previous['source_frames']) /
                                 (current['cpu']['at'] - previous['cpu']['at']), 'raw': current}
            report['samples'].append(row); previous = current
            print('sample', len(report['samples']), 'fps', round(row['ingest_fps'], 2), flush=True)
        report['last'] = previous
        report['observed_seconds'] = time.monotonic() - started
        report['inference_fps'] = (previous['metrics']['profiling']['inference_gpu']['count'] -
            first['metrics']['profiling']['inference_gpu']['count']) / (previous['cpu']['at'] - first['cpu']['at'])
        report['summary'] = {key: summarize([s[key] for s in report['samples']])
                            for key in ['gpu_percent', 'memory_rss_bytes', 'ingest_fps']}
        report['summary']['end_to_end_browser_ms'] = summarize([v for s in report['samples'] for v in s['raw']['media']['latency']])
        for key in ['process_percent', 'rtsp_workers_percent']:
            report['summary'][key] = summarize([s['cpu'][key] for s in report['samples']])
        report['summary']['receiver_media_ms'] = summarize([s['receiver_delays']['receiver_media_ms'] for s in report['samples']])
        report['summary']['whep_queue_interval_mean_ms'] = summarize([s['whep_queue_interval_mean_ms'] for s in report['samples']])
        report['stage_totals'] = {k: {'count': v['count'] - first['metrics']['profiling'][k]['count'],
            'mean_ms': timing_delta(first['metrics']['profiling'][k], v)}
            for k, v in previous['metrics']['profiling'].items()}
        report['stage_interval_means'] = {k: summarize([s['stage_interval_mean_ms'][k] for s in report['samples']
            if s['stage_interval_mean_ms'][k] is not None]) for k in previous['metrics']['profiling']}
        report['browser_annotation'] = {'count': previous['media']['annotation']['count'] - first['media']['annotation']['count'],
            'mean_ms': timing_delta(first['media']['annotation'], previous['media']['annotation'])}
        report['browser_probe'] = {'count': previous['media']['probe']['count'] - first['media']['probe']['count'],
            'mean_ms': timing_delta(first['media']['probe'], previous['media']['probe'])}
        report['browser_presented_fps'] = (previous['frames'] - first['frames']) / (previous['cpu']['at'] - first['cpu']['at'])
        report['latency_sampling'] = 'one watermark sample per five presented frames, every decoded marker validated'
        report['receiver_interval_means'] = {k: summarize([s['receiver_delays'][k] for s in report['samples']])
            for k in ['jitter_buffer_ms', 'decode_ms']}
        report['encode_cpu'] = {'status': 'not_applicable', 'reason': 'source H.264 passthrough; no local encoder'}
        report['status'] = 'portable_harness_passed' if args.skip_device_check else 'baseline_passed'
    except Exception as error:
        report['error'] = str(error)
    finally:
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        if browser:
            try: browser.close()
            except Exception as error: report.update(status='failed', browser_cleanup_error=str(error))
        for process in [edge, fixture]:
            if process:
                clean = stop_process(process, 30)
                if not clean: report.update(status='failed', shutdown_error='owned process did not exit cleanly')
        for handle in handles: handle.close()
        if edge and report['status'] != 'failed':
            try:
                recordings = list((root / 'recordings').glob('*.mp4'))
                if not recordings: raise RuntimeError('no finalized recording')
                report['recordings'] = []
                for recording in recordings:
                    decoded = subprocess.run(['ffmpeg', '-v', 'error', '-i', str(recording),
                        '-map', '0:v:0', '-f', 'null', '-', '-progress', 'pipe:1'],
                        capture_output=True, text=True, timeout=30)
                    frames = max([int(v) for v in re.findall(r'frame=(\d+)', decoded.stdout)] or [0])
                    if decoded.returncode != 0 or frames <= 0: raise RuntimeError('recording decode failed')
                    report['recordings'].append({'name': recording.name, 'bytes': recording.stat().st_size, 'decoded_frames': frames})
                with sqlite3.connect('file:' + str(root / 'alerts.db') + '?mode=ro', uri=True) as db:
                    if db.execute('PRAGMA integrity_check').fetchone()[0] != 'ok': raise RuntimeError('SQLite integrity failure')
            except Exception as error: report.update(status='failed', storage_error=str(error))
        signal.signal(signal.SIGTERM, old_term)
        (root / 'report.json').write_text(json.dumps(report, indent=2, allow_nan=False) + '\n')
    print(report['status'], root / 'report.json')
    return 0 if report['status'] == 'baseline_passed' else 2 if report['status'] == 'portable_harness_passed' else 1

if __name__ == '__main__':
    sys.exit(main())
