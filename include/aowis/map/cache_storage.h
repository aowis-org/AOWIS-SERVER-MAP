#ifndef AOWIS_MAP_CACHE_STORAGE_H
#define AOWIS_MAP_CACHE_STORAGE_H

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

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

struct TerrainCacheStorageStatistics
{
    CacheStorageStatistics normalized;
    CacheStorageStatistics sources;
};

// Potentially expensive: call from a worker, not a rendering/event-loop thread.
CacheStorageStatistics scanCacheStorage(const std::filesystem::path &root,
                                       CacheStorageKind kind);

// Durable, best-effort last-use hints for ordinary cache entries. Keys are
// relative to the managed root; raster keys omit the image extension so format
// changes and legacy .png files share the same identity. No tile payloads are
// stored in the database. Missing rows mean unknown recency, not zero age.
// No disk I/O on recordAccess(); flushes are batched on a worker. The database
// is not an authority for file existence or size and never deletes tile files.
class CacheAccessIndex final
{
public:
    explicit CacheAccessIndex(const std::filesystem::path &root);
    ~CacheAccessIndex();
    CacheAccessIndex(const CacheAccessIndex &) = delete;
    CacheAccessIndex &operator=(const CacheAccessIndex &) = delete;

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
