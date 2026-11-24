# Cache storage accounting

MAP records ordinary raster and normalized terrain cache access recency in a
SQLite index. The tile bytes and existing on-disk layouts are unchanged.
This is the foundation for storage limits and eviction, not an eviction policy.
No files are automatically removed by this feature.

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
files; use real filesystem inventory for any future eviction decision.

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

## Next steps

Enforce configurable byte limits with a high/low-water policy, protection for
explicit offline regions and active requests, and a reconciliation mechanism
for stale or missing index rows. Native terrain source eviction needs its own
coordination with readers, partial downloads and shared source dependencies.
Expose management via the C++ and REST APIs only after safe eviction is in place.
