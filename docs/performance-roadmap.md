# Performance roadmap — QHttpServer

## Milestone 0: reproducible baseline

The tool `tools/benchmark_http.py` measures throughput, response status counts,
network errors and client-observed p50/p95/p99 latency. It is intentionally
client-side only and uses Python's standard library.

Start the map server with a local test configuration and populate an authorized
cache fixture. For example, substitute an existing cached endpoint and run:

```sh
python3 tools/benchmark_http.py 'http://127.0.0.1:PORT/EXISTING/TILE' --requests 1000 --concurrency 16
```

Run identical benchmarks against the direct local listener and, separately,
Apache. Record server configuration, hardware, tile size, cache state,
concurrency and CPU/RSS alongside JSON output. Test RAM-warm, disk-warm,
cold-cache (using a **local mock upstream**, never a public tile provider),
and mixed-map/terrain loads. Results are not interchangeable between runs
with different request mixes.

The harness makes **one new HTTP connection per request** (urllib); it does not
measure keep-alive reuse, event-loop lag, server memory, or per-stage queue
times. It is a starting baseline, not a complete load-test suite. Keep tests
local by default, and avoid benchmark traffic against public providers.

## Milestone 1: safe cache fast path

`MapTiles::getTile()` validates provider and coordinates first, then returns
known memory-cache hits before resolving the provider's filesystem cache path.
When a permanent cache directory is configured and the key is not known to
be permanent, the existing permanent-file precedence is preserved.

## Remaining milestones (not yet implemented)

1. Audit pending-response completion, disconnects, cancellation and shutdown.
2. Add low-overhead internal counters and event-loop latency instrumentation.
3. Offload bounded disk reads/writes while preserving atomic cache updates.
4. Create a bounded dedicated terrain scheduler and isolate upstream I/O.
5. Add terrain single-flight and fair work scheduling.
6. Implement independent queue and memory budgets and overload handling.
7. Improve upstream failure isolation and retry/negative-cache policy.
8. Validate HTTP caching/Apache behavior and operational metrics.
9. Run race, failure, stress and endurance tests before production release.

No unmeasured performance improvement or production readiness is claimed by
this initial patch.


## Native test runner and loop-stall diagnostics

Run the complete native test suite from the repository root:

```bash
./compile_linux_tests.sh
./compile_linux_tests.sh --debug
./compile_linux_tests.sh --release
./compile_linux_tests.sh --both
```

The runner configures CMake with Ninja and `BUILD_TESTING=ON`, builds the
`aowis-server-map-tests` aggregate target, ensures that CTest has at least one
test labeled `aowis`, and runs all such tests with failure output.
With `AOWIS_SERVER_MAP_BUILD_SERVER=OFF`, the provider-state test is omitted.

Event-loop stall diagnostics are opt-in. To log event-loop scheduling delays
exceeding 100 ms, start the server with:

```bash
AOWIS_MAP_EVENT_LOOP_WARN_MS=100 ./build-linux/aowis-server-map
```

This measurement detects delays between timer callbacks; it is not a precise
per-request latency metric. Logs are throttled to one warning every 30 seconds,
and the monitor is disabled when the environment variable is absent.

## Lifecycle increment: pending map responses on shutdown

The server now closes its listener, detaches map download callbacks, cancels upstream map downloads, and completes outstanding map-tile response promises with HTTP 503 in its destructor. New pending registrations are rejected once shutdown begins. This does not yet cover terrain worker lifetime and does not imply full graceful shutdown support.

After successful application, `tools/apply_patch.sh` runs `tools/zip_map_server.sh` automatically to regenerate the adjacent `map.zip` source archive. If that export fails, the apply command reports failure even though the files have already been extracted.

## Lifecycle increment: terrain worker ownership

All five asynchronous terrain HTTP routes now submit work to a dedicated
`QThreadPool` owned by `Server`, rather than the global Qt thread pool. Its
initial maximum concurrency is `max(1, idealThreadCount / 2)`.

During shutdown, the listener is closed, upstream cancellation is requested,
and the terrain pool is joined before `TerrainData` is destroyed. This prevents
terrain route lambdas from retaining a dangling `Server`/`TerrainData` pointer.

This is *not* yet a bounded queue or job-admission mechanism: `QThreadPool`
can still queue work without an application-level limit, and an upstream
operation that ignores cancellation can delay shutdown. These remain next
steps. Re-run `./compile_linux_tests.sh --both` after applying the patch.

## Terrain job admission (Phase 01 continuation)

All five asynchronous terrain handlers now share a nonblocking admission budget.
`[terrain] maximum_jobs=128` limits the combined number of running and queued
terrain requests; when full, the frontend responds immediately with HTTP 503
`Service Unavailable` and `Retry-After: 1`. Admission tokens are released
when worker tasks exit, including through exceptions. This is a count limit,
not yet a byte-weighted memory budget, single-flight scheduler, or timeout.
The CTest `aowis-server-map-terrain-admission` exercises saturation, token
release and concurrent acquisition.

### Cache write isolation (Phase 03, first increment)

Completed upstream map tiles now use a dedicated two-thread cache-write pool for
`QSaveFile` persistence. Up to 64 pending writes are admitted; beyond that,
responses still deliver the downloaded bytes but persistence is skipped rather
than allocating an unbounded write backlog. The downloaded tile is published
to RAM immediately to avoid duplicate downloads. The `tileReady` signal remains
deferred until the disk-save attempt finishes for admitted writes. Cache
removal serializes against pending writes. This deliberately does **not** yet
move cold disk-cache reads or provider-path initialization off the event loop.

### Build and persistence regression gate

The `aowis-server-map-tests` aggregate target now also compiles the production
`aowis-server-map-server` executable when server builds are enabled. Both Debug
and Release test runs thus catch Qt HTTP API compilation errors in production
handlers, even when no HTTP integration test exercises them. The cache-reuse
test also checks that `tileReady` is not emitted before an admitted asynchronous
cache write has published its complete file. This is a correctness gate for the
cache-I/O work; asynchronous cold-cache reads remain a subsequent milestone.

### Phase 03: bounded cache-write memory

Asynchronous map cache persistence is limited to 64 outstanding writes **and**
64 MiB of queued/in-flight payload bytes. When either budget is exhausted,
the tile is returned without persistent storage; this protects the HTTP
event loop against an oversized cache-write backlog. These are independent
of the 256 MiB memory-tile cache budget. Cold disk cache reads remain
synchronous and are the next separate migration step.

### Phase 03: asynchronous cold HTTP cache reads

The HTTP tile endpoint now calls `MapTiles::getTileForHttp()`. Warm RAM
hits return immediately; established cold disk hits use a dedicated two-worker
read pool. Up to 64 unique cold reads can be outstanding; simultaneous
requests for the same canonical tile share one disk read. The original
`getTile()` API remains synchronous for existing native callers and tests.

Files are read in a worker, but cache publication, signal delivery and network
fallback remain in the owning Qt event-loop thread. A configuration/deletion
generation prevents publishing file contents obtained from an obsolete cache
configuration. When the read queue is saturated the HTTP route returns its
existing busy response. Missing paths still follow the existing download path.

**Follow-up:** move provider cache-path metadata initialization and permanent
cache existence probes off the event loop as well, and evaluate byte-budgeted
read admission under representative large-file workloads.

### Cold cache read memory admission

Asynchronous cold-cache reads are limited to 64 unique jobs and a combined
64 MiB reserved file payload budget. Disk files larger than this limit are
rejected with the existing busy status instead of being loaded into memory.
The read worker also bounds the actual read if a file changes after admission.


### Phase 03: invalidated cold reads

Cache deletion and provider reconfiguration now cancel pending asynchronous
cache reads rather than interpreting them as ordinary disk misses. This avoids
accidental upstream refetches and stale response delivery after administrative
invalidation. Genuine read failures with an unchanged cache generation retain
the existing fallback behavior. The cache reuse regression test covers deletion
while a read completion is queued.

### Phase 03: nonblocking cache-read invalidation during deletion

Administrative tile deletion no longer waits for all cache-read workers. Each
read operates on its own file handle and the generation guard on completion
cancels results invalidated by a deletion. Cache-write workers still require a
completion barrier before filesystem removal; eliminating that barrier safely
requires a follow-up per-tile write/delete coordinator. Invalid deletion
parameters are now rejected before mutating the cache generation, preserving
unrelated pending reads. The existing cache-reuse regression exercises both
pending-read invalidation and invalid deletion input.

### Phase 03: narrow cache-write deletion barrier

Cache deletion no longer waits for outstanding asynchronous cache writes
when none of their canonical tile keys overlap the requested provider,
zoom, and coordinate ranges. A matching write still triggers the existing
write-pool drain before filesystem removal to prevent a deleted tile from
reappearing. This is a limited optimization rather than fully asynchronous
delete handling: overlapping deletion may still block while unrelated
workers finish, and the filesystem traversal itself remains synchronous.

## Cache write/delete coordination

Cache writes now carry independent synchronization states. Deletion marks only
writes overlapping the requested tile range as invalidated and synchronizes
with their per-write mutexes before deleting the files. In-flight writes that
have already begun publishing complete before deletion removes their files;
queued invalidated writes skip publication. This replaces the global cache
write-pool `waitForDone()` barrier, avoiding waits on unrelated writes.
Deletion may still wait for an *overlapping* write in progress; making that
operation fully nonblocking requires an asynchronous deletion API and distinct
HTTP completion semantics. Test this behavior under heavy write/delete traffic
before considering it production-qualified.
