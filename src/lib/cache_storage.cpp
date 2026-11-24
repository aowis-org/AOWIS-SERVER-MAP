#include <aowis/map/cache_storage.h>

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace Aowis::Map
{
namespace
{
constexpr std::size_t FlushThreshold = 1024;
constexpr std::size_t MaximumPending = 16384;
constexpr std::chrono::seconds FlushInterval(30);
constexpr std::chrono::seconds RetryInterval(30);
constexpr const char *IndexFilename = ".aowis-cache-access-v1.sqlite3";
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
} // namespace

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
        : root(root), database_path(root / IndexFilename), worker([this]() { this->run(); }) {}
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

    void run()
    {
        std::unique_lock<std::mutex> lock(this->mutex);
        while (!this->stopping)
        {
            this->condition.wait_for(lock, FlushInterval, [this]() {
                return this->stopping || this->pending.size() >= FlushThreshold;
            });
            if (this->stopping) break;
            lock.unlock();
            const bool ok = this->flush();
            lock.lock();
            if (!ok)
                this->condition.wait_for(lock, RetryInterval, [this]() { return this->stopping; });
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
    std::thread worker;
};

CacheAccessIndex::CacheAccessIndex(const std::filesystem::path &root)
    : impl(std::make_unique<Impl>(root)) {}
CacheAccessIndex::~CacheAccessIndex() = default;
void CacheAccessIndex::recordAccess(const std::string &key) { this->impl->record(key); }
bool CacheAccessIndex::flush() { return this->impl->flush(); }
std::optional<std::int64_t> CacheAccessIndex::lastAccess(const std::string &key) const
{ return this->impl->lastAccess(key); }
std::uint64_t CacheAccessIndex::droppedAccessCount() const
{ std::lock_guard<std::mutex> lock(this->impl->mutex); return this->impl->dropped; }
std::string CacheAccessIndex::lastError() const
{ std::lock_guard<std::mutex> lock(this->impl->mutex); return this->impl->last_error; }
} // namespace Aowis::Map
