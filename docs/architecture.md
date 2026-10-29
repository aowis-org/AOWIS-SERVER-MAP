# Architecture and resource limits

AOWIS Server Map has two independent data services behind one Qt 6 HTTP listener: `MapTiles` for raster imagery and `TerrainData` for elevation and analysis grids. Both are available through the server and C++ library boundaries. This document describes the **running system**—the work it performs, resource limits and operational constraints—not a development plan.

## Request and storage flow

```text
Trusted client / TLS reverse proxy
                |
          Qt QHttpServer
          validation + auth
                |
          asynchronous reply
                |
      +---------+------------+
      |                      |
   MapTiles                TerrainData
      |                      |
 RAM cache                normalized .aowterrain
      | miss                 | miss / corrupt
 permanent/disk           native Copernicus COG
      | miss                 | miss
 asynchronous raster      remote COG acquisition
 upstream request            |
      |                    source-ready handoff
 bounded atomic             |
 disk persistence      CPU sampling/normalization
```

The HTTP event loop does not wait synchronously for remote terrain downloads or large grid sampling. Pending responses use Qt `QFuture`/`QPromise`; requests for the **same** terrain tile, elevation or grid key share one in-flight computation while retaining separate HTTP waiters. Different keys compete for admission. Programmatic C++ consumers can call `TerrainData` directly without loopback HTTP.

## Raster tile path

- Successful raster tiles are cached in a **256 MiB `QCache`**, with permanent-data precedence; cold HTTP cache reads use a dedicated asynchronous worker pool. `MapTiles::getTile()` remains synchronous for library callers, while `getTileForHttp()` is asynchronous.
- Disk reads: **2 workers, 64 queued/running unique operations and 64 MiB reserved payload bytes**. Concurrent reads for the same tile coalesce; oversized files are rejected before being loaded.
- Disk writes: **2 workers, 64 queued/running operations and 64 MiB pending bytes**. Publication is atomic. Under write pressure, a fetched tile can still be returned while disk persistence is skipped.
- Remote raster downloads: **32 active by default**, with additional per-origin bounds and request deduplication. A separate `server/max_pending_requests` limit applies to pending HTTP responses.
- Deletion coordinates with pending reads and overlapping writes to avoid resurrecting deleted entries. An active overlapping disk commit can delay a deletion request.

Raster tile files are retained indefinitely until explicitly invalidated; there is no TTL, automatic revalidation, background refresh or disk quota. Responses explicitly marked `Cache-Control: no-store` or `private` are not stored in the shared cache. Raster cache locations use:

```text
<raster-cache>/maptiles/<provider-uuid>/<z>/<x>/<y>.png
```

An upstream URL change can create a fingerprinted variant under that provider's cache tree. Administrator-provisioned permanent tiles under `<permanent-data>/maptiles/...` are not deleted by the cache DELETE route.

## Terrain request scheduling

The server owns two independent Qt thread pools:

- **Cached/CPU processing pool** (default 4 threads): normalized cache hits, normalization and CPU sampling after acquisition, and terrain maintenance.
- **Remote-capable acquisition pool** (default 2 threads): source discovery, interprocess source locks, HTTP downloads of missing Copernicus COGs and any acquisition-stage waiting.

Terrain tile, point-elevation and grid work can **hand off** from the remote-capable pool to CPU processing after sources are ready. Acquiring sources does not occupy a CPU processing worker; the remote worker is released at handoff. In offline-only mode, cached work can execute in the CPU pool. Elevation fallback to coarser cached tiles is CPU-only; missing coarser native source regions are not proactively downloaded from CPU workers.

Admission limits are not the same as thread counts:

| Resource | Default limit | Scope |
| --- | ---: | --- |
| Terrain processing workers | 4 | Cached/CPU processing concurrency |
| Remote-capable workers | 2 | Source-acquisition concurrency |
| Total terrain jobs | 128 | Distinct queued/running jobs, including CPU and remote stages |
| Remote-capable terrain admission | 64 | Additional limit within global jobs (`max(1, maximum_jobs / 2)`) |
| Pending HTTP requests | 2048 | Configurable server response/waiter budget |
| Distinct grid result + serialization reservations | 64 MiB | 8 bytes/sample estimated for float32 vector and float32 HTTP bytes |
| Analysis grid dimensions | 512 × 512 | Maximum rows and columns of one request |

An identical in-flight terrain request reuses its existing computation and does not consume another terrain job or grid-byte reservation; it **does** have its own HTTP waiter. Requests exceeding admission return `503 Service Unavailable` (terrain admission can supply `Retry-After: 1`). The two worker pools and remote admission protect cached processing from being monopolized by slow upstream acquisitions.

## Terrain memory accounting

| Resource | Limit | Behavior when full |
| --- | ---: | --- |
| Decoded TIFF blocks in a reader | 32 MiB per open raster | Evict oldest retained blocks; a single block larger than the limit fails |
| Retained decoded TIFF blocks across readers | 128 MiB shared | Decode a needed block without caching it |
| Concurrent temporary TIFF decode buffers | 128 MiB shared | Reject the source read rather than allocate beyond the budget |
| Distinct grid result and serialized payloads | 64 MiB shared | Reject grid work when admission cannot reserve estimated buffers |
| Normalized `.aowterrain` disk read | Fixed v1 format maximum (8,642 bytes) | Reject oversized/corrupt cache entries before reading |

Grid-byte admission accounts for **both** the float result vector and the serialized HTTP payload (8 bytes per sample). The temporary float vector is released before HTTP delivery; response bytes may be implicitly shared by Qt's copy-on-write containers for single-flight fan-out. Shared decoder reservations are released on eviction, read completion, error and teardown paths.

**These budgets do not constitute a hard process-wide RSS ceiling.** libtiff internal allocations, Qt networking buffers, native file mappings and other intermediate allocations can also use memory. A memory cache being full is not equivalent to an out-of-memory failure, and allocator-retained pages may remain resident after request completion.

## Terrain cache and source acquisition

```text
<terrain-cache>/
  providers/copernicus/glo30/       Native 1° COG cells, shared across normalized tiles
  normalized/v1/copernicus-glo30/   <z>/<x>/<y>.aowterrain
  offline-packages/                Reserved; automatic package mounting unavailable
```

A normalized tile request checks its validated local cache, acquires the provider-native 1° COGs needed by that tile and normalizes a 65 × 65 heightfield on demand (Copernicus supports normalized zooms 8–14). Each normalized tile and native source uses cache-fill locking, including cross-process `QLockFile` protection for shared source directories. Normalized files and complete native COGs are atomically published; corrupted or mismatched normalized tiles are not served and can be repaired from source data.

Copernicus downloads use independent defaults: **30 s connection/initial response**, **120 s inactivity**, **1800 s absolute transfer deadline**, plus a 256 MiB source-size cap. Slow transfers that continue making progress can extend beyond the inactivity duration but not the absolute deadline. Source lock stale detection is set beyond the maximum download duration to avoid two processes treating a slow legitimate download as abandoned.

For interrupted transfers with a **strong ETag and known total size**, the downloader can keep a validated `.partial` prefix and metadata. On retry, `Range`/`If-Range` must match the saved entity and byte positions; a complete `200 OK` can replace the attempt when the server ignores Range. Corrupt or inconsistent partial state is rejected. Incomplete files are never published as final `.tif` sources; there is no scheduled background retry.

Disabling remote downloads prevents only **new** source acquisition. Cached native COGs can still generate missing normalized tiles, and normalized files remain directly readable. A download-coverage rule is checked against the requested XYZ tile; a native COG can cover a larger area than the allowed tile footprint.

## Client disconnects, cancellation and shutdown

The server tracks accepted TCP connections for asynchronous terrain tile, point-elevation and grid response waiters. Disconnecting a client frees its pending HTTP waiter while preserving shared work for other connected clients.

When **the last waiter leaves**, its in-flight job is removed from the single-flight registry and receives a per-job cancellation token. A replacement request starts a fresh flight, subject to admission; a late completion from the old flight cannot consume the new flight's waiters. Source lock waits and active Copernicus downloads check per-job cancellation; an orphaned active transfer can be aborted without canceling unrelated source downloads. Queued CPU work checks cancellation before starting. **CPU processing already executing is not preempted mid-operation.**

Shutdown stops listening, cancels upstream activity, completes/drains outstanding promises and joins both terrain pools before destroying terrain dependencies. A native operation that fails to respond to cancellation can still delay shutdown. Reservations are released as jobs exit, including canceled and superseded grid jobs.

## Verification and benchmarking

Use the project-root scripts; tests run on loopback fixtures, not public Copernicus or tile services.

```bash
./compile_linux_tests.sh --debug
./compile_linux_tests.sh --release
./tools/qualify_release.sh
./tools/qualify_release.sh --sanitizers
```

Release qualification builds the production executable and registered native tests, verifies configuration generation and validation without opening the HTTP listener, runs longer terrain disconnect/shutdown regression cycles, and repeats the lifecycle test in fresh processes. Sanitizer mode uses a separate AddressSanitizer/UndefinedBehaviorSanitizer build. Logs live under `build-linux-qualification/`; an unsupported sanitizer runtime is a failed gate, not a silent skip. ThreadSanitizer is not part of this script.

To focus on lifecycle races:

```bash
AOWIS_TERRAIN_LIFECYCLE_REPETITIONS=20 \
  ctest --test-dir build-linux-tests-debug \
  -R '^aowis-server-map-terrain-singleflight-http$' --output-on-failure

ctest --test-dir build-linux-tests-debug \
  -R '^aowis-server-map-terrain-singleflight-http$' \
  --repeat until-fail:100 --output-on-failure
```

`tools/benchmark_http.py` measures client-observed request throughput, status distribution and latency against a **controlled local endpoint**. It creates a new connection per request, so results are not persistent-HTTP connection benchmarks. The Linux terrain benchmark `tools/benchmark_terrain_memory.py` samples the **specified** server's `/proc/<pid>` metrics: observed peak/recovered RSS, CPU time, physical disk I/O and thread counts, alongside HTTP throughput and latency. It fails when a workload unexpectedly returns a non-200 response. An example against a known-good **local** terrain tile and grid:

```bash
SERVER_PID=$(pgrep -f '/build-linux/aowis-server-map$')
TILE_URL='http://127.0.0.1:8122/terrain/v1/copernicus-glo30/8/133/86.aowterrain'
GRID_URL='http://127.0.0.1:8122/terrain/v1/analysis/grid?dataset=copernicus-glo30&west=8.10&south=50.10&east=8.11&north=50.11&columns=512&rows=512'

python3 tools/benchmark_terrain_memory.py \
  --pid "$SERVER_PID" \
  --tile-url "$TILE_URL" --grid-url "$GRID_URL" \
  --requests 100 --concurrencies 1,4,16 \
  --output terrain-memory.json
```

The example locations require available source data on your server; confirm `200` before benchmarking and warm caches if measuring cache hits. For sustained varied grids, add `--endurance-rounds 10 --grid-jitter-degrees 0.001 --seed 12345 --max-rss-growth-kib 16384`, staying inside source coverage. RSS sampling may miss short spikes and measures the entire process; concurrent builds or unrelated requests confound performance results. **Do not load-test public upstream servers.**

For event-loop timer-scheduling warnings during normal runs, opt in with `AOWIS_MAP_EVENT_LOOP_WARN_MS=100`; warnings are not HTTP latency measurements.

## Integration and deployment boundaries

- The current supported Internet terrain dataset is **Copernicus GLO-30**; its source is a DSM in EGM2008, not bare-earth hydraulic elevations. Map does not perform cross-datum conversion.
- Analysis grids use original GeoTIFF source-pixel sampling, rather than normalized renderer tiles. The two products have different precision and sampling semantics.
- `PUT /map/v1/providers` currently has **no API-key authentication**. Protect it at the network/proxy level; the read and delete keys cover different endpoint groups.
- Bind QHttpServer to loopback or a protected private interface. Terminate public HTTPS at Apache or another suitable reverse proxy. CORS is not authorization.

See [Configuration](configuration.md), [HTTP API](http-api.md), [terrain sources](terrain-providers.md), [elevation contract](terrain-elevation-contract.md), and [systemd](../configs/systemd/README.md)/[Apache](../configs/apache/README.md) deployment.
