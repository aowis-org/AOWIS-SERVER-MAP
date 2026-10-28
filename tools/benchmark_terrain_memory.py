#!/usr/bin/env python3
"""Benchmark terrain HTTP latency and Linux process RSS using Python's standard library."""
import argparse
import concurrent.futures
import json
import os
import statistics
import random
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from collections import Counter


def rss_kib(pid):
    with open('/proc/{}/status'.format(pid), encoding='ascii') as stream:
        for line in stream:
            if line.startswith('VmRSS:'):
                return int(line.split()[1])
    raise RuntimeError('VmRSS unavailable for PID {}'.format(pid))



def process_counters(pid):
    """Read Linux process CPU time, actual disk bytes and thread count."""
    with open('/proc/{}/stat'.format(pid), encoding='ascii') as stream:
        line = stream.read()
    # comm is parenthesized and may contain spaces or closing parentheses.
    end_comm = line.rfind(')')
    if end_comm < 0:
        raise RuntimeError('Malformed /proc/{}/stat'.format(pid))
    fields = line[end_comm + 2:].split()
    ticks = float(os.sysconf('SC_CLK_TCK'))
    cpu_seconds = (int(fields[11]) + int(fields[12])) / ticks
    threads = int(fields[17])
    with open('/proc/{}/io'.format(pid), encoding='ascii') as stream:
        io_values = dict(line.strip().split(': ', 1) for line in stream if ': ' in line)
    return {'cpu_seconds': cpu_seconds, 'threads': threads,
            'read_bytes': int(io_values['read_bytes']),
            'write_bytes': int(io_values['write_bytes'])}



def percentile(values, fraction):
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    low = int(position)
    high = min(low + 1, len(ordered) - 1)
    return round(ordered[low] + (ordered[high] - ordered[low]) * (position - low), 3)


def request(url, timeout):
    start = time.perf_counter()
    try:
        with urllib.request.urlopen(url, timeout=timeout) as response:
            code = response.status
            size = len(response.read())
        error = None
    except urllib.error.HTTPError as exc:
        code = exc.code
        size = len(exc.read())
        error = None
    except (OSError, ValueError) as exc:
        code = None
        size = 0
        error = type(exc).__name__
    return ((time.perf_counter() - start) * 1000.0, code, size, error)


def varied_grid_urls(url, count, jitter, seed):
    """Generate reproducible grid footprints without altering dimensions or dataset."""
    if jitter == 0:
        return [url] * count
    parsed = urllib.parse.urlsplit(url)
    if not parsed.path.endswith('/analysis/grid'):
        raise ValueError('--grid-jitter-degrees requires an /analysis/grid URL')
    query = urllib.parse.parse_qsl(parsed.query, keep_blank_values=True)
    params = dict(query)
    try:
        west, east = float(params['west']), float(params['east'])
        south, north = float(params['south']), float(params['north'])
    except (KeyError, ValueError) as exc:
        raise ValueError('Grid variation requires valid west/east/south/north') from exc
    if not (-180 <= west < east <= 180 and -90 <= south < north <= 90):
        raise ValueError('Grid coordinates are outside valid geographic bounds')
    rng = random.Random(seed)
    urls = []
    for _ in range(count):
        dx = rng.uniform(-jitter, jitter)
        dy = rng.uniform(-jitter, jitter)
        dx = min(max(dx, -180 - west), 180 - east)
        dy = min(max(dy, -90 - south), 90 - north)
        changes = {'west': west + dx, 'east': east + dx,
                   'south': south + dy, 'north': north + dy}
        updated = [(key, format(changes[key], '.10f') if key in changes else value)
                   for key, value in query]
        urls.append(urllib.parse.urlunsplit((parsed.scheme, parsed.netloc, parsed.path,
                                            urllib.parse.urlencode(updated), parsed.fragment)))
    return urls


def run_stage(name, url, count, concurrency, timeout, pid, interval, recovery,
              urls=None, round_number=None):
    samples = []
    stop = threading.Event()
    monitor_errors = []

    def monitor():
        while not stop.is_set():
            try:
                samples.append(rss_kib(pid))
            except (OSError, ValueError, RuntimeError) as exc:
                monitor_errors.append(str(exc))
                break
            stop.wait(interval)

    before = rss_kib(pid)
    counters_before = process_counters(pid)
    watcher = threading.Thread(target=monitor, daemon=True)
    watcher.start()
    started = time.perf_counter()
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as pool:
            results = list(pool.map(lambda target: request(target, timeout), urls or [url] * count))
    finally:
        stop.set()
        watcher.join()
    elapsed = time.perf_counter() - started
    counters_after = process_counters(pid)
    immediate = rss_kib(pid)
    if recovery:
        time.sleep(recovery)
    after = rss_kib(pid)
    if monitor_errors:
        raise RuntimeError('RSS monitor failed: {}'.format(monitor_errors[0]))
    times = [item[0] for item in results]
    statuses = Counter(str(item[1]) for item in results)
    errors = Counter(item[3] for item in results if item[3])
    return {
        'name': name, 'url': url, 'requests': count, 'concurrency': concurrency,
        'round': round_number, 'distinct_request_urls': len(set(urls)) if urls else 1,
        'elapsed_s': round(elapsed, 3),
        'throughput_requests_per_s': round(count / elapsed, 2),
        'latency_ms': {
            'mean': round(statistics.mean(times), 3),
            'p50': percentile(times, .5), 'p95': percentile(times, .95),
            'p99': percentile(times, .99), 'max': round(max(times), 3),
        },
        'process_activity': {
            'cpu_seconds': round(counters_after['cpu_seconds'] - counters_before['cpu_seconds'], 3),
            'cpu_cores_used': round((counters_after['cpu_seconds'] - counters_before['cpu_seconds']) / elapsed, 3),
            'disk_read_bytes': counters_after['read_bytes'] - counters_before['read_bytes'],
            'disk_write_bytes': counters_after['write_bytes'] - counters_before['write_bytes'],
            'threads_before': counters_before['threads'],
            'threads_after': counters_after['threads'],
        },
        'statuses': dict(sorted(statuses.items())),
        'errors': dict(sorted(errors.items())),
        'unexpected_http_statuses': {code: value for code, value in sorted(statuses.items())
                                     if code != '200'},
        'passed': not errors and statuses == {'200': count},
        'received_bytes': sum(item[2] for item in results),
        'rss_kib': {
            'before': before, 'peak_observed': max(samples + [before, immediate]),
            'immediately_after': immediate, 'after_recovery': after,
            'recovery_delta_from_before': after - before,
        },
    }


def safe_url(url, allow_remote):
    parsed = urllib.parse.urlsplit(url)
    if parsed.scheme not in ('http', 'https') or not parsed.hostname or parsed.username or parsed.password:
        raise ValueError('Requires an absolute HTTP(S) URL without embedded credentials')
    if not allow_remote and parsed.hostname not in ('127.0.0.1', 'localhost', '::1'):
        raise ValueError('Only loopback targets permitted without --allow-remote')
    return url


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True, help='PID of the dedicated server under test')
    parser.add_argument('--tile-url', help='Known cached normalized terrain tile URL')
    parser.add_argument('--grid-url', help='Known working grid URL; may trigger source downloads')
    parser.add_argument('--requests', type=int, default=100)
    parser.add_argument('--concurrencies', default='1,4,16', help='Comma-separated positive worker counts')
    parser.add_argument('--timeout', type=float, default=30.0)
    parser.add_argument('--sample-interval', type=float, default=0.02)
    parser.add_argument('--recovery', type=float, default=1.0)
    parser.add_argument('--allow-remote', action='store_true')
    parser.add_argument('--endurance-rounds', type=int, default=1,
                        help='Repeat each workload/concurrency stage (default: 1)')
    parser.add_argument('--grid-jitter-degrees', type=float, default=0.0,
                        help='Deterministically shift grid footprints by up to this many degrees')
    parser.add_argument('--seed', type=int, default=12345, help='Grid variation random seed')
    parser.add_argument('--max-rss-growth-kib', type=int,
                        help='Fail if recovered server RSS grows by more than this across rounds')
    parser.add_argument('--output', help='Optional path to write JSON report')
    args = parser.parse_args()
    try:
        if args.pid <= 0 or args.requests <= 0 or args.timeout <= 0 or args.sample_interval <= 0 or args.recovery < 0:
            raise ValueError('PID, request count, timeout and sample interval must be positive; recovery nonnegative')
        if args.endurance_rounds <= 0 or not 0 <= args.grid_jitter_degrees <= 180:
            raise ValueError('Endurance rounds must be positive and grid jitter within 0..180 degrees')
        if args.max_rss_growth_kib is not None and args.max_rss_growth_kib < 0:
            raise ValueError('Maximum RSS growth must be nonnegative')
        levels = [int(value.strip()) for value in args.concurrencies.split(',')]
        if not levels or any(level <= 0 for level in levels):
            raise ValueError('Concurrency levels must be positive')
        endpoints = [('cached_tile', args.tile_url), ('grid', args.grid_url)]
        endpoints = [(name, safe_url(url, args.allow_remote)) for name, url in endpoints if url]
        if not endpoints:
            raise ValueError('Specify --tile-url, --grid-url, or both')
        for name, url in endpoints:
            if name == 'grid':
                varied_grid_urls(url, 1, args.grid_jitter_degrees, args.seed)
        rss_kib(args.pid)
    except (ValueError, OSError, RuntimeError) as exc:
        parser.error(str(exc))

    report = {'pid': args.pid, 'sample_interval_s': args.sample_interval,
              'recovery_s': args.recovery, 'endurance_rounds': args.endurance_rounds,
              'grid_jitter_degrees': args.grid_jitter_degrees, 'seed': args.seed,
              'stages': [], 'endurance_summary': []}
    failed = False
    try:
        for name, url in endpoints:
            for concurrency in levels:
                baseline = None
                first_recovered = None
                last_recovered = None
                peaks = []
                for round_number in range(1, args.endurance_rounds + 1):
                    urls = None
                    if name == 'grid' and args.grid_jitter_degrees:
                        urls = varied_grid_urls(url, args.requests, args.grid_jitter_degrees,
                                                args.seed + round_number - 1)
                    stage = run_stage(name, url, args.requests, concurrency, args.timeout,
                                      args.pid, args.sample_interval, args.recovery,
                                      urls=urls, round_number=round_number)
                    report['stages'].append(stage)
                    current = stage['rss_kib']['after_recovery']
                    if baseline is None:
                        baseline = stage['rss_kib']['before']
                        first_recovered = current
                    last_recovered = current
                    peaks.append(stage['rss_kib']['peak_observed'])
                    if not stage['passed']:
                        failed = True
                    print('{} c={} round={}/{} p95={}ms peak={}KiB statuses={} errors={} {}'.format(
                        name, concurrency, round_number, args.endurance_rounds,
                        stage['latency_ms']['p95'], stage['rss_kib']['peak_observed'],
                        stage['statuses'], stage['errors'],
                        'PASS' if stage['passed'] else 'FAIL'), flush=True)
                    print('  cpu={:.2f} cores disk_read={}KiB disk_write={}KiB threads={}→{}'.format(
                        stage['process_activity']['cpu_cores_used'],
                        stage['process_activity']['disk_read_bytes'] // 1024,
                        stage['process_activity']['disk_write_bytes'] // 1024,
                        stage['process_activity']['threads_before'],
                        stage['process_activity']['threads_after']), flush=True)
                # First-round warm-up can legitimately retain allocator pages; report both
                # full-run growth and additional growth after the first round.
                growth = last_recovered - baseline
                post_warmup_growth = last_recovered - first_recovered
                over_limit = (args.max_rss_growth_kib is not None and
                              post_warmup_growth > args.max_rss_growth_kib)
                summary = {'name': name, 'concurrency': concurrency,
                           'rounds': args.endurance_rounds,
                           'before_first_round_kib': baseline,
                           'after_first_round_kib': first_recovered,
                           'after_last_round_kib': last_recovered,
                           'total_recovered_rss_growth_kib': growth,
                           'post_first_round_rss_growth_kib': post_warmup_growth,
                           'peak_observed_kib': max(peaks),
                           'max_rss_growth_kib': args.max_rss_growth_kib,
                           'rss_growth_limit_exceeded': over_limit}
                report['endurance_summary'].append(summary)
                if over_limit:
                    failed = True
                    print('FAIL {} c={} RSS growth after first round: {} KiB (limit {} KiB)'.format(
                        name, concurrency, post_warmup_growth, args.max_rss_growth_kib),
                        flush=True)
    except (OSError, RuntimeError) as exc:
        report['fatal_error'] = str(exc)
        failed = True

    report['passed'] = not failed
    encoded = json.dumps(report, indent=2) + '\n'
    if args.output:
        with open(args.output, 'w', encoding='utf-8') as stream:
            stream.write(encoded)
    else:
        print(encoded)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
