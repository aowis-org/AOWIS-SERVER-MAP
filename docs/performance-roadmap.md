# Performance, concurrency and operations

This document describes the **current implementation** of AOWIS-SERVER-MAP. It is not a patch history or a claim that particular throughput targets have been measured. For endpoints, authentication, cache locations, provider configuration and deployment, start with the [README](../README.md).

## Architecture at a glance

```text
Apache / trusted local clients
          |
    QHttpServer (Qt event loop)
          |
   route validation / admission / QFuture responses
          |
          +-- raster tiles: RAM -> async disk read -> async upstream download
          |                   \-> bounded async atomic disk write
          |
          +-- terrain tiles / elevation / grids
                   |  shared HTTP waiter budget
                   |  per-operation single-flight registry
                   |  bounded unique-job admission
                   |  grid output-byte reservations
                   v
              separate cached / remote-capable pools
                   |
             TerrainData + Copernicus provider
                   |
            normalized/native caches -> upstream COG
```

The frontend stays event-driven: waiting HTTP tile responses use `QFuture`/`QPromise` rather than dedicating a worker to every waiter. Network downloads for raster imagery use Qt's asynchronous network APIs. Terrain jobs run in two independently bounded server-owned pools: normalized-cache tile hits and deletion work use the CPU pool, while potentially remote-fetching work uses a two-thread remote-capable pool. Copernicus source acquisition still uses a worker-local event loop, but **normalized terrain tile cache misses now use a two-stage handoff**: the remote pool acquires source GeoTIFFs and enqueues CPU normalization on the processing pool without waiting. Elevation and grid workloads still combine acquisition and processing in remote workers; the pipeline is not yet universal.

## Raster tile delivery

Raster tiles are validated by provider ID and XYZ address, then looked up in a 256 MiB `QCache` memory cache (with permanent-data precedence preserved), permanent data and filesystem cache. The HTTP path uses `getTileForHttp()` so cold disk reads can complete asynchronously; the public library's `getTile()` remains synchronous for its existing callers.

* Cold HTTP reads use a two-thread pool, at most **64 distinct queued/running reads**, and at most **64 MiB total reserved payload bytes**. Concurrent HTTP reads for the same tile are coalesced. Oversized cached files are rejected before loading them into memory.
* Downloaded raster tiles are persisted through a separate two-thread pool, with at most **64 queued/running writes** and **64 MiB pending payload bytes**. Writes use atomic file publication. If the write queue or byte budget is full, the fetched tile can still be delivered but disk persistence is skipped.
* Normal map downloads are asynchronous with a global active-download limit (default **32**), additional per-origin restrictions and deduplication of identical downloads. Limits for pending HTTP responses and queued upstream work must not be confused with active download capacity.
* Cache deletion invalidates stale asynchronous disk reads. Pending writes overlapping a deletion are coordinated individually: queued invalidated writes do not publish deleted files, and active overlapping writes are synchronized before deletion. This does not make every deletion call nonblocking; an active overlapping commit can still delay it.
* Cache-first clients can request `?delivery=cache-first` or `X-AOWIS-Tile-Delivery: cache-first`. An uncached request in progress may return **202 Accepted**, instead of holding the HTTP response open. See the README for endpoint and response behavior.

Cached tiles and already available terrain remain usable without external network access. Raster and terrain have independent remote-fetch switches and download-coverage rules. Cache retention and licensing policy are documented in the README.

## Terrain execution and concurrency

Asynchronous terrain routes use separate cached-work and remote-capable `QThreadPool` instances. The remote-capable pool defaults to two workers. Remote-capable jobs have a separate admission cap of half the global terrain job limit (64 of 128 by default), as well as sharing the global budget. This reserves capacity for cache-hit and maintenance work when slow remote requests accumulate. Current defaults:

| Budget | Default | Meaning |
| --- | ---: | --- |
| `terrain/maximum_workers` | 4 | Cached terrain processing and maintenance pool size; clamped to the job budget |
| `terrain/maximum_remote_workers` | 2 | Independent remote-capable pool size, clamped to the shared job budget; tile source acquisition and combined elevation/grid requests occupy these workers |
| `terrain/maximum_jobs` | 128 | Total admitted unique/ordinary terrain tasks, including queued tasks |
| Remote-capable job admission | 64 | At most `max(1, maximum_jobs / 2)` remote-capable queued or running jobs, also counted against the global budget |
| `server/max_pending_requests` | 2048 | Upper bound used for pending HTTP response waiters; shared by terrain tile, elevation and grid single-flight requests |
| In-flight terrain grid reservation | 64 MiB | Estimated float32 output bytes for distinct admitted grid jobs |
| Analysis grid dimensions | 512 × 512 | Maximum samples per HTTP analysis-grid request |

Identical concurrent requests to normalized terrain tiles, point elevation, and analysis grids share an in-flight computation. The request identity includes the relevant dataset, coordinates/bounds, dimensions, and vertical datum where applicable. Each waiting client gets its own HTTP response; identical requests do not consume additional terrain job admission slots or an additional grid-byte reservation. Different requests still compete for admission. Remote requests can reach their own admission limit while global slots remain for cached terrain. At capacity, the server responds **503 Service Unavailable** (with `Retry-After: 1` in admission paths).

The grid byte budget accounts for estimated float-buffer output, **not** every intermediate decoder allocation, native raster buffer, waiting response or provider-cache allocation. These still need production memory profiling. Terrain cache deletion and upstream cancellation have their own endpoint semantics; do not assume every route is coalesced.

On shutdown, the server stops listening, requests upstream cancellation, drains outstanding response promises and joins both terrain pools before destroying terrain dependencies. A blocked operation that does not respond to cancellation may still delay shutdown. Connection-disconnect tracking, absolute job deadlines and cancellation semantics merit further stress testing.

## Slow-network and offline behavior

Copernicus GLO-30 uses native source COGs, normalized `.aowterrain` cache files and interprocess source locks. Download timeouts are independently configurable:

```ini
[terrain]
maximum_remote_workers=2
connect_timeout_seconds=30
inactivity_timeout_seconds=120
download_deadline_seconds=1800
```

The connection limit bounds waiting for response progress, the inactivity limit detects stalled transfers, and the **30-minute absolute deadline** allows slow but continuously progressing downloads to continue much longer than two minutes. All three are bounded; these are not unlimited downloads. Source-lock acquisition can be canceled while waiting. Incomplete downloads are not published as complete cache objects.

Interrupted Copernicus COG downloads can resume from validated partial files when a strong ETag and known total length are available. Transfers without suitable validators restart from byte zero. Despite the available timeout controls, Copernicus downloads occupy remote-capable workers until they finish, fail or are canceled. Normalized cached tile hits use the separate processing pool. Grid requests now acquire the exact native source cells touched by their sample centres on the remote-capable pool, then enqueue CPU-only sampling without waiting; grid byte reservations and global job admission remain held across the handoff. Elevation requests still combine source acquisition and sampling on remote-capable workers when remote fetching is enabled.

To enforce offline use while retaining cached data, configure `terrain/remote_fetch_enabled=false` independently from `downloads/remote_fetch_enabled=false` for imagery. Restricting download coverage does **not** forbid reading already cached data outside that coverage.

## Building and testing

From the repository root:

```bash
./compile_linux_tests.sh --debug   # normal development gate, includes production server build
./compile_linux_tests.sh --release # performance/release-mode checks
./compile_linux_tests.sh --both    # major changes and release qualification
./compile_linux.sh --no-run        # build without trying to bind the default port
```

`compile_linux_tests.sh` builds the `aowis-server-map-tests` aggregate target (including the production executable when enabled), verifies that CTest registers tests labeled `aowis`, and runs them with failure output. The present suite contains **14 registered native tests**, including asynchronous cache reuse, provider state, terrain source lock/cancellation, HTTP single-flight concurrency and overload, terrain admission, and grid byte admission. The 14/14 Debug result was reported on 2026-10-28; it is not a benchmark or proof of race freedom.

`compile_linux.sh` starts the executable unless `--no-run` is passed. If another instance already listens at `127.0.0.1:8122`, a bind error after a successful compilation is expected and is not a compilation failure.

### HTTP benchmark harness

Use `tools/benchmark_http.py` against a **controlled local instance and fixture**:

```bash
python3 tools/benchmark_http.py 'http://127.0.0.1:8122/EXISTING/TILE' --requests 1000 --concurrency 16
```

It reports client-observed throughput, status codes, network errors and latency percentiles. It opens a fresh HTTP connection per request, so it does **not** measure persistent-connection reuse, internal event-loop lag, memory, queue residency or stage-level latency. Always record the hardware, cache warmth, dataset, tile size, configuration and whether traffic passed through Apache. Use a local mock upstream for cold-cache load tests; **do not load-test public tile providers**.

Opt-in event-loop delay warnings:

```bash
AOWIS_MAP_EVENT_LOOP_WARN_MS=100 ./build-linux/aowis-server-map
```

These are timer-scheduling delay warnings, not precise request latency measurements. Output is throttled; no warnings are produced by default.

## Deployment and safe operation

Use QHttpServer on loopback/private networks, with Apache or another appropriately configured TLS reverse proxy for public access. Both read and delete API keys have separate purposes. **The provider catalog PUT endpoint currently has no API-key authentication**: limit it to trusted clients at the network/proxy boundary. See [systemd](../configs/systemd/README.md) and [Apache](../configs/apache/README.md) deployment guidance.

The supplied `tools/apply_patch.sh` accepts `aowis-server-map-*.zip` archives with repository-root-relative contents; after a successful application it calls `tools/zip_map_server.sh` to generate a fresh adjacent `map.zip` snapshot. The test runner is the normal validation gate after a patch.

## Further engineering work (not implemented)

These are **open engineering goals**, not descriptions of the present server:

1. Extend the implemented tile acquisition-to-CPU handoff to elevation and analysis grids; refine I/O concurrency, backpressure and fairness.
2. Strengthen resumable source downloads with integration coverage for unstable real-world upstreams, bounded partial-file retention, and optional retry scheduling.
3. Account for all terrain intermediate/native raster memory and response payloads, not just grid output reservations.
4. Add comprehensive per-stage metrics, disconnect-aware waiter cleanup, retry/circuit-breaker policies and operational dashboards where justified.
5. Run sustained mixed workload, slow-upstream, shutdown, client-disconnect, sanitizer, and endurance tests; record repeatable before/after benchmarks before claiming performance gains.

The architectural objective is to keep cached requests responsive under pressure while bounding work and resource usage. There are no published throughput or latency guarantees yet.
