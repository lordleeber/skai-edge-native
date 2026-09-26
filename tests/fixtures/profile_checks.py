import pathlib
import subprocess
import sys
import time
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / 'scripts'))
from profile_values import timing_delta, summarize, parse_stat, cpu_delta, validate_window, receiver_delays, build_info, require_release
from jetson_smoke import stop_process

def reject(action):
    try: action()
    except (ValueError, RuntimeError): return
    raise AssertionError('invalid evidence accepted')

case = sys.argv[1]
if case == 'timings':
    assert timing_delta({'count': 2, 'total_ms': 20}, {'count': 4, 'total_ms': 26}) == 3
    assert timing_delta({'count': 0, 'total_ms': 0}, {'count': 0, 'total_ms': 0}) is None
    assert timing_delta({'count': 0, 'total_ms': 0}, {'count': 1, 'total_ms': 0}) == 0
elif case == 'invalid':
    for value in [float('nan'), float('inf'), -1]:
        reject(lambda: timing_delta({'count': 0, 'total_ms': 0}, {'count': 1, 'total_ms': value}))
    reject(lambda: timing_delta({'count': 3, 'total_ms': 2}, {'count': 2, 'total_ms': 3}))
    reject(lambda: timing_delta({'count': 1, 'total_ms': 4}, {'count': 2, 'total_ms': 2}))
    reject(lambda: timing_delta({'count': 0, 'total_ms': 0}, {'count': 0, 'total_ms': 1}))
elif case == 'cpu':
    fields = ['S'] + ['0'] * 21
    fields[11], fields[12], fields[19] = '4', '6', '99'
    stat = parse_stat('12 (name with ) spaces) ' + ' '.join(fields))
    assert stat == {'identity': '12:99', 'name': 'name with ) spaces', 'ticks': 10}
    first = {'at': 2, 'identity': '1:2', 'ticks': 10, 'threads': [dict(stat, name='skai-decode:src')]}
    last = dict(first, at=3, ticks=35, threads=[dict(stat, ticks=30, name='skai-decode:src')])
    result = cpu_delta(first, last, 100)
    assert result['process_percent'] == 25 and result['rtsp_workers_percent'] == 20
    reject(lambda: cpu_delta(first, dict(last, identity='1:3'), 100))
    last['threads'][0]['identity'] = '12:100'
    assert cpu_delta(first, last, 100)['rtsp_workers_percent'] == 0
    first['threads'].append(dict(stat, identity='13:99', name='nvv4l2decoder0:'))
    last['threads'].append(dict(stat, identity='13:99', ticks=20, name='nvv4l2decoder0:'))
    assert cpu_delta(first, last, 100)['rtsp_workers_percent'] == 10
elif case == 'freeze':
    first = {'frames': 1, 'time': 1, 'bytes': 10, 'peer': 'a', 'source_frames': 1}
    last = {'frames': 2, 'time': 2, 'bytes': 20, 'peer': 'a', 'source_frames': 2}
    validate_window(first, last)
    for key in ['frames', 'time', 'bytes', 'source_frames']:
        reject(lambda: validate_window(first, dict(last, **{key: first[key]})))
    reject(lambda: validate_window(first, dict(last, peer='b')))
    reject(lambda: validate_window(first, dict(last, clock_skew_ms=10)))
    reject(lambda: validate_window(first, dict(last, clock_skew_ms=float('nan'))))
elif case == 'summary':
    assert summarize([]) is None
    value = summarize([4, 1, 3, 2])
    assert value['count'] == 4 and value['mean'] == 2.5 and value['p95'] == 4
    reject(lambda: summarize([float('nan')]))
elif case == 'cleanup':
    child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'], start_new_session=True)
    try:
        assert stop_process(child, 1) is False  # Unhandled SIGTERM is not a clean application exit.
        assert child.poll() is not None
    finally:
        if child.poll() is None: child.kill(); child.wait()

elif case == 'receiver':
    first = {'id': 'a', 'jitterBufferEmittedCount': 10, 'jitterBufferDelay': 1, 'framesDecoded': 10, 'totalDecodeTime': 1}
    last = dict(first, jitterBufferEmittedCount=60, jitterBufferDelay=1.2, framesDecoded=60, totalDecodeTime=1.5)
    assert abs(receiver_delays(first, last)['receiver_media_ms'] - 14) < 1e-8
    reject(lambda: receiver_delays(first, dict(last, id='b')))
    reject(lambda: receiver_delays(first, dict(last, totalDecodeTime=.5)))
    reject(lambda: receiver_delays(first, dict(last, framesDecoded=10)))

elif case == 'runner':
    import json, os, tempfile, yaml
    with tempfile.TemporaryDirectory(prefix='skai-profile ') as temporary:
        root = pathlib.Path(temporary)
        engine = root / 'engine'; engine.write_bytes(b'portable fixture engine')
        service = pathlib.Path(__file__).with_name('smoke_service.py')
        edge = root / 'edge'
        edge.write_text('#!' + sys.executable + '\nimport os,sys\nos.execv(sys.executable, [sys.executable, ' + repr(str(service)) + '] + sys.argv[1:])\n')
        edge.chmod(0o700)
        fixture = root / 'source.py'
        fixture.write_text("import argparse,json,signal,time\np=argparse.ArgumentParser();p.add_argument('--nonce',type=int);a=p.parse_args()\nprint(json.dumps({'url':'rtsp://fixture/healthy','nonce':a.nonce,'width':1280,'height':720}),flush=True)\nsignal.signal(signal.SIGTERM,lambda *_:exit(0))\ntime.sleep(60)\n")
        config = root / 'original.yaml'
        config.write_text(yaml.safe_dump({'video': {'rtsp_url': 'rtsp://camera/original'},
            'detector': {'engine': str(engine)}, 'webrtc': {'host_interfaces': ['lo']}}))
        before = config.read_bytes()
        runner = pathlib.Path(__file__).resolve().parents[2] / 'scripts/profile_jetson.py'
        result = subprocess.run([sys.executable, str(runner), str(config), '--binary', str(edge),
            '--fixture-script', str(fixture), '--webdriver-url', 'http://127.0.0.1:1',
            '--output-dir', str(root / 'result'), '--skip-device-check', '--seconds', '2',
            '--warmup', '0', '--timeout', '1'], timeout=12)
        assert result.returncode == 1
        report = json.loads((root / 'result/report.json').read_text())
        assert report['status'] == 'failed' and config.read_bytes() == before
        for key in ['edge_pid', 'fixture_pid']:
            try: os.kill(report[key], 0)
            except ProcessLookupError: pass
            else: raise AssertionError('owned child leaked: ' + key)

elif case == 'build':
    import tempfile
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        (root / 'CMakeCache.txt').write_text('CMAKE_BUILD_TYPE:STRING=Release\nCMAKE_CXX_FLAGS_RELEASE:STRING=-O3 -DNDEBUG\nSECRET:STRING=not-in-report\n')
        info = build_info(root / 'edge')
        assert info == {'CMAKE_BUILD_TYPE': 'Release', 'CMAKE_CXX_FLAGS_RELEASE': '-O3 -DNDEBUG'}
        require_release(info, False)
        reject(lambda: require_release(dict(info, CMAKE_BUILD_TYPE='Debug'), False))
        require_release({}, True)
