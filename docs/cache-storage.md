# Cache storage and eviction

MAP records ordinary raster and normalized terrain cache access recency in a
SQLite index. The tile bytes and existing on-disk layouts are unchanged.
Optional soft storage limits enable background LRU eviction. By default both
limits are zero (unlimited), so existing installations do not delete any files
automatically.

## Storage areas

| Area | Cache path | Access tracking |
| --- | --- | --- |
| Raster imagery | `<cache-base>/maptiles/` | Disk hits, RAM hits and completed writes |
| Normalized terrain | `<terrain-root>/normalized/` | Validated reads and completed writes |
| Native terrain sources | `<terrain-root>/providers/` | Storage accounting only |

Each of the first two areas has its own `.aowis-cache-access-v1.sqlite3` database.
Keys are relative to that cache root, including provider URL variants and terrain
normalization revisions. Raster keys omit the extension, so JPEG, PNG and WebP
represent the same logical tile in a given provider variant. Permanent imagery
and offline terrain package roots are not part of the ordinary cache index or
statistics (configure them separately from the ordinary cache roots).

An access updates an in-memory timestamp. A worker merges up to 16,384 pending
unique keys per batch in a transaction, triggered by 1,024 pending keys or the
30-second interval. Normal shutdown flushes remaining updates. Recording an
access performs no database writes or file reads. The index does not use OS
access timestamps. An abrupt exit can lose unflushed recency; this is a cache
hint, not project data. Database errors do not prevent serving or saving tiles.
Failed batches are retried; overload can drop hints rather than grow memory
without bound. `CacheAccessIndex::lastError()` and `droppedAccessCount()` expose
these diagnostics. Existing caches need no migration: unknown recency remains
unknown until a successful access. The index can also contain rows for deleted
files; eviction uses the real filesystem inventory and prunes rows for the
logical tiles it deletes.

## C++ API

- `MapTiles::cacheStorageStatistics()` reports ordinary raster storage.
- `TerrainData::cacheStorageStatistics()` reports normalized terrain and native
  source storage separately.
- `CacheStorageStatistics` contains payload and auxiliary file counts and
  logical byte sizes, available filesystem bytes when known, and scan errors.
  Auxiliary files include index databases, metadata, and unfinished downloads.
  Native terrain payloads currently include completed TIFF files.
- `MapTiles::flushCacheAccessIndex()` and `TerrainData::flushCacheAccessIndex()`
  explicitly synchronize pending access metadata.
- `MapTiles::setCacheStoragePolicy()` and `TerrainData::setCacheStoragePolicy()`
  enable or disable each independent limit. The policy uses bytes and a
  cleanup-target percentage (1–99); an invalid policy or overlap with permanent
  data is rejected. Calls can wait for ongoing cleanup; use from initialization
  or a suitable management thread (respect MapTiles thread affinity).
- `lastCacheCleanup()` reports the last pass's before/remaining bytes, deleted
  file and byte counts, skipped files, errors, shared-root status and whether the
  target was reached. Counts describe that pass, not lifetime counters.

Statistics walk the actual directory tree on demand, including existing and
inactive provider variants. Run these operations on a worker thread, not the
GUI or HTTP event loop. Scans skip symlinks inside the cache tree; results are
not a transactional snapshot under concurrent writes/deletes. File sizes are
logical bytes, not allocated filesystem blocks. Available space is reported per
filesystem; do not add two values when both caches share a volume. A nonzero
scan error count indicates potentially incomplete results.

## Build dependency

SQLite >= 3.24 is required (native C API, not Qt SQL). Debian packaging and the
Linux dependency installers include `libsqlite3-dev`; Arch uses `sqlite`.
Embedded consumers must provide a target-architecture SQLite library and
headers to CMake (`SQLite::SQLite3` via `FindSQLite3`). Android/iOS and Windows
cross-build images must include it before updating to this MAP revision.

## Automatic cleanup

For a dedicated server, configure the administrator-owned INI and restart:

```ini
[storage]
; 10 GiB ordinary imagery; 2 GiB normalized terrain
raster_cache_maximum_bytes=10737418240
normalized_terrain_cache_maximum_bytes=2147483648
cache_cleanup_target_percent=85
cache_prefer_overview_tiles=true
```

The worker checks limits at startup/policy change and roughly every 30 seconds.
A cache exceeding its limit is trimmed toward 85% of that limit (configurable).
If active files or the bounded selection batch prevent reaching that target, a
later pass continues toward it. At most 32,768 candidates are retained per pass;
large caches may take multiple passes. Selection uses the persistent last-access
index, including pending access updates. Unknown legacy entries are considered
older than known entries; file modification time breaks ties. If overview
preference is enabled, zoom levels 0–8 are considered only after higher zoom
levels. This is a preference, not a permanent pin.

Only recognized XYZ raster or normalized terrain payloads are eligible. Raster
HTTP sidecars belonging to removed files are removed too. The worker checks the
file's size, modification time, symlink status, active request leases and newer
access hints immediately before unlinking. Normalized fills using `.lock` files
are also skipped. Files recently published or read during a pass can be skipped
until the next pass. RAM caches remain valid and are not covered by these disk
limits. If metadata cannot be read reliably, cleanup fails closed.

Current MAP instances hold a shared advisory ownership lock per cache root.
Eviction requires exclusive ownership and pauses while another participating
instance is using the root. The lock files are not stale-marker files: never
remove them while MAP is running. Use local filesystems with working advisory
locks, private writable cache roots, and do not share an eviction-enabled root
with older MAP versions or external writers that do not participate in the
ownership protocol. Permanent data must use separate roots. An overlapping
permanent path disables or rejects automatic eviction.

These are **soft payload limits**, not filesystem quotas. Native GeoTIFFs,
permanent/offline package data, SQLite files, partial files and other auxiliary
files are not evicted. Writes continue during cleanup and can temporarily exceed
the limit. An unreachable target is reported instead of deleting unrelated or
active data. Disk-free-space admission, explicit offline-region pins, and native
source retention are not yet implemented. Available disk space is still exposed
by storage statistics; do not set a payload limit equal to total free space.

## Next steps

Add persistent protected tile/region references and a disk-free-space admission
policy, then expose management via the MAP settings API and REST. Native source
eviction needs independent coordination with readers, partial downloads and
shared source dependencies. GUI and Mobile can then use these common APIs.
