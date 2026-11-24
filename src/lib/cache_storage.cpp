#include <aowis/map/cache_storage.h>

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <charconv>
#include <exception>
#include <cmath>
#include <numbers>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace Aowis::Map
{
namespace
{
constexpr std::size_t FlushThreshold = 1024;
constexpr std::size_t MaximumPending = 16384;
constexpr std::size_t MaximumEvictionCandidates = 32768;
constexpr std::size_t MaximumProtectionRangesPerGroup = 16384;
constexpr std::chrono::seconds FlushInterval(30);
constexpr std::chrono::seconds RetryInterval(30);
constexpr const char *IndexFilename = ".aowis-cache-access-v1.sqlite3";
// Cooperative ownership locks are separate from the SQLite index. Every
// participating instance holds a shared owner lock during its lifetime; only
// a sole owner may evict. A second lock serializes shared->exclusive upgrades
// (flock conversion is not atomic). Never remove these lock files.
class AdvisoryFileLock
{
public:
    ~AdvisoryFileLock()
    {
#ifdef _WIN32
        if (this->handle != INVALID_HANDLE_VALUE) CloseHandle(this->handle);
#else
        if (this->handle >= 0) close(this->handle);
#endif
    }
    bool openFile(const std::filesystem::path &path)
    {
#ifdef _WIN32
        if (this->handle == INVALID_HANDLE_VALUE)
            this->handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        return this->handle != INVALID_HANDLE_VALUE;
#else
        if (this->handle < 0) this->handle = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        return this->handle >= 0;
#endif
    }
    bool lock(bool exclusive, bool wait)
    {
#ifdef _WIN32
        OVERLAPPED offset{};
        return LockFileEx(this->handle, (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0) |
            (wait ? 0 : LOCKFILE_FAIL_IMMEDIATELY), 0, 1, 0, &offset) != 0;
#else
        int result;
        do { result = flock(this->handle, (exclusive ? LOCK_EX : LOCK_SH) | (wait ? 0 : LOCK_NB)); }
        while (result < 0 && errno == EINTR);
        return result == 0;
#endif
    }
    void unlock()
    {
#ifdef _WIN32
        OVERLAPPED offset{};
        UnlockFileEx(this->handle, 0, 1, 0, &offset);
#else
        flock(this->handle, LOCK_UN);
#endif
    }
private:
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
#else
    int handle = -1;
#endif
};

class CacheRootOwnership
{
public:
    // Caller serializes access with PayloadGate::mutex.
    bool ensure(const std::filesystem::path &root)
    {
        if (this->held) return true;
        if (root.empty()) return false;
        std::error_code error;
        std::filesystem::create_directories(root, error);
        if (error || !std::filesystem::is_directory(std::filesystem::symlink_status(root, error)) || error)
            return false;
        this->held = this->owners.openFile(root / ".aowis-cache-owners-v1.lock") &&
                     this->coordinator.openFile(root / ".aowis-cache-cleanup-v1.lock") &&
                     this->owners.lock(false, true);
        return this->held;
    }
    bool beginCleanup()
    {
        if (!this->held || !this->coordinator.lock(true, false)) return false;
        this->owners.unlock();
        if (this->owners.lock(true, false)) return true;
        this->held = this->owners.lock(false, true);
        this->coordinator.unlock();
        return false;
    }
    void endCleanup()
    {
        this->owners.unlock();
        this->held = this->owners.lock(false, true);
        this->coordinator.unlock();
    }
private:
    AdvisoryFileLock owners;
    AdvisoryFileLock coordinator;
    bool held = false;
};

struct PayloadGate
{
    std::mutex mutex;
    CacheRootOwnership ownership;
    std::unordered_map<std::string, std::size_t> active;
};

struct PayloadUse
{
    std::shared_ptr<PayloadGate> gate;
    std::string key;
    PayloadUse(const std::shared_ptr<PayloadGate> &gate, const std::string &key, const std::filesystem::path &root)
        : gate(gate), key(key)
    {
        std::lock_guard<std::mutex> lock(this->gate->mutex);
        this->gate->ownership.ensure(root);
        ++this->gate->active[this->key];
    }
    ~PayloadUse()
    {
        std::lock_guard<std::mutex> lock(this->gate->mutex);
        std::unordered_map<std::string, std::size_t>::iterator it = this->gate->active.find(this->key);
        if (--it->second == 0) this->gate->active.erase(it);
    }
};

struct ExclusiveCleanup
{
    std::shared_ptr<PayloadGate> gate;
    bool acquired = false;
    explicit ExclusiveCleanup(const std::shared_ptr<PayloadGate> &gate) : gate(gate)
    {
        std::lock_guard<std::mutex> lock(this->gate->mutex);
        this->acquired = this->gate->ownership.beginCleanup();
    }
    ~ExclusiveCleanup()
    {
        if (this->acquired)
        {
            std::lock_guard<std::mutex> lock(this->gate->mutex);
            this->gate->ownership.endCleanup();
        }
    }
};

using AccessMap = std::unordered_map<std::string, std::int64_t>;

std::int64_t nowMilliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool validKey(const std::string &key)
{
    if (key.empty() || key.size() > 4096 || key.find('\0') != std::string::npos ||
        key.find('\\') != std::string::npos || key.find(':') != std::string::npos)
        return false;
    // Keys are portable slash-delimited relative identifiers, not OS paths.
    std::size_t start = 0;
    while (start <= key.size())
    {
        const std::size_t end = key.find('/', start);
        const std::size_t length = (end == std::string::npos ? key.size() : end) - start;
        if (length == 0 || (length == 1 && key[start] == '.') ||
            (length == 2 && key[start] == '.' && key[start + 1] == '.')) return false;
        if (end == std::string::npos) return true;
        start = end + 1;
    }
    return false;
}

struct Database
{
    sqlite3 *handle = nullptr;
    ~Database() { if (this->handle != nullptr) sqlite3_close(this->handle); }
    bool open(const std::filesystem::path &path, int flags)
    {
        const std::u8string utf8 = path.u8string();
        if (sqlite3_open_v2(reinterpret_cast<const char *>(utf8.c_str()),
                           &this->handle, flags, nullptr) != SQLITE_OK) return false;
        sqlite3_busy_timeout(this->handle, 2000);
        return true;
    }
    bool execute(const char *sql) const
    { return sqlite3_exec(this->handle, sql, nullptr, nullptr, nullptr) == SQLITE_OK; }
    std::string error() const
    { return this->handle != nullptr ? sqlite3_errmsg(this->handle) : "Cannot open cache access index"; }
};

struct Statement
{
    sqlite3_stmt *handle = nullptr;
    ~Statement() { if (this->handle != nullptr) sqlite3_finalize(this->handle); }
};

bool payloadFile(const std::filesystem::path &path, CacheStorageKind kind)
{
    const std::filesystem::path extension = path.extension();
    if (kind == CacheStorageKind::Raster)
        return extension == ".png" || extension == ".jpg" || extension == ".webp";
    if (kind == CacheStorageKind::NormalizedTerrain)
        return extension == ".aowterrain";
    // Completed provider-native files; partials, locks and metadata are auxiliary.
    return extension == ".tif" || extension == ".tiff";
}
// Only recognize actual XYZ payload paths, not arbitrary image files placed
// in the cache tree. Inactive raster variants and old terrain revisions count.
struct TileIdentity
{
    std::string source_namespace;
    int zoom = 0;
    int x = 0;
    int y = 0;
};
bool tileKey(const std::filesystem::path &relative, CacheStorageKind kind,
             std::string &key, TileIdentity &tile)
{
    std::filesystem::path base = relative;
    base.replace_extension();
    std::vector<std::string> parts;
    for (const std::filesystem::path &part : base)
    {
        const std::u8string utf8 = part.u8string();
        parts.emplace_back(reinterpret_cast<const char *>(utf8.data()), utf8.size());
    }
    if (kind == CacheStorageKind::Raster)
    {
        if (parts.size() != 4 && (parts.size() != 6 || parts[1] != "variants")) return false;
    }
    else if (kind == CacheStorageKind::NormalizedTerrain)
    {
        if (parts.size() != 6 || !parts[0].starts_with('v') || !parts[1].starts_with('r')) return false;
    }
    else return false;
    int coordinates[3] = {};
    for (std::size_t i = 0; i < 3; ++i)
    {
        const std::string &part = parts[parts.size() - 3 + i];
        const std::from_chars_result parsed = std::from_chars(part.data(), part.data() + part.size(), coordinates[i]);
        if (parsed.ec != std::errc() || parsed.ptr != part.data() + part.size() || coordinates[i] < 0)
            return false;
    }
    tile.zoom = coordinates[0];
    tile.x = coordinates[1];
    tile.y = coordinates[2];
    if (tile.zoom > 30 || tile.x >= (1 << tile.zoom) || tile.y >= (1 << tile.zoom)) return false;
    tile.source_namespace.clear();
    for (std::size_t i = 0; i < parts.size() - 3; ++i)
    {
        if (i) tile.source_namespace += '/';
        tile.source_namespace += parts[i];
    }
    const std::u8string utf8 = (kind == CacheStorageKind::Raster ? base : relative).generic_u8string();
    key.assign(reinterpret_cast<const char *>(utf8.data()), utf8.size());
    return validKey(key);
}

bool validGroupId(const std::string &id)
{
    return !id.empty() && id.size() <= 256 &&
        std::none_of(id.begin(), id.end(), [](unsigned char c) { return c < 32 || c == 127; });
}

bool validRange(CacheStorageKind kind, const CacheTileRange &range)
{
    if (!validKey(range.source_namespace) || range.zoom < 0 || range.zoom > 30 ||
        range.min_x < 0 || range.min_y < 0 || range.min_x > range.max_x || range.min_y > range.max_y ||
        range.max_x >= (1 << range.zoom) || range.max_y >= (1 << range.zoom)) return false;
    // Match tileKey's namespace grammar without filesystem conversion (which
    // may throw on untrusted non-UTF-8 input on Windows).
    const std::string &source = range.source_namespace;
    const std::size_t first = source.find('/');
    if (first == std::string::npos) return kind == CacheStorageKind::Raster;
    const std::size_t second = source.find('/', first + 1);
    if (second == std::string::npos || source.find('/', second + 1) != std::string::npos) return false;
    if (kind == CacheStorageKind::Raster)
        return source.compare(first + 1, second - first - 1, "variants") == 0;
    return kind == CacheStorageKind::NormalizedTerrain && source.front() == 'v' && source[first + 1] == 'r';
}

// The access index predates protection. Migrate version 0 transactionally.
// Once marked migrated, a missing protection table is an error, not an
// empty set of pins. The index is now durable protection metadata as well.
bool openProtectionDatabase(Database &database, const std::filesystem::path &path)
{
    if (!database.open(path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) ||
        !database.execute("PRAGMA synchronous=FULL") || !database.execute("BEGIN IMMEDIATE")) return false;
    int version;
    {
        Statement query;
        if (sqlite3_prepare_v2(database.handle, "PRAGMA user_version", -1, &query.handle, nullptr) != SQLITE_OK ||
            sqlite3_step(query.handle) != SQLITE_ROW) return false;
        version = sqlite3_column_int(query.handle, 0);
    }
    if (version == 0)
    {
        if (!database.execute("CREATE TABLE IF NOT EXISTS tile_access ("
                              "tile_key TEXT PRIMARY KEY NOT NULL, last_access_ms INTEGER NOT NULL) WITHOUT ROWID") ||
            !database.execute("CREATE TABLE cache_protection ("
                "group_id TEXT NOT NULL, source_namespace TEXT NOT NULL, zoom INTEGER NOT NULL, "
                "min_x INTEGER NOT NULL, max_x INTEGER NOT NULL, min_y INTEGER NOT NULL, max_y INTEGER NOT NULL, "
                "PRIMARY KEY(group_id,source_namespace,zoom,min_x,max_x,min_y,max_y), "
                "CHECK(zoom BETWEEN 0 AND 30 AND min_x>=0 AND min_y>=0 AND "
                "max_x>=min_x AND max_y>=min_y AND max_x<(1<<zoom) AND max_y<(1<<zoom))) WITHOUT ROWID") ||
            !database.execute("CREATE INDEX cache_protection_lookup ON cache_protection(source_namespace,zoom,min_x)") ||
            !database.execute("PRAGMA user_version=1")) return false;
    }
    else if (version != 1) return false;
    // Verify the table's required columns even when no tiles match.
    {
        Statement check;
        if (sqlite3_prepare_v2(database.handle,
            "SELECT group_id,source_namespace,zoom,min_x,max_x,min_y,max_y FROM cache_protection LIMIT 0",
            -1, &check.handle, nullptr) != SQLITE_OK || sqlite3_step(check.handle) != SQLITE_DONE) return false;
    }
    return database.execute("COMMIT");
}

bool bindText(sqlite3_stmt *statement, int column, const std::string &value)
{
    return sqlite3_bind_text(statement, column, value.data(), int(value.size()), SQLITE_TRANSIENT) == SQLITE_OK;
}

std::optional<bool> tileProtected(Statement &query, const TileIdentity &tile)
{
    if (!bindText(query.handle, 1, tile.source_namespace) ||
        sqlite3_bind_int(query.handle, 2, tile.zoom) != SQLITE_OK ||
        sqlite3_bind_int(query.handle, 3, tile.x) != SQLITE_OK ||
        sqlite3_bind_int(query.handle, 4, tile.y) != SQLITE_OK) return std::nullopt;
    const int status = sqlite3_step(query.handle);
    if ((status != SQLITE_ROW && status != SQLITE_DONE) || sqlite3_reset(query.handle) != SQLITE_OK)
        return std::nullopt;
    return status == SQLITE_ROW;
}

// Recheck the entire path before removing anything. The ordinary cache root
// must be private to its owning MAP instance, not modified by another process.
bool safeRegularFile(const std::filesystem::path &root, const std::filesystem::path &relative)
{
    std::error_code error;
    std::filesystem::path current = root;
    if (!std::filesystem::is_directory(std::filesystem::symlink_status(current, error)) || error) return false;
    for (const std::filesystem::path &part : relative)
    {
        current /= part;
        const std::filesystem::file_status status = std::filesystem::symlink_status(current, error);
        if (error || std::filesystem::is_symlink(status)) return false;
        if (current == root / relative) return std::filesystem::is_regular_file(status);
        if (!std::filesystem::is_directory(status)) return false;
    }
    return false;
}

struct EvictionCandidate
{
    std::filesystem::path relative;
    std::string key;
    std::uint64_t size = 0;
    std::filesystem::file_time_type modified;
    std::optional<std::int64_t> access;
    bool overview = false;
};

bool olderCandidate(const EvictionCandidate &a, const EvictionCandidate &b)
{
    if (a.overview != b.overview) return !a.overview;
    if (a.access != b.access) return a.access < b.access;
    if (a.modified != b.modified) return a.modified < b.modified;
    return a.relative < b.relative;
}

} // namespace

std::optional<std::vector<CacheTileRange>> cacheRegionTileRanges(
    const std::string &source_namespace, double west, double south,
    double east, double north, int min_zoom, int max_zoom)
{
    constexpr double latitude_limit = 85.0511287798066;
    if (!validKey(source_namespace) || !std::isfinite(west) || !std::isfinite(east) ||
        !std::isfinite(south) || !std::isfinite(north) || west < -180 || west > 180 ||
        east < -180 || east > 180 || south < -latitude_limit || north > latitude_limit ||
        south > north || min_zoom < 0 || max_zoom > 30 || min_zoom > max_zoom) return std::nullopt;
    std::vector<CacheTileRange> ranges;
    for (int zoom = min_zoom; zoom <= max_zoom; ++zoom)
    {
        const int count = 1 << zoom;
        const int min_y = std::clamp(int(std::floor((1 - std::asinh(std::tan(north * std::numbers::pi / 180)) /
            std::numbers::pi) * 0.5 * count)), 0, count - 1);
        const int max_y = std::clamp(int(std::floor((1 - std::asinh(std::tan(south * std::numbers::pi / 180)) /
            std::numbers::pi) * 0.5 * count)), 0, count - 1);
        const int min_x = std::clamp(int(std::floor((west + 180) / 360 * count)), 0, count - 1);
        const int max_x = std::clamp(int(std::floor((east + 180) / 360 * count)), 0, count - 1);
        if (west <= east || min_x <= max_x)
            ranges.push_back({source_namespace, zoom, west <= east ? min_x : 0,
                              west <= east ? max_x : count - 1, min_y, max_y});
        else
        {
            ranges.push_back({source_namespace, zoom, min_x, count - 1, min_y, max_y});
            ranges.push_back({source_namespace, zoom, 0, max_x, min_y, max_y});
        }
    }
    return ranges;
}

bool cacheStorageRootsOverlap(const std::filesystem::path &first,
                              const std::filesystem::path &second)
{
    if (first.empty() || second.empty()) return true;
    std::error_code error;
    const std::filesystem::path a = std::filesystem::weakly_canonical(first, error);
    if (error) return true;
    const std::filesystem::path b = std::filesystem::weakly_canonical(second, error);
    if (error) return true;
    const std::pair<std::filesystem::path::const_iterator, std::filesystem::path::const_iterator> common =
        std::mismatch(a.begin(), a.end(), b.begin(), b.end(),
            [](const std::filesystem::path &left, const std::filesystem::path &right) {
#ifdef _WIN32
                return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
                return left == right;
#endif
            });
    return common.first == a.end() || common.second == b.end();
}

CacheStorageStatistics scanCacheStorage(const std::filesystem::path &root,
                                       CacheStorageKind kind)
{
    CacheStorageStatistics result;
    if (root.empty()) { ++result.scan_errors; return result; }
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::symlink_status(root, error);
    if (error && error != std::errc::no_such_file_or_directory)
    { ++result.scan_errors; return result; }
    if (std::filesystem::is_symlink(status) ||
        (std::filesystem::exists(status) && !std::filesystem::is_directory(status)))
    { ++result.scan_errors; return result; }

    // An empty cache need not have created its directory yet.
    error.clear();
    std::filesystem::path existing = std::filesystem::absolute(root, error);
    if (error) { ++result.scan_errors; return result; }
    while (!std::filesystem::exists(existing, error) && !error)
    {
        const std::filesystem::path parent = existing.parent_path();
        if (parent == existing || parent.empty()) break;
        existing = parent;
    }
    if (!error)
    {
        const std::filesystem::space_info space = std::filesystem::space(existing, error);
        if (!error) result.available_bytes = space.available;
    }
    if (error) ++result.scan_errors;
    if (!std::filesystem::is_directory(status)) return result;

    error.clear();
    std::filesystem::recursive_directory_iterator it(root, error);
    const std::filesystem::recursive_directory_iterator end;
    if (error) ++result.scan_errors;
    while (it != end)
    {
        const std::filesystem::directory_entry &entry = *it;
        error.clear();
        const std::filesystem::file_status file_status = entry.symlink_status(error);
        if (!error && std::filesystem::is_regular_file(file_status))
        {
            const std::uintmax_t size = entry.file_size(error);
            if (!error)
            {
                if (payloadFile(entry.path(), kind))
                { result.payload_bytes += size; ++result.payload_files; }
                else
                { result.auxiliary_bytes += size; ++result.auxiliary_files; }
            }
        }
        if (error) ++result.scan_errors;
        error.clear();
        it.increment(error);
        if (error) ++result.scan_errors;
    }
    return result;
}

class CacheAccessIndex::Impl
{
public:
    explicit Impl(const std::filesystem::path &root)
        : root(root), database_path(root / IndexFilename), worker([this]() { this->run(); })
    {
        std::error_code error;
        if (std::filesystem::is_directory(root, error))
        {
            std::lock_guard<std::mutex> lock(this->gate->mutex);
            this->gate->ownership.ensure(root);
        }
    }
    ~Impl()
    {
        {
            std::lock_guard<std::mutex> lock(this->mutex);
            this->stopping = true;
        }
        this->condition.notify_one();
        this->worker.join();
        this->flush();
    }

    void record(const std::string &key)
    {
        if (!validKey(key)) return;
        const std::int64_t timestamp = nowMilliseconds();
        std::lock_guard<std::mutex> lock(this->mutex);
        AccessMap::iterator existing = this->pending.find(key);
        if (existing != this->pending.end())
            existing->second = std::max(existing->second, timestamp);
        else if (this->pending.size() < MaximumPending)
            this->pending.emplace(key, timestamp);
        else
            ++this->dropped;
        if (this->pending.size() >= FlushThreshold) this->condition.notify_one();
    }

    bool flush()
    {
        // Serialize the worker, explicit flush and readers. Never hold the
        // pending mutex during database or filesystem operations.
        std::lock_guard<std::mutex> disk_lock(this->disk_mutex);
        AccessMap batch;
        {
            std::lock_guard<std::mutex> lock(this->mutex);
            batch.swap(this->pending);
        }
        if (batch.empty()) return true;
        std::string error;
        if (this->writeBatch(batch, error))
        {
            std::lock_guard<std::mutex> lock(this->mutex);
            this->last_error.clear();
            return true;
        }
        std::lock_guard<std::mutex> lock(this->mutex);
        this->last_error = error;
        for (const AccessMap::value_type &entry : batch)
        {
            AccessMap::iterator existing = this->pending.find(entry.first);
            if (existing != this->pending.end())
                existing->second = std::max(existing->second, entry.second);
            else if (this->pending.size() < MaximumPending)
                this->pending.emplace(entry);
            else
                ++this->dropped;
        }
        return false;
    }

    std::optional<std::int64_t> lastAccess(const std::string &key) const
    {
        if (!validKey(key)) return std::nullopt;
        std::lock_guard<std::mutex> disk_lock(this->disk_mutex);
        std::optional<std::int64_t> result;
        {
            std::lock_guard<std::mutex> lock(this->mutex);
            const AccessMap::const_iterator entry = this->pending.find(key);
            if (entry != this->pending.end()) result = entry->second;
        }
        Database database;
        if (!database.open(this->database_path, SQLITE_OPEN_READONLY)) return result;
        Statement statement;
        if (sqlite3_prepare_v2(database.handle,
                "SELECT last_access_ms FROM tile_access WHERE tile_key = ?1", -1,
                &statement.handle, nullptr) != SQLITE_OK) return result;
        if (sqlite3_bind_text(statement.handle, 1, key.data(), int(key.size()), SQLITE_TRANSIENT) != SQLITE_OK)
            return result;
        if (sqlite3_step(statement.handle) == SQLITE_ROW)
        {
            const std::int64_t stored = sqlite3_column_int64(statement.handle, 0);
            if (!result || stored > *result) result = stored;
        }
        return result;
    }

    bool writeBatch(const AccessMap &batch, std::string &message) const
    {
        if (this->root.empty()) { message = "Empty cache root"; return false; }
        std::error_code error;
        std::filesystem::create_directories(this->root, error);
        if (error) { message = error.message(); return false; }
        Database database;
        if (!database.open(this->database_path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) ||
            !database.execute("BEGIN IMMEDIATE") ||
            !database.execute("CREATE TABLE IF NOT EXISTS tile_access ("
                              "tile_key TEXT PRIMARY KEY NOT NULL, last_access_ms INTEGER NOT NULL) WITHOUT ROWID"))
        { message = database.error(); return false; }
        Statement statement;
        if (sqlite3_prepare_v2(database.handle,
                "INSERT INTO tile_access(tile_key,last_access_ms) VALUES(?1,?2) "
                "ON CONFLICT(tile_key) DO UPDATE SET last_access_ms = "
                "MAX(tile_access.last_access_ms,excluded.last_access_ms)", -1,
                &statement.handle, nullptr) != SQLITE_OK)
        { message = database.error(); return false; }
        for (const AccessMap::value_type &entry : batch)
        {
            if (sqlite3_bind_text(statement.handle, 1, entry.first.data(), int(entry.first.size()), SQLITE_TRANSIENT) != SQLITE_OK ||
                sqlite3_bind_int64(statement.handle, 2, entry.second) != SQLITE_OK ||
                sqlite3_step(statement.handle) != SQLITE_DONE ||
                sqlite3_reset(statement.handle) != SQLITE_OK)
            { message = database.error(); return false; }
        }
        if (!database.execute("COMMIT")) { message = database.error(); return false; }
        return true;
    }

    bool changeProtection(const std::string &id, const std::vector<CacheTileRange> &ranges)
    {
        // Same-instance maintenance mutex serializes protection commits with
        // the entire eviction pass. Shared ownership blocks another instance
        // from evicting until this index is destroyed.
        std::lock_guard<std::mutex> maintenance(this->cleanup_mutex);
        {
            std::lock_guard<std::mutex> gate_lock(this->gate->mutex);
            if (!this->gate->ownership.ensure(this->root)) return false;
        }
        std::lock_guard<std::mutex> disk_lock(this->disk_mutex);
        Database database;
        if (!openProtectionDatabase(database, this->database_path) || !database.execute("BEGIN IMMEDIATE")) return false;
        Statement erase;
        if (sqlite3_prepare_v2(database.handle, "DELETE FROM cache_protection WHERE group_id=?1", -1,
            &erase.handle, nullptr) != SQLITE_OK || !bindText(erase.handle, 1, id) ||
            sqlite3_step(erase.handle) != SQLITE_DONE) return false;
        if (!ranges.empty())
        {
            Statement insert;
            if (sqlite3_prepare_v2(database.handle,
                "INSERT INTO cache_protection(group_id,source_namespace,zoom,min_x,max_x,min_y,max_y) "
                "VALUES(?1,?2,?3,?4,?5,?6,?7) ON CONFLICT DO NOTHING", -1,
                &insert.handle, nullptr) != SQLITE_OK) return false;
            for (const CacheTileRange &range : ranges)
            {
                if (!bindText(insert.handle, 1, id) || !bindText(insert.handle, 2, range.source_namespace) ||
                    sqlite3_bind_int(insert.handle, 3, range.zoom) != SQLITE_OK ||
                    sqlite3_bind_int(insert.handle, 4, range.min_x) != SQLITE_OK ||
                    sqlite3_bind_int(insert.handle, 5, range.max_x) != SQLITE_OK ||
                    sqlite3_bind_int(insert.handle, 6, range.min_y) != SQLITE_OK ||
                    sqlite3_bind_int(insert.handle, 7, range.max_y) != SQLITE_OK ||
                    sqlite3_step(insert.handle) != SQLITE_DONE || sqlite3_reset(insert.handle) != SQLITE_OK) return false;
            }
        }
        if (!database.execute("COMMIT")) return false;
        // A removal may let a previously blocked cleanup reach its target.
        {
            std::lock_guard<std::mutex> lock(this->mutex);
            this->policy_changed = true;
        }
        this->condition.notify_one();
        return true;
    }

    std::optional<std::vector<CacheProtectionGroup>> protectionGroups(CacheStorageKind kind)
    {
        if (kind == CacheStorageKind::TerrainSources) return std::nullopt;
        std::lock_guard<std::mutex> maintenance(this->cleanup_mutex);
        {
            std::lock_guard<std::mutex> gate_lock(this->gate->mutex);
            if (!this->gate->ownership.ensure(this->root)) return std::nullopt;
        }
        std::lock_guard<std::mutex> disk_lock(this->disk_mutex);
        Database database;
        Statement query;
        if (!openProtectionDatabase(database, this->database_path) || sqlite3_prepare_v2(database.handle,
            "SELECT group_id,source_namespace,zoom,min_x,max_x,min_y,max_y FROM cache_protection "
            "ORDER BY group_id,source_namespace,zoom,min_x,max_x,min_y,max_y", -1,
            &query.handle, nullptr) != SQLITE_OK) return std::nullopt;
        std::vector<CacheProtectionGroup> groups;
        int status;
        while ((status = sqlite3_step(query.handle)) == SQLITE_ROW)
        {
            const unsigned char *id_text = sqlite3_column_text(query.handle, 0);
            const unsigned char *namespace_text = sqlite3_column_text(query.handle, 1);
            if (!id_text || !namespace_text) return std::nullopt;
            const std::string id(reinterpret_cast<const char *>(id_text), sqlite3_column_bytes(query.handle, 0));
            CacheTileRange range{std::string(reinterpret_cast<const char *>(namespace_text), sqlite3_column_bytes(query.handle, 1)),
                sqlite3_column_int(query.handle, 2), sqlite3_column_int(query.handle, 3), sqlite3_column_int(query.handle, 4),
                sqlite3_column_int(query.handle, 5), sqlite3_column_int(query.handle, 6)};
            if (!validGroupId(id) || !validRange(kind, range)) return std::nullopt;
            if (groups.empty() || groups.back().id != id) groups.push_back({id, {}});
            groups.back().ranges.push_back(std::move(range));
        }
        if (status != SQLITE_DONE) return std::nullopt;
        return groups;
    }

    bool setPolicy(CacheStorageKind kind, const CacheStoragePolicy &policy)
    {
        if (kind == CacheStorageKind::TerrainSources || policy.cleanup_target_percent == 0 ||
            policy.cleanup_target_percent >= 100) return false;
        // Serialize policy changes with maintenance, including disabling it.
        std::lock_guard<std::mutex> maintenance(this->cleanup_mutex);
        if (policy.maximum_bytes)
        {
            std::lock_guard<std::mutex> gate_lock(this->gate->mutex);
            if (!this->gate->ownership.ensure(this->root)) return false;
        }
        std::lock_guard<std::mutex> lock(this->mutex);
        this->kind = kind;
        this->policy = policy;
        this->cleanup_in_progress = false;
        this->policy_changed = true;
        this->condition.notify_one();
        return true;
    }

    CacheCleanupResult cleanup()
    {
        std::lock_guard<std::mutex> maintenance(this->cleanup_mutex);
        const CacheStoragePolicy policy = this->policy;
        const CacheStorageKind kind = this->kind;
        CacheCleanupResult result;
        if (policy.maximum_bytes == 0) return this->publish(result);
        const ExclusiveCleanup ownership(this->gate);
        if (!ownership.acquired)
        {
            const CacheStorageStatistics stats = scanCacheStorage(this->root, kind);
            result.before_bytes = result.remaining_bytes = stats.payload_bytes;
            result.errors = stats.scan_errors;
            result.shared_root = true;
            result.target_reached = !result.errors && result.remaining_bytes <= policy.maximum_bytes;
            return this->publish(result);
        }
        // Freeze durable recency during selection; newer access hints remain
        // in pending and are rechecked before unlinking each candidate.
        if (!this->flush()) { ++result.errors; result.target_reached = false; return this->publish(result); }
        std::lock_guard<std::mutex> disk_lock(this->disk_mutex);
        const CacheStorageStatistics stats = scanCacheStorage(this->root, kind);
        result.before_bytes = result.remaining_bytes = stats.payload_bytes;
        result.errors = stats.scan_errors;
        const std::uint64_t target = (policy.maximum_bytes / 100) * policy.cleanup_target_percent +
            (policy.maximum_bytes % 100) * policy.cleanup_target_percent / 100;
        if (result.errors || result.before_bytes <= (this->cleanup_in_progress ? target : policy.maximum_bytes))
        {
            result.target_reached = !result.errors;
            if (result.target_reached) this->cleanup_in_progress = false;
            return this->publish(result);
        }
        this->cleanup_in_progress = true;
        Database database;
        Statement statement;
        std::error_code error;
        Statement protection;
        if (!openProtectionDatabase(database, this->database_path) ||
            sqlite3_prepare_v2(database.handle, "SELECT last_access_ms FROM tile_access WHERE tile_key=?1", -1,
                               &statement.handle, nullptr) != SQLITE_OK ||
            sqlite3_prepare_v2(database.handle, "SELECT 1 FROM cache_protection WHERE source_namespace=?1 AND "
                "zoom=?2 AND min_x<=?3 AND max_x>=?3 AND min_y<=?4 AND max_y>=?4 LIMIT 1", -1,
                &protection.handle, nullptr) != SQLITE_OK)
        { ++result.errors; result.target_reached = false; return this->publish(result); }
        std::uint64_t initial_dropped;
        { std::lock_guard<std::mutex> lock(this->mutex); initial_dropped = this->dropped; }
        std::vector<EvictionCandidate> candidates;
        std::filesystem::recursive_directory_iterator it(this->root, error), end;
        while (!error && it != end)
        {
            const std::filesystem::directory_entry &entry = *it;
            const std::filesystem::file_status status = entry.symlink_status(error);
            if (error) break;
            if (std::filesystem::is_regular_file(status) && payloadFile(entry.path(), kind))
            {
                EvictionCandidate candidate;
                candidate.relative = entry.path().lexically_relative(this->root);
                TileIdentity tile;
                if (tileKey(candidate.relative, kind, candidate.key, tile))
                {
                    candidate.size = entry.file_size(error);
                    if (error) break;
                    const std::optional<bool> protected_tile = tileProtected(protection, tile);
                    if (!protected_tile) { ++result.errors; break; }
                    if (*protected_tile)
                    {
                        ++result.protected_files;
                        result.protected_bytes += candidate.size;
                        it.increment(error);
                        continue;
                    }
                    candidate.modified = entry.last_write_time(error);
                    if (error) break;
                    candidate.overview = policy.prefer_overview_tiles && tile.zoom <= 8;
                    if (statement.handle)
                    {
                        if (sqlite3_bind_text(statement.handle, 1, candidate.key.data(), int(candidate.key.size()), SQLITE_TRANSIENT) != SQLITE_OK)
                        { ++result.errors; break; }
                        const int status = sqlite3_step(statement.handle);
                        if (status == SQLITE_ROW) candidate.access = sqlite3_column_int64(statement.handle, 0);
                        else if (status != SQLITE_DONE) { ++result.errors; break; }
                        if (sqlite3_reset(statement.handle) != SQLITE_OK) { ++result.errors; break; }
                    }
                    { // Includes accesses recorded since the durable flush.
                        std::lock_guard<std::mutex> lock(this->mutex);
                        const AccessMap::const_iterator pending = this->pending.find(candidate.key);
                        if (pending != this->pending.end() && (!candidate.access || pending->second > *candidate.access))
                            candidate.access = pending->second;
                    }
                    if (candidates.size() < MaximumEvictionCandidates)
                    {
                        candidates.push_back(std::move(candidate));
                        std::push_heap(candidates.begin(), candidates.end(), olderCandidate);
                    }
                    else if (olderCandidate(candidate, candidates.front()))
                    {
                        std::pop_heap(candidates.begin(), candidates.end(), olderCandidate);
                        candidates.back() = std::move(candidate);
                        std::push_heap(candidates.begin(), candidates.end(), olderCandidate);
                    }
                }
            }
            it.increment(error);
        }
        if (error) ++result.errors;
        if (result.errors) { result.target_reached = false; return this->publish(result); }
        std::sort_heap(candidates.begin(), candidates.end(), olderCandidate);
        std::vector<std::string> removed_keys;
        for (const EvictionCandidate &candidate : candidates)
        {
            if (result.remaining_bytes <= target) break;
            // Serialize only one deletion, not the full scan or sort, against
            // request admission and recency updates. Never wait for active I/O.
            std::lock_guard<std::mutex> gate_lock(this->gate->mutex);
            std::lock_guard<std::mutex> lock(this->mutex);
            if (this->stopping || this->dropped != initial_dropped) break;
            const AccessMap::const_iterator pending = this->pending.find(candidate.key);
            if (this->gate->active.contains(candidate.key) ||
                (pending != this->pending.end() && (!candidate.access || pending->second >= *candidate.access)))
            { ++result.skipped_files; continue; }
            const std::filesystem::path path = this->root / candidate.relative;
            if (!safeRegularFile(this->root, candidate.relative)) { ++result.skipped_files; continue; }
            error.clear();
            const std::uint64_t size = std::filesystem::file_size(path, error);
            if (error || size != candidate.size || std::filesystem::last_write_time(path, error) != candidate.modified || error)
            { ++result.skipped_files; continue; }
            // Also respect normalized fills from cooperating older code using
            // the existing QLockFile convention.
            if (kind == CacheStorageKind::NormalizedTerrain)
            {
                std::filesystem::path lock_path = path; lock_path += ".lock";
                if (std::filesystem::exists(lock_path, error) || error) { ++result.skipped_files; continue; }
            }
            if (!std::filesystem::remove(path, error) || error) { ++result.errors; continue; }
            ++result.deleted_files;
            result.deleted_bytes += size;
            result.remaining_bytes -= std::min(result.remaining_bytes, size);
            bool key_removed = true;
            if (kind == CacheStorageKind::Raster)
            {
                std::filesystem::path sidecar = candidate.relative; sidecar += ".http.json";
                if (safeRegularFile(this->root, sidecar))
                {
                    std::filesystem::remove(this->root / sidecar, error);
                    if (error) ++result.errors;
                }
                // Multiple encodings can share a logical raster access row.
                for (const char *extension : {".png", ".jpg", ".webp"})
                {
                    std::filesystem::path variant = path;
                    variant.replace_extension(extension);
                    error.clear();
                    if (std::filesystem::exists(variant, error) || error) key_removed = false;
                }
            }
            if (key_removed) removed_keys.push_back(candidate.key);
        }
        // Prune rows for our own evictions in one transaction. Newly published
        // files have pending hints, which are subsequently flushed as usual.
        // A crash between unlink and pruning merely leaves a stale hint.
        if (!removed_keys.empty())
        {
            Database writer;
            Statement erase;
            bool ok = writer.open(this->database_path, SQLITE_OPEN_READWRITE) &&
                writer.execute("BEGIN IMMEDIATE") &&
                sqlite3_prepare_v2(writer.handle, "DELETE FROM tile_access WHERE tile_key=?1", -1,
                                   &erase.handle, nullptr) == SQLITE_OK;
            for (const std::string &key : removed_keys)
            {
                if (!ok) break;
                ok = sqlite3_bind_text(erase.handle, 1, key.data(), int(key.size()), SQLITE_TRANSIENT) == SQLITE_OK &&
                    sqlite3_step(erase.handle) == SQLITE_DONE && sqlite3_reset(erase.handle) == SQLITE_OK;
            }
            if (!ok || !writer.execute("COMMIT")) ++result.errors;
        }
        // Reconcile concurrent publication and unrecognized payloads with actual
        // filesystem accounting instead of claiming a transactional byte limit.
        const CacheStorageStatistics after = scanCacheStorage(this->root, kind);
        result.remaining_bytes = after.payload_bytes;
        result.errors += after.scan_errors;
        result.target_reached = !result.errors && result.remaining_bytes <= target;
        if (result.target_reached) this->cleanup_in_progress = false;
        return this->publish(result);
    }

    CacheCleanupResult publish(const CacheCleanupResult &result)
    {
        std::lock_guard<std::mutex> lock(this->mutex);
        this->last_cleanup = result;
        return result;
    }

    void run()
    {
        std::unique_lock<std::mutex> lock(this->mutex);
        while (!this->stopping)
        {
            this->condition.wait_for(lock, FlushInterval, [this]() {
                return this->stopping || this->policy_changed || this->pending.size() >= FlushThreshold;
            });
            if (this->stopping) break;
            const bool check_policy = this->policy_changed ||
                std::chrono::steady_clock::now() >= this->next_cleanup;
            this->policy_changed = false;
            if (check_policy) this->next_cleanup = std::chrono::steady_clock::now() + FlushInterval;
            lock.unlock();
            const bool ok = this->flush();
            if (check_policy)
            {
                try { this->cleanup(); }
                catch (const std::exception &)
                {
                    CacheCleanupResult failure;
                    failure.errors = 1;
                    failure.target_reached = false;
                    this->publish(failure);
                }
            }
            lock.lock();
            if (!ok)
                this->condition.wait_for(lock, RetryInterval, [this]() { return this->stopping || this->policy_changed; });
        }
    }

    const std::filesystem::path root;
    const std::filesystem::path database_path;
    mutable std::mutex mutex;
    mutable std::mutex disk_mutex;
    std::condition_variable condition;
    AccessMap pending;
    std::string last_error;
    std::uint64_t dropped = 0;
    bool stopping = false;
    std::shared_ptr<PayloadGate> gate = std::make_shared<PayloadGate>();
    std::mutex cleanup_mutex;
    CacheStoragePolicy policy;
    CacheStorageKind kind = CacheStorageKind::Raster;
    CacheCleanupResult last_cleanup;
    bool policy_changed = false;
    bool cleanup_in_progress = false;
    std::chrono::steady_clock::time_point next_cleanup = std::chrono::steady_clock::now() + FlushInterval;
    std::thread worker;
};

CacheAccessIndex::CacheAccessIndex(const std::filesystem::path &root)
    : impl(std::make_unique<Impl>(root)) {}
CacheAccessIndex::~CacheAccessIndex() = default;
CacheAccessIndex::PayloadLease CacheAccessIndex::leasePayload(const std::string &key) const
{ return validKey(key) ? std::make_shared<PayloadUse>(this->impl->gate, key, this->impl->root) : PayloadLease(); }
bool CacheAccessIndex::setStoragePolicy(CacheStorageKind kind, const CacheStoragePolicy &policy)
{ return this->impl->setPolicy(kind, policy); }
CacheCleanupResult CacheAccessIndex::lastCleanup() const
{ std::lock_guard<std::mutex> lock(this->impl->mutex); return this->impl->last_cleanup; }
CacheCleanupResult CacheAccessIndex::cleanupNow() { return this->impl->cleanup(); }
bool CacheAccessIndex::replaceProtectionGroup(CacheStorageKind kind, const CacheProtectionGroup &group)
{
    if (!validGroupId(group.id) || group.ranges.empty() || group.ranges.size() > MaximumProtectionRangesPerGroup ||
        !std::all_of(group.ranges.begin(), group.ranges.end(), [kind](const CacheTileRange &range) {
            return validRange(kind, range);
        })) return false;
    return this->impl->changeProtection(group.id, group.ranges);
}
bool CacheAccessIndex::removeProtectionGroup(const std::string &id)
{ return validGroupId(id) && this->impl->changeProtection(id, {}); }
std::optional<std::vector<CacheProtectionGroup>> CacheAccessIndex::protectionGroups(CacheStorageKind kind) const
{ return this->impl->protectionGroups(kind); }
void CacheAccessIndex::recordAccess(const std::string &key) { this->impl->record(key); }
bool CacheAccessIndex::flush() { return this->impl->flush(); }
std::optional<std::int64_t> CacheAccessIndex::lastAccess(const std::string &key) const
{ return this->impl->lastAccess(key); }
std::uint64_t CacheAccessIndex::droppedAccessCount() const
{ std::lock_guard<std::mutex> lock(this->impl->mutex); return this->impl->dropped; }
std::string CacheAccessIndex::lastError() const
{ std::lock_guard<std::mutex> lock(this->impl->mutex); return this->impl->last_error; }
} // namespace Aowis::Map
