#include <aowis/map/terrain_data.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QString>
#include <QTemporaryDir>

#include <iostream>
#include <chrono>
#include <thread>

namespace
{
bool require(bool condition, const char *message)
{
    if (condition)
        return true;

    std::cerr << message << '\n';
    return false;
}

Aowis::Map::TerrainTile makeTile(const QString &dataset, int zoom, quint32 x, quint32 y)
{
    Aowis::Map::TerrainTile tile;
    tile.address.zoom = zoom;
    tile.address.x = x;
    tile.address.y = y;
    tile.dataset = dataset;
    tile.nominal_resolution_m = 30.0;
    tile.vertical_datum = Aowis::Map::TerrainVerticalDatum::Egm2008;
    tile.elevations_m.fill(100.0f, Aowis::Map::TerrainTileSampleCount);
    return tile;
}

bool writeTile(const Aowis::Map::TerrainData &terrain_data, const Aowis::Map::TerrainTile &tile)
{
    QString error_message;
    const QByteArray encoded = Aowis::Map::encodeTerrainTile(tile, &error_message);
    if (encoded.isEmpty())
        return false;

    const QString path = terrain_data.normalizedTilePath(tile.dataset, tile.address);
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(encoded) != encoded.size())
        return false;
    return file.commit();
}

bool tileExists(const Aowis::Map::TerrainData &terrain_data, const QString &dataset,
                int zoom, quint32 x, quint32 y)
{
    Aowis::Map::TerrainTileAddress address;
    address.zoom = zoom;
    address.x = x;
    address.y = y;
    return QFileInfo::exists(terrain_data.normalizedTilePath(dataset, address));
}

bool writeFile(const QString &path)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    return file.write("not a real GeoTIFF") > 0;
}
}

int main()
{
    using namespace Aowis::Map;

    QTemporaryDir temporary_directory;
    if (!require(temporary_directory.isValid(), "Failed to create terrain deletion test directory"))
        return 1;

    TerrainData::Config config;
    config.enabled = true;
    config.remote_fetch_enabled = false;
    config.cache_directory = temporary_directory.path();

    TerrainData terrain_data(config);
    QString error_message;
    if (!require(terrain_data.initialize(&error_message),
                 "Failed to initialize terrain deletion test subsystem"))
    {
        return 1;
    }

    const QString dataset = QStringLiteral("copernicus-glo30");
    const int zoom = 10;
    const quint32 last_column = (1u << zoom) - 1u;
    // A 3 x 3 block, a tile beyond it and the last column (antimeridian).
    for (quint32 x = 500; x <= 502; ++x)
    {
        for (quint32 y = 300; y <= 302; ++y)
        {
            if (!require(writeTile(terrain_data, makeTile(dataset, zoom, x, y)),
                         "Failed to write normalized test tile"))
            {
                return 1;
            }
        }
    }
    if (!require(writeTile(terrain_data, makeTile(dataset, zoom, 503, 301))
                     && writeTile(terrain_data, makeTile(dataset, zoom, last_column, 301))
                     && writeTile(terrain_data, makeTile(dataset, zoom + 1, 1001, 601)),
                 "Failed to write normalized test tiles"))
    {
        return 1;
    }

    const TerrainTile access_tile = makeTile(dataset, zoom, 501, 300);
    const TerrainTileLookupResult cached = terrain_data.terrainTile(dataset, access_tile.address, false);
    if (!require(cached.status == TerrainTileLookupStatus::Ready && terrain_data.flushCacheAccessIndex(),
                 "Cached terrain read did not complete and flush")) return 1;
    CacheAccessIndex index(std::filesystem::path(terrain_data.storagePaths().normalized.toStdU16String()));
    const QString relative = normalizedTerrainTileRelativePath(dataset, access_tile.address);
    if (!require(index.lastAccess(relative.toUtf8().toStdString()).has_value(),
                 "Normalized terrain cache read did not record access")) return 1;
    const TerrainCacheStorageStatistics before = terrain_data.cacheStorageStatistics();
    if (!require(before.normalized.payload_files == 12 && before.normalized.scan_errors == 0,
                 "Normalized storage statistics did not count existing files")) return 1;

    // The inner column and rows 300..301 only.
    TerrainCacheDeletionResult result =
        terrain_data.deleteNormalizedTiles(dataset, zoom, 501, 501, 300, 301);
    if (!require(result.status == TerrainCacheDeletionStatus::Deleted && result.deleted_count == 2,
                 "Normalized deletion did not report the two deleted tiles")
        || !require(!tileExists(terrain_data, dataset, zoom, 501, 300)
                        && !tileExists(terrain_data, dataset, zoom, 501, 301)
                        && tileExists(terrain_data, dataset, zoom, 501, 302)
                        && tileExists(terrain_data, dataset, zoom, 500, 300)
                        && tileExists(terrain_data, dataset, zoom, 502, 301),
                    "Normalized deletion removed tiles outside its range"))
    {
        return 1;
    }

    if (!require(terrain_data.cacheStorageStatistics().normalized.payload_files == 10,
                 "Deleted normalized tiles still counted in storage statistics")) return 1;

    // A range starting west of column 0 wraps to the last column.
    result = terrain_data.deleteNormalizedTiles(dataset, zoom, -1, 0, 301, 301);
    if (!require(result.status == TerrainCacheDeletionStatus::Deleted && result.deleted_count == 1
                     && !tileExists(terrain_data, dataset, zoom, last_column, 301),
                 "Normalized deletion did not wrap around the antimeridian"))
    {
        return 1;
    }

    // Rows beyond the zoom level are clamped; other zoom levels are untouched.
    result = terrain_data.deleteNormalizedTiles(dataset, zoom, 500, 503, -5, 5000);
    if (!require(result.status == TerrainCacheDeletionStatus::Deleted && result.deleted_count == 8,
                 "Normalized deletion with clamped rows did not remove the rest")
        || !require(tileExists(terrain_data, dataset, zoom + 1, 1001, 601),
                    "Normalized deletion removed a tile of another zoom level"))
    {
        return 1;
    }

    // Nothing left: the empty zoom directory is gone, deleting again is fine.
    result = terrain_data.deleteNormalizedTiles(dataset, zoom, 0, int(last_column), 0, int(last_column));
    if (!require(result.status == TerrainCacheDeletionStatus::Deleted && result.deleted_count == 0,
                 "Deleting an empty normalized range failed"))
    {
        return 1;
    }

    result = terrain_data.deleteNormalizedTiles(dataset, zoom, 10, 9, 0, 0);
    if (!require(result.status == TerrainCacheDeletionStatus::InvalidRange,
                 "An inverted deletion range was accepted")
        || !require(terrain_data.deleteNormalizedTiles(QStringLiteral("../escape"), zoom, 0, 0, 0, 0).status
                        == TerrainCacheDeletionStatus::InvalidDataset,
                    "An invalid dataset identifier was accepted"))
    {
        return 1;
    }

    // Source data: the Copernicus 1 x 1 degree cells covering a tile range.
    // Zoom 8 tile x=135, y=90 spans about 9.84..11.25 E, 46.07..47.04 N:
    // cells N46/N47 x E009/E010/E011.
    const QString providers = terrain_data.storagePaths().providers;
    const QString inside_a = QDir(providers).filePath(
        QStringLiteral("copernicus/glo30/Copernicus_DSM_COG_10_N47_00_E011_00_DEM.tif"));
    const QString inside_b = QDir(providers).filePath(
        QStringLiteral("copernicus/glo30/Copernicus_DSM_COG_10_N46_00_E009_00_DEM.tif"));
    const QString outside = QDir(providers).filePath(
        QStringLiteral("copernicus/glo30/Copernicus_DSM_COG_10_N45_00_E011_00_DEM.tif"));
    if (!require(writeFile(inside_a) && writeFile(inside_b) && writeFile(outside),
                 "Failed to write fake provider source files"))
    {
        return 1;
    }

    result = terrain_data.deleteSourceData(dataset, 8, 135, 135, 90, 90);
    if (!require(result.status == TerrainCacheDeletionStatus::Deleted && result.deleted_count == 2,
                 "Source deletion did not report the two covered cells")
        || !require(!QFileInfo::exists(inside_a) && !QFileInfo::exists(inside_b)
                        && QFileInfo::exists(outside),
                    "Source deletion removed the wrong cells"))
    {
        return 1;
    }

    if (!require(terrain_data.deleteSourceData(QStringLiteral("unknown-dataset"), 8, 0, 0, 0, 0).status
                     == TerrainCacheDeletionStatus::InvalidDataset,
                 "Source deletion accepted a dataset no provider serves"))
    {
        return 1;
    }

    TerrainData::Config disabled_config = config;
    disabled_config.enabled = false;
    TerrainData disabled_data(disabled_config);
    disabled_data.initialize(&error_message);
    if (!require(disabled_data.deleteNormalizedTiles(dataset, zoom, 0, 0, 0, 0).status
                     == TerrainCacheDeletionStatus::Disabled,
                 "Deletion ran on a disabled terrain subsystem"))
    {
        return 1;
    }

    // A standalone cache uses the same background policy for normalized
    // terrain. Native sources are never candidates, even under pressure.
    {
        QTemporaryDir eviction_directory;
        TerrainData::Config eviction_config = config;
        eviction_config.cache_directory = eviction_directory.path();
        TerrainData limited(eviction_config);
        if (!require(eviction_directory.isValid() && limited.initialize(&error_message),
                     "Cannot initialize terrain eviction fixture")) return 1;
        const TerrainTile old = makeTile(dataset, zoom, 500, 300);
        const TerrainTile recent = makeTile(dataset, zoom, 500, 301);
        const TerrainTile pinned = makeTile(dataset, zoom, 500, 302);
        const QString native = QDir(limited.storagePaths().providers).filePath(QStringLiteral("native.tif"));
        if (!require(writeTile(limited, old) && writeTile(limited, recent) && writeTile(limited, pinned) && writeFile(native),
                     "Cannot write terrain eviction fixture")) return 1;
        const QString old_path = limited.normalizedTilePath(dataset, old.address);
        const QString recent_path = limited.normalizedTilePath(dataset, recent.address);
        const std::uint64_t recent_size = std::uint64_t(QFileInfo(recent_path).size());
        const std::string protection_namespace = limited.cacheProtectionNamespace(dataset);
        const CacheProtectionGroup offline_group{
            "offline", {{protection_namespace, zoom, 500, 500, 302, 302}}};
        if (!require(!protection_namespace.empty() &&
                     limited.replaceCacheProtectionGroup(offline_group) &&
                     limited.cacheProtectionGroups().value_or(std::vector<CacheProtectionGroup>()).size() == 1,
                     "Cannot protect normalized terrain through TerrainData API")) return 1;
        if (!require(limited.terrainTile(dataset, recent.address, false).status == TerrainTileLookupStatus::Ready &&
                     limited.setCacheStoragePolicy({2 * recent_size + recent_size / 2, 85, false}),
                     "Cannot enable terrain eviction after cache hit")) return 1;
        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (QFile::exists(old_path) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!require(!QFile::exists(old_path) && QFile::exists(recent_path) && QFile::exists(native) &&
                     QFile::exists(limited.normalizedTilePath(dataset, pinned.address)),
                     "Normalized eviction did not preserve recent tile and native source")) return 1;
    }
    return 0;
}
