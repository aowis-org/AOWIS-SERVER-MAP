#ifndef AOWIS_MAP_CACHE_STORAGE_H
#define AOWIS_MAP_CACHE_STORAGE_H

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Aowis::Map
{

enum class CacheStorageKind { Raster, NormalizedTerrain, TerrainSources };

// Logical file sizes, not allocated filesystem blocks. The scan does not
// follow symlinks. Concurrent writes/deletes can change the result during a scan.
struct CacheStorageStatistics
{
    std::uint64_t payload_bytes = 0;
    std::uint64_t payload_files = 0;
    std::uint64_t auxiliary_bytes = 0;
    std::uint64_t auxiliary_files = 0;
    std::uint64_t scan_errors = 0;
    std::optional<std::uint64_t> available_bytes;
};

// Soft payload-byte limit per cache root. Zero disables eviction. Native
// sources, permanent data and auxiliary files are not subject to this limit.
struct CacheStoragePolicy
{
    std::uint64_t maximum_bytes = 0;
    unsigned cleanup_target_percent = 85;
    bool prefer_overview_tiles = true;
};

// Inclusive XYZ rectangle within a specific on-disk provider variant or
// terrain format/revision/dataset namespace. A single tile is a 1x1 range.
// Ranges also protect matching files published after the group is saved.
struct CacheTileRange
{
    std::string source_namespace;
    int zoom = 0;
    int min_x = 0;
    int max_x = 0;
    int min_y = 0;
    int max_y = 0;
    bool operator==(const CacheTileRange &) const = default;
};

struct CacheProtectionGroup
{
    std::string id;
    std::vector<CacheTileRange> ranges;
};

// Convert a WGS84 rectangle into compact Web Mercator XYZ ranges. West > east
// crosses the antimeridian. Both corner tiles are included. Coordinates must
// be within the Web Mercator domain, zooms 0..30. No downloads are performed.
std::optional<std::vector<CacheTileRange>> cacheRegionTileRanges(
    const std::string &source_namespace, double west, double south,
    double east, double north, int min_zoom, int max_zoom);

struct CacheCleanupResult
{
    std::uint64_t before_bytes = 0;
    std::uint64_t remaining_bytes = 0;
    std::uint64_t deleted_files = 0;
    std::uint64_t deleted_bytes = 0;
    std::uint64_t skipped_files = 0;
    // Protected payloads encountered during an over-limit selection scan.
    // Includes all encodings; overlapping groups are counted only once.
    std::uint64_t protected_files = 0;
    std::uint64_t protected_bytes = 0;
    std::uint64_t errors = 0;
    bool target_reached = true;
    bool shared_root = false; // another cooperating MAP instance owns the root, or lock unavailable
};

struct TerrainCacheStorageStatistics
{
    CacheStorageStatistics normalized;
    CacheStorageStatistics sources;
};

// Reject overlapping ordinary and permanent roots before enabling eviction.
// Unknown/canonicalization errors conservatively count as overlap.
bool cacheStorageRootsOverlap(const std::filesystem::path &first,
                              const std::filesystem::path &second);

// Potentially expensive: call from a worker, not a rendering/event-loop thread.
CacheStorageStatistics scanCacheStorage(const std::filesystem::path &root,
                                       CacheStorageKind kind);

// Durable, best-effort last-use hints for ordinary cache entries. Keys are
// relative to the managed root; raster keys omit the image extension so format
// changes and legacy .png files share the same identity. No tile payloads are
// stored in the database. Missing rows mean unknown recency, not zero age.
// No disk I/O on recordAccess(); flushes are batched on a worker. The database
// is not an authority for file existence or size. Optional eviction uses actual files.
class CacheAccessIndex final
{
public:
    explicit CacheAccessIndex(const std::filesystem::path &root);
    ~CacheAccessIndex();
    CacheAccessIndex(const CacheAccessIndex &) = delete;
    CacheAccessIndex &operator=(const CacheAccessIndex &) = delete;

    // A copyable, cross-thread lifetime token. Keep it until all reads or
    // publications for this key finish. Eviction never removes leased files.
    using PayloadLease = std::shared_ptr<void>;
    PayloadLease leasePayload(const std::string &relative_key) const;
    // Policy is opt-in and applies to an exclusively owned ordinary cache
    // root. No automatic eviction of native source files is supported.
    bool setStoragePolicy(CacheStorageKind kind, const CacheStoragePolicy &policy);
    CacheCleanupResult lastCleanup() const;
    // Synchronous maintenance for tests/administration; call on a worker.
    CacheCleanupResult cleanupNow();

    // Persistent protection, not best-effort access hints. A successful
    // replace atomically replaces all ranges in a named group. An empty
    // range list is rejected; use removeProtectionGroup to unprotect it.
    // Groups may overlap and cover missing/future tiles. These calls can wait
    // for maintenance and perform disk I/O; use a suitable management thread.
    // False/nullopt means failure, not an empty group list.
    bool replaceProtectionGroup(CacheStorageKind kind, const CacheProtectionGroup &group);
    bool removeProtectionGroup(const std::string &id);
    std::optional<std::vector<CacheProtectionGroup>> protectionGroups(CacheStorageKind kind) const;

    void recordAccess(const std::string &relative_key);
    // Explicit synchronization point for tests, shutdown and maintenance.
    // Returns false if persistence failed. Normal requests remain usable.
    bool flush();
    // Reads durable and pending recency. May perform disk I/O; use a worker.
    std::optional<std::int64_t> lastAccess(const std::string &relative_key) const;
    std::uint64_t droppedAccessCount() const;
    std::string lastError() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Aowis::Map
#endif
