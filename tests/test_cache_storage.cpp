#include <aowis/map/cache_storage.h>

#include <chrono>
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
    std::cout << "Cache storage and access index tests passed\n";
}
