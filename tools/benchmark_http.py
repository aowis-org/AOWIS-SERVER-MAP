#!/usr/bin/env python3
"""Repeatable HTTP latency probe. Standard library only; no external traffic by default."""
import argparse
import concurrent.futures
import json
import statistics
import time
import urllib.error
import urllib.parse
import urllib.request
from collections import Counter


def percentile(values, fraction):
    ordered = sorted(values)
    index = (len(ordered) - 1) * fraction
    lower = int(index)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (index - lower)


def probe(url, timeout):
    started = time.perf_counter()
    try:
        with urllib.request.urlopen(url, timeout=timeout) as response:
            status = response.status
            payload_size = len(response.read())
        error = None
    except urllib.error.HTTPError as exc:
        status = exc.code
        payload_size = len(exc.read())
        error = None
    except (OSError, ValueError) as exc:
        status = None
        payload_size = 0
        error = type(exc).__name__
    return (time.perf_counter() - started) * 1000.0, status, payload_size, error


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('url', help='HTTP(S) endpoint to benchmark')
    parser.add_argument('--requests', type=int, default=1000)
    parser.add_argument('--concurrency', type=int, default=16)
    parser.add_argument('--timeout', type=float, default=10.0)
    parser.add_argument('--allow-remote', action='store_true',
                        help='Permit non-loopback targets (use only with authorization)')
    args = parser.parse_args()
    parsed = urllib.parse.urlsplit(args.url)
    if parsed.scheme not in ('http', 'https') or not parsed.hostname:
        parser.error('URL must be an absolute HTTP(S) URL')
    if not args.allow_remote and parsed.hostname not in ('localhost', '127.0.0.1', '::1'):
        parser.error('Only loopback targets allowed without --allow-remote')
    if args.requests < 1 or args.concurrency < 1 or args.timeout <= 0:
        parser.error('requests, concurrency and timeout must be positive')
    started = time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.concurrency) as pool:
        results = list(pool.map(lambda _: probe(args.url, args.timeout), range(args.requests)))
    elapsed = time.perf_counter() - started
    latencies = [r[0] for r in results]
    summary = {
        'url': args.url, 'requests': args.requests,
        'concurrency': args.concurrency, 'elapsed_s': round(elapsed, 3),
        'throughput_requests_per_s': round(args.requests / elapsed, 2),
        'latency_ms': {
            'mean': round(statistics.mean(latencies), 3),
            'p50': round(percentile(latencies, .50), 3),
            'p95': round(percentile(latencies, .95), 3),
            'p99': round(percentile(latencies, .99), 3),
            'max': round(max(latencies), 3),
        },
        'statuses': dict(sorted(Counter(str(r[1]) for r in results).items())),
        'errors': dict(sorted(Counter(r[3] for r in results if r[3]).items())),
        'received_bytes': sum(r[2] for r in results),
    }
    print(json.dumps(summary, indent=2))
    return 1 if summary['errors'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
