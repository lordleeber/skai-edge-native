"""Profiling math: cumulative deltas, scoped CPU attribution and raw summaries."""
import math
import os
import pathlib
import time


def number(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0:
        raise ValueError('measurement must be finite and nonnegative')
    return value


def timing_delta(first, last):
    a, b = number(first['count']), number(last['count'])
    x, y = number(first['total_ms']), number(last['total_ms'])
    if a != int(a) or b != int(b) or b < a or y < x or (b == a and y != x):
        raise ValueError('cumulative timing reset or inconsistent count')
    return (y - x) / (b - a) if b > a else None


def summarize(values):
    if not values:
        return None
    ordered = sorted(number(v) for v in values)
    return {'count': len(ordered), 'mean': sum(ordered) / len(ordered),
            'min': ordered[0], 'max': ordered[-1],
            'p50': ordered[math.ceil(len(ordered) * .5) - 1],
            'p95': ordered[math.ceil(len(ordered) * .95) - 1]}


def parse_stat(line):
    start, end = line.index('('), line.rindex(')')
    fields = line[end + 2:].split()
    return {'identity': line[:start].strip() + ':' + fields[19],
            'name': line[start + 1:end], 'ticks': int(fields[11]) + int(fields[12])}


def process_sample(pid):
    root = pathlib.Path('/proc') / str(pid)
    result = parse_stat((root / 'stat').read_text())
    result['threads'] = []
    for task in (root / 'task').iterdir():
        try:
            result['threads'].append(parse_stat((task / 'stat').read_text()))
        except FileNotFoundError:
            pass  # Thread churn remains visible in each sample's task inventory.
    result['at'] = time.monotonic()
    return result


def cpu_delta(first, last, hz=None):
    if first['identity'] != last['identity'] or last['ticks'] < first['ticks']:
        raise ValueError('owned process identity changed')
    elapsed = last['at'] - first['at']
    if elapsed <= 0:
        raise ValueError('CPU observation requires positive elapsed time')
    scale = 100 / ((hz or os.sysconf('SC_CLK_TCK')) * elapsed)
    old = {t['identity']: t for t in first['threads']}
    threads = []
    for task in last['threads']:
        previous = old.get(task['identity'])
        if previous and task['ticks'] >= previous['ticks']:
            threads.append({'name': task['name'], 'identity': task['identity'],
                            'percent': (task['ticks'] - previous['ticks']) * scale})
    prefixes = ('skai-rtsp', 'skai-decode', 'skai-demux', 'rtpjitterbuffer', 'rtspsrc', 'V4L2', 'nvv4l2decoder', 'avdec_h264', 'avdec_h265')
    return {'process_percent': (last['ticks'] - first['ticks']) * scale,
            'rtsp_workers_percent': sum(t['percent'] for t in threads if t['name'].startswith(prefixes)),
            'threads': threads,
            'new_tasks': len({t['identity'] for t in last['threads']} - set(old)),
            'exited_tasks': len(set(old) - {t['identity'] for t in last['threads']})}


def validate_window(first, last):
    for sample in [first, last]:
        if number(abs(sample.get('clock_skew_ms', 0))) > 5:
            raise RuntimeError('browser wall clock differs from its monotonic origin')
    if first['peer'] != last['peer']:
        raise RuntimeError('WHEP session changed within the observation window')
    for key in ['frames', 'time', 'bytes', 'source_frames']:
        if number(last[key]) <= number(first[key]):
            raise RuntimeError('live media stopped advancing: ' + key)


def receiver_delays(first, last):
    if not first or not last or first['id'] != last['id']:
        raise ValueError('browser receiver statistics unavailable or changed')
    def average(count, seconds):
        return timing_delta({'count': first[count], 'total_ms': number(first[seconds]) * 1000},
                            {'count': last[count], 'total_ms': number(last[seconds]) * 1000})
    jitter = average('jitterBufferEmittedCount', 'jitterBufferDelay')
    decode = average('framesDecoded', 'totalDecodeTime')
    if jitter is None or decode is None:
        raise ValueError('browser receiver statistics stopped advancing')
    return {'jitter_buffer_ms': jitter, 'decode_ms': decode,
            'receiver_media_ms': jitter + decode}


def build_info(binary):
    cache = pathlib.Path(binary).resolve().parent / 'CMakeCache.txt'
    keys = {'CMAKE_BUILD_TYPE', 'CMAKE_CXX_COMPILER', 'CMAKE_CXX_FLAGS', 'CMAKE_CXX_FLAGS_RELEASE',
            'CMAKE_CUDA_FLAGS', 'CMAKE_CUDA_FLAGS_RELEASE'}
    result = {}
    for line in cache.read_text().splitlines() if cache.is_file() else []:
        key = line.split(':', 1)[0]
        if key in keys and '=' in line:
            result[key] = line.split('=', 1)[1]
    return result


def require_release(info, portable):
    if not portable and info.get('CMAKE_BUILD_TYPE') != 'Release':
        raise RuntimeError('build a Release binary for a device performance baseline')
