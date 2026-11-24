#include <aowis/map/cache_storage.h>
#include <sqlite3.h>
#include <limits>

#include <chrono>
#include <barrier>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

using namespace Aowis::Map;
namespace fs = std::filesystem;

namespace
{
void require(bool condition, const char *message)
{
    if (!condition) { std::cerr << message << '\n'; std::exit(EXIT_FAILURE); }
}

struct TemporaryDirectory
{
    fs::path path;
    TemporaryDirectory()
    {
        const std::string suffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path = fs::temp_directory_path() / ("aowis-cache-storage-" + suffix);
        require(fs::create_directory(path), "Cannot create test directory");
    }
    ~TemporaryDirectory()
    {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

void writeFile(const fs::path &path, std::size_t bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream << std::string(bytes, 'x');
    stream.close();
    require(bool(stream), "Cannot write fixture");
}
}

int main()
{
    TemporaryDirectory temporary;
    // Simulate pressure by reserving existing free space, never by filling
    // the test filesystem. Verify that both roots share concurrent admission.
    require(cacheMinimumFreeBytes() == DefaultCacheMinimumFreeBytes,
            "Default free-space floor missing");
    const fs::path admission_a = temporary.path / "admission-a";
    const fs::path admission_b = temporary.path / "admission-b";
    fs::create_directories(admission_a);
    fs::create_directories(admission_b);
    const std::uint64_t window = 64ULL * 1024 * 1024;
    const std::uint64_t amount = 40ULL * 1024 * 1024;
    const std::uintmax_t available = fs::space(admission_a).available;
    require(available != std::uintmax_t(-1) && available > window,
            "Insufficient fixture filesystem space for admission test");
    setCacheMinimumFreeBytes(available - window);
    {
        std::barrier start(2);
        CacheDiskWriteReservation first, second;
        std::thread one([&]() { start.arrive_and_wait(); first = reserveCacheDiskWrite(admission_a, amount); });
        std::thread two([&]() { start.arrive_and_wait(); second = reserveCacheDiskWrite(admission_b, amount); });
        one.join();
        two.join();
        require(bool(first) != bool(second), "Concurrent reservations overcommitted shared free space");
        CacheDiskWriteReservation retained = first ? first : second;
        first.reset();
        second.reset();
        require(!reserveCacheDiskWrite(admission_b, amount), "Copy did not retain reservation");
        retained.reset();
        require(bool(reserveCacheDiskWrite(admission_b, amount)), "Released capacity not reusable");
    }
    setCacheMinimumFreeBytes(std::numeric_limits<std::uint64_t>::max());
    require(!reserveCacheDiskWrite(admission_a, 1), "Free-space floor not enforced");
    setCacheMinimumFreeBytes(0);
    require(!reserveCacheDiskWrite(admission_a, std::numeric_limits<std::uint64_t>::max()),
            "Zero floor must still enforce actual availability and reject overflow");
    require(!reserveCacheDiskWrite(temporary.path / "missing", 1), "Unknown destination accepted");
    require(!reserveCacheDiskWrite({}, 1), "Empty destination accepted");
    require(bool(reserveCacheDiskWrite(admission_a, 1)), "Zero floor rejected available space");
    setCacheMinimumFreeBytes(DefaultCacheMinimumFreeBytes);

    const fs::path raster = temporary.path / "maptiles";
    const fs::path normalized = temporary.path / "terrain" / "normalized";
    const fs::path sources = temporary.path / "terrain" / "providers";
    const fs::path permanent = temporary.path / "permanent";
    const CacheStorageStatistics empty = scanCacheStorage(raster, CacheStorageKind::Raster);
    require(empty.scan_errors == 0 && empty.payload_files == 0 && empty.available_bytes.has_value(),
            "Missing cache should report zero and filesystem availability");
    writeFile(raster / "provider/6/1/2.png", 17);
    writeFile(raster / "provider/variants/fingerprint/6/1/2.jpg", 23);
    writeFile(raster / "provider/6/1/2.png.http.json", 11);
    writeFile(normalized / "v1/r1/dataset/6/1/2.aowterrain", 31);
    writeFile(sources / "copernicus/glo30/example.tif", 37);
    writeFile(sources / "copernicus/glo30/other.tif.partial", 41);
    writeFile(permanent / "maptiles/provider/6/1/3.png", 1000);
    std::error_code link_error;
    fs::create_directory_symlink(permanent, raster / "external", link_error);
    const CacheStorageStatistics images = scanCacheStorage(raster, CacheStorageKind::Raster);
    require(images.payload_files == 2 && images.payload_bytes == 40 &&
            images.auxiliary_files == 1 && images.auxiliary_bytes == 11 && images.scan_errors == 0,
            "Raster accounting must include old variants, not external files");
    const CacheStorageStatistics dem = scanCacheStorage(normalized, CacheStorageKind::NormalizedTerrain);
    require(dem.payload_files == 1 && dem.payload_bytes == 31, "Normalized terrain accounting failed");
    const CacheStorageStatistics native = scanCacheStorage(sources, CacheStorageKind::TerrainSources);
    require(native.payload_bytes == 37 && native.auxiliary_bytes == 41,
            "Native DEM and partial accounting failed");

    std::int64_t original = 0;
    {
        CacheAccessIndex index(raster);
        require(!index.lastAccess("provider/6/1/2"), "Old cache must have unknown recency");
        index.recordAccess("provider/6/1/2");
        require(index.lastAccess("provider/6/1/2").has_value(), "Pending access must be visible");
        original = *index.lastAccess("provider/6/1/2");
        index.recordAccess("../escape");
        index.recordAccess("/absolute");
        require(!index.lastAccess("../escape") && !index.lastAccess("/absolute"), "Invalid keys accepted");
        require(index.flush(), "Initial index flush failed");
        // Destructor must persist even a single pending update.
        index.recordAccess("provider/variants/fingerprint/6/1/2");
    }
    {
        CacheAccessIndex index(raster);
        require(index.lastAccess("provider/6/1/2") == original, "Access index did not survive restart");
        require(index.lastAccess("provider/variants/fingerprint/6/1/2").has_value(),
                "Variant key or destructor flush was lost");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        index.recordAccess("provider/6/1/2");
        require(index.lastAccess("provider/6/1/2").value() >= original, "Recency moved backwards");
        // Concurrent recording and explicit flushing, with two writers to the same root.
        CacheAccessIndex second(raster);
        std::vector<std::thread> writers;
        for (int thread = 0; thread < 4; ++thread)
        {
            writers.emplace_back([&index, &second, thread]() {
                for (int number = 0; number < 500; ++number)
                {
                    const std::string key = "parallel/" + std::to_string(thread) + "/" + std::to_string(number);
                    if (thread % 2 == 0) index.recordAccess(key);
                    else second.recordAccess(key);
                }
            });
        }
        for (std::thread &writer : writers) writer.join();
        require(index.flush() && second.flush(), "Concurrent batch flush failed");
        require(index.droppedAccessCount() == 0 && second.droppedAccessCount() == 0,
                "Unexpected lost access hints");
        for (int thread = 0; thread < 4; ++thread)
            require(index.lastAccess("parallel/" + std::to_string(thread) + "/499").has_value(),
                    "Concurrent writer's row was lost");
    }
    // A busy index must flush without waiting for an explicit caller or shutdown.
    {
        CacheAccessIndex writer(raster);
        CacheAccessIndex reader(raster);
        for (int number = 0; number < 1024; ++number)
            writer.recordAccess("background/" + std::to_string(number));
        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!reader.lastAccess("background/1023") && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        require(reader.lastAccess("background/1023").has_value(), "Worker did not flush a full batch");
    }
    fs::remove(raster / "provider/6/1/2.png");
    require(scanCacheStorage(raster, CacheStorageKind::Raster).payload_bytes == 23,
            "Deleted files must not be counted just because index rows remain");
    // An unavailable database must never make ordinary cache reads/writes fail.
    const fs::path blocked = temporary.path / "blocked";
    writeFile(blocked, 1);
    {
        CacheAccessIndex index(blocked);
        index.recordAccess("provider/1/0/0");
        require(!index.flush() && !index.lastError().empty(), "Index failure was not reported");
        require(index.lastAccess("provider/1/0/0").has_value(), "Failed batch was discarded");
        fs::remove(blocked);
        require(index.flush() && index.lastError().empty(), "Index did not recover after failure");
    }
    // Opt-in high/low water policy. Unknown legacy tiles first, then old
    // tracked tiles, while overview data and in-flight leases take priority.
    const fs::path eviction = temporary.path / "eviction";
    writeFile(eviction / "provider/12/1/1.png", 100);
    writeFile(eviction / "provider/variants/fingerprint/12/1/2.jpg", 100);
    writeFile(eviction / "provider/12/1/3.webp", 100);
    writeFile(eviction / "provider/4/1/4.png", 100);
    writeFile(eviction / "provider/12/1/1.png.http.json", 10);
    writeFile(eviction / "other/important.png", 1);
    writeFile(eviction / "provider/12/1/4.png.partial", 70);
    {
        CacheAccessIndex index(eviction);
        index.recordAccess("provider/variants/fingerprint/12/1/2");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        index.recordAccess("provider/12/1/3");
        require(index.flush(), "Cannot flush LRU fixture");
        require(index.cleanupNow().deleted_files == 0, "Default policy must not evict");
        require(!index.setStoragePolicy(CacheStorageKind::TerrainSources, {300, 85, true}),
                "Native source eviction must be rejected");
        require(!index.setStoragePolicy(CacheStorageKind::Raster, {300, 100, true}),
                "Invalid cleanup target accepted");
        CacheAccessIndex::PayloadLease lease = index.leasePayload("provider/variants/fingerprint/12/1/2");
        require(index.setStoragePolicy(CacheStorageKind::Raster, {300, 85, true}), "Cannot enable raster limit");
        index.cleanupNow(); // may already have been executed by the worker
        require(!fs::exists(eviction / "provider/12/1/1.png") &&
                !fs::exists(eviction / "provider/12/1/1.png.http.json"), "Unknown old tile and sidecar not removed");
        require(fs::exists(eviction / "provider/variants/fingerprint/12/1/2.jpg"), "Active lease was evicted");
        require(!fs::exists(eviction / "provider/12/1/3.webp"), "Eligible high zoom tile not evicted");
        require(fs::exists(eviction / "provider/4/1/4.png") &&
                fs::exists(eviction / "other/important.png") &&
                fs::exists(eviction / "provider/12/1/4.png.partial"), "Overview or unrelated file removed");
        require(scanCacheStorage(eviction, CacheStorageKind::Raster).payload_bytes == 201,
                "Did not reach low-water target");
        require(!index.lastAccess("provider/12/1/3"), "Evicted key not pruned");
        // A second owner must prevent automatic eviction altogether.
        {
            CacheAccessIndex other_owner(eviction);
            require(index.setStoragePolicy(CacheStorageKind::Raster, {50, 85, false}), "Cannot update limit");
            const CacheCleanupResult shared = index.cleanupNow();
            require(shared.shared_root && !shared.target_reached && shared.deleted_files == 0,
                    "Shared cache root must not be evicted");
        }
        const CacheCleanupResult blocked_cleanup = index.cleanupNow();
        require(!blocked_cleanup.target_reached && fs::exists(eviction / "provider/variants/fingerprint/12/1/2.jpg"),
                "Active tile should make low-water target temporarily unreachable");
        lease.reset();
        index.cleanupNow();
        require(!fs::exists(eviction / "provider/variants/fingerprint/12/1/2.jpg"), "Released tile was not evicted");
        require(fs::exists(eviction / "other/important.png"), "Unrecognized path was removed");
    }
    const fs::path lru = temporary.path / "lru";
    writeFile(lru / "provider/12/1/1.png", 100);
    writeFile(lru / "provider/12/1/2.png", 100);
    writeFile(lru / "provider/12/1/3.png", 100);
    {
        CacheAccessIndex index(lru);
        index.recordAccess("provider/12/1/2");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        index.recordAccess("provider/12/1/1");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        index.recordAccess("provider/12/1/3");
        require(index.flush(), "LRU order fixture flush failed");
    }
    {
        CacheAccessIndex index(lru);
        require(index.setStoragePolicy(CacheStorageKind::Raster, {250, 85, false}), "Cannot enable LRU after restart");
        index.cleanupNow();
        require(!fs::exists(lru / "provider/12/1/2.png") && fs::exists(lru / "provider/12/1/1.png") &&
                fs::exists(lru / "provider/12/1/3.png"), "Durable LRU order not respected");
    }
    const fs::path terrain_evict = temporary.path / "terrain_evict";
    writeFile(terrain_evict / "v1/r1/dataset/12/1/1.aowterrain", 100);
    writeFile(terrain_evict / "v1/r1/dataset/12/1/2.aowterrain", 100);
    writeFile(terrain_evict / "v1/r1/dataset/12/1/2.aowterrain.lock", 1);
    writeFile(terrain_evict / "provider/native.tif", 200);
    {
        CacheAccessIndex index(terrain_evict);
        require(index.setStoragePolicy(CacheStorageKind::NormalizedTerrain, {120, 85, false}), "Cannot enable terrain limit");
        index.cleanupNow();
        require(!fs::exists(terrain_evict / "v1/r1/dataset/12/1/1.aowterrain") &&
                fs::exists(terrain_evict / "v1/r1/dataset/12/1/2.aowterrain") &&
                fs::exists(terrain_evict / "provider/native.tif"), "Normalized eviction removed locked or native data");
    }
    const fs::path broken_index = temporary.path / "broken_index";
    writeFile(broken_index / "provider/12/1/1.png", 100);
    writeFile(broken_index / ".aowis-cache-access-v1.sqlite3", 100);
    {
        CacheAccessIndex index(broken_index);
        require(index.setStoragePolicy(CacheStorageKind::Raster, {10, 85, false}), "Cannot set policy for broken index");
        const CacheCleanupResult failed = index.cleanupNow();
        require(failed.errors > 0 && !failed.target_reached && fs::exists(broken_index / "provider/12/1/1.png"),
                "Corrupt index must fail closed");
    }
    // Setting a policy starts cleanup automatically. No synchronous maintenance
    // call is needed; a lease can move to a different thread and outlive its
    // original owner while a background read completes.
    const fs::path background = temporary.path / "background_eviction";
    writeFile(background / "provider/12/1/1.png", 100);
    writeFile(background / "provider/12/1/2.png", 100);
    {
        CacheAccessIndex index(background);
        CacheAccessIndex::PayloadLease lease = index.leasePayload("provider/12/1/1");
        require(index.setStoragePolicy(CacheStorageKind::Raster, {120, 85, false}), "Cannot enable background eviction");
        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (fs::exists(background / "provider/12/1/2.png") && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        require(fs::exists(background / "provider/12/1/1.png") &&
                !fs::exists(background / "provider/12/1/2.png"), "Background cleanup ignored lease or did not run");
        std::thread release([lease = std::move(lease)]() mutable { lease.reset(); });
        release.join();
        require(index.setStoragePolicy(CacheStorageKind::Raster, {50, 85, false}), "Cannot lower background limit");
        index.cleanupNow();
        require(!fs::exists(background / "provider/12/1/1.png"), "Cross-thread release left a stuck lease");
    }
    // Geographic ranges stay compact even at zoom 30 and split the date line.
    const std::optional<std::vector<CacheTileRange>> world = cacheRegionTileRanges(
        "provider", -180, -85, 180, 85, 0, 30);
    require(world && world->size() == 31 && world->back().max_x == (1 << 30) - 1,
            "World range should use one rectangle per zoom, not enumerate tiles");
    const std::optional<std::vector<CacheTileRange>> dateline = cacheRegionTileRanges(
        "provider", 170, -10, -170, 10, 2, 2);
    require(dateline && dateline->size() == 2 && (*dateline)[0] == CacheTileRange{"provider", 2, 3, 3, 1, 2} &&
            (*dateline)[1] == CacheTileRange{"provider", 2, 0, 0, 1, 2}, "Antimeridian split is wrong");
    const std::optional<std::vector<CacheTileRange>> coarse = cacheRegionTileRanges(
        "provider", 170, -10, -170, 10, 0, 0);
    require(coarse && coarse->size() == 1 && coarse->front() == CacheTileRange{"provider", 0, 0, 0, 0, 0},
            "Antimeridian must not duplicate zoom-zero tile");
    require(!cacheRegionTileRanges("provider", 0, 10, 1, -10, 0, 1) &&
            !cacheRegionTileRanges("provider", 0, 0, 1, 90, 0, 1) &&
            !cacheRegionTileRanges("provider", 0, 0, 1, 1, 0, 31) &&
            !cacheRegionTileRanges("provider", std::numeric_limits<double>::quiet_NaN(), 0, 1, 1, 0, 1),
            "Invalid geographic bounds accepted");

    const fs::path protected_raster = temporary.path / "protected_raster";
    writeFile(protected_raster / "provider/12/1/1.png", 100);
    writeFile(protected_raster / "provider/12/1/1.jpg", 100);
    writeFile(protected_raster / "provider/12/1/2.png", 100);
    writeFile(protected_raster / "provider/variants/new/12/1/1.png", 100);
    writeFile(protected_raster / "other/12/1/1.png", 100);
    {
        CacheAccessIndex index(protected_raster);
        // First migrate an existing access-only database, then save protection.
        index.recordAccess("provider/12/1/1");
        require(index.flush(), "Cannot establish pre-protection index");
        require(index.replaceProtectionGroup(CacheStorageKind::Raster, {"region A", {{"provider", 12, 1, 1, 1, 2}}}) &&
                index.replaceProtectionGroup(CacheStorageKind::Raster, {"region B", {{"provider", 12, 1, 1, 1, 1},
                    {"provider", 12, 1, 1, 3, 3}}}), "Cannot save overlapping regions");
        require(!index.replaceProtectionGroup(CacheStorageKind::Raster, {"region A", {{"provider", 12, 1, 1, 1, 1},
                    {"../invalid", 12, 1, 1, 1, 1}}}) &&
                !index.replaceProtectionGroup(CacheStorageKind::Raster, {"region A", {}}) &&
                !index.replaceProtectionGroup(CacheStorageKind::Raster, {"bad", {{"provider", 1, 0, 2, 0, 0}}}) &&
                !index.replaceProtectionGroup(CacheStorageKind::TerrainSources, {"bad", {{"provider", 1, 0, 0, 0, 0}}}),
                "Invalid group replacement must be rejected");
        const std::optional<std::vector<CacheProtectionGroup>> groups = index.protectionGroups(CacheStorageKind::Raster);
        require(groups && groups->size() == 2 && groups->front().ranges.front().max_y == 2,
                "Invalid replacement changed original group");
        // A real SQL failure after deleting the old group must roll back.
        sqlite3 *db = nullptr;
        require(sqlite3_open((protected_raster / ".aowis-cache-access-v1.sqlite3").string().c_str(), &db) == SQLITE_OK,
                "Cannot open transaction fixture");
        require(sqlite3_exec(db, "CREATE TRIGGER reject_pin BEFORE INSERT ON cache_protection "
            "WHEN NEW.group_id='region A' BEGIN SELECT RAISE(ABORT,'test'); END", nullptr, nullptr, nullptr) == SQLITE_OK,
            "Cannot create transaction failure fixture");
        require(!index.replaceProtectionGroup(CacheStorageKind::Raster, {"region A", {{"provider", 12, 1, 1, 3, 3}}}),
                "Injected transaction failure reported success");
        require(sqlite3_exec(db, "DROP TRIGGER reject_pin", nullptr, nullptr, nullptr) == SQLITE_OK, "Cannot remove trigger");
        sqlite3_close(db);
        require(index.protectionGroups(CacheStorageKind::Raster)->front().ranges.front().max_y == 2,
                "Failed transaction lost original protection");
    }
    // This file did not exist when the group was defined.
    writeFile(protected_raster / "provider/12/1/3.webp", 100);
    {
        CacheAccessIndex index(protected_raster);
        const std::optional<std::vector<CacheProtectionGroup>> groups = index.protectionGroups(CacheStorageKind::Raster);
        require(groups && groups->size() == 2, "Protection did not survive restart");
        require(index.setStoragePolicy(CacheStorageKind::Raster, {150, 85, false}), "Cannot enable protected eviction");
        const CacheCleanupResult blocked = index.cleanupNow();
        require(!blocked.target_reached && blocked.errors == 0 && blocked.protected_bytes == 400 &&
                blocked.protected_files == 4 && blocked.remaining_bytes == 400,
                "Protected data must count toward limit once per file and block unreachable target");
        require(fs::exists(protected_raster / "provider/12/1/1.png") &&
                fs::exists(protected_raster / "provider/12/1/1.jpg") &&
                fs::exists(protected_raster / "provider/12/1/2.png") &&
                fs::exists(protected_raster / "provider/12/1/3.webp") &&
                !fs::exists(protected_raster / "provider/variants/new/12/1/1.png") &&
                !fs::exists(protected_raster / "other/12/1/1.png"), "Pin identity or future tile protection failed");
        // Another cooperating instance may save a group, but eviction cannot
        // run while both hold shared ownership.
        {
            CacheAccessIndex other(protected_raster);
            require(other.removeProtectionGroup("region A"), "Cannot remove group from second index");
            const CacheCleanupResult shared = index.cleanupNow();
            require(shared.shared_root && fs::exists(protected_raster / "provider/12/1/2.png"),
                    "Shared root eviction must remain disabled");
        }
        index.cleanupNow();
        require(!fs::exists(protected_raster / "provider/12/1/2.png") &&
                fs::exists(protected_raster / "provider/12/1/1.png") &&
                fs::exists(protected_raster / "provider/12/1/3.webp"), "Removing one region lost overlapping protection");
        require(index.removeProtectionGroup("region B") && index.removeProtectionGroup("region B"),
                "Removal should be idempotent");
        index.cleanupNow();
        require(scanCacheStorage(protected_raster, CacheStorageKind::Raster).payload_bytes <= 127,
                "Unpinned tiles did not become evictable");
    }
    const fs::path protected_terrain = temporary.path / "protected_terrain";
    writeFile(protected_terrain / "v1/r2/dataset/12/1/1.aowterrain", 100);
    writeFile(protected_terrain / "v1/r1/dataset/12/1/1.aowterrain", 100);
    writeFile(protected_terrain / "v1/r2/other/12/1/1.aowterrain", 100);
    {
        CacheAccessIndex index(protected_terrain);
        require(index.replaceProtectionGroup(CacheStorageKind::NormalizedTerrain, {"DEM", {{"v1/r2/dataset", 12, 1, 1, 1, 1}}}) &&
                index.setStoragePolicy(CacheStorageKind::NormalizedTerrain, {110, 85, false}), "Cannot pin terrain");
        index.cleanupNow();
        require(fs::exists(protected_terrain / "v1/r2/dataset/12/1/1.aowterrain") &&
                !fs::exists(protected_terrain / "v1/r1/dataset/12/1/1.aowterrain") &&
                !fs::exists(protected_terrain / "v1/r2/other/12/1/1.aowterrain"), "Terrain revision/dataset isolation failed");
    }
    // A migrated database that has lost its protection table is not equivalent
    // to a legacy index without pins. Cleanup must refuse to remove payloads.
    {
        sqlite3 *db = nullptr;
        require(sqlite3_open((protected_terrain / ".aowis-cache-access-v1.sqlite3").string().c_str(), &db) == SQLITE_OK &&
                sqlite3_exec(db, "DROP TABLE cache_protection", nullptr, nullptr, nullptr) == SQLITE_OK,
                "Cannot create missing protection table fixture");
        sqlite3_close(db);
        CacheAccessIndex index(protected_terrain);
        require(!index.protectionGroups(CacheStorageKind::NormalizedTerrain) &&
                index.setStoragePolicy(CacheStorageKind::NormalizedTerrain, {10, 85, false}), "Missing pin table not detected");
        const CacheCleanupResult failed = index.cleanupNow();
        require(failed.errors && !failed.target_reached && fs::exists(protected_terrain / "v1/r2/dataset/12/1/1.aowterrain"),
                "Missing protection table must fail closed");
    }
    require(cacheStorageRootsOverlap(eviction, eviction / "permanent") &&
            !cacheStorageRootsOverlap(eviction, permanent), "Root overlap check failed");
    std::cout << "Cache storage and access index tests passed\n";
}
