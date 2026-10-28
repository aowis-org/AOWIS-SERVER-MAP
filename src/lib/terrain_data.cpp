#include <aowis/map/terrain_data.h>
#include <aowis/map/terrain_provider.h>

#include "copernicus_terrain_provider.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QMutexLocker>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace Aowis::Map
{
namespace
{
constexpr double WebMercatorMaximumLatitudeDeg = 85.0511287798066;
constexpr int NormalizedTileLockTimeoutMs = 180000;
constexpr int NormalizedTileStaleLockMs = 10 * 60 * 1000;

struct TerrainGridPosition
{
    TerrainTileAddress address;
    double column = 0.0;
    double row = 0.0;
};

bool isValidWgs84Coordinate(double latitude_deg, double longitude_deg)
{
    return std::isfinite(latitude_deg) && std::isfinite(longitude_deg) &&
           latitude_deg >= -90.0 && latitude_deg <= 90.0 &&
           longitude_deg >= -180.0 && longitude_deg <= 180.0;
}

QString verticalDatumDiagnosticLabel(TerrainVerticalDatum datum)
{
    const QString identifier = terrainVerticalDatumId(datum);
    const QString authority_code = terrainVerticalDatumAuthorityCode(datum);
    if (authority_code.isEmpty())
        return identifier;
    return QStringLiteral("%1 (%2)").arg(identifier, authority_code);
}

std::optional<TerrainGridPosition> terrainGridPosition(double latitude_deg,
                                                       double longitude_deg,
                                                       int zoom)
{
    if (zoom < 0 || zoom > TerrainTileMaximumZoom)
        return std::nullopt;
    if (latitude_deg < -WebMercatorMaximumLatitudeDeg ||
        latitude_deg > WebMercatorMaximumLatitudeDeg)
    {
        return std::nullopt;
    }

    const quint64 tile_count = quint64(1) << zoom;
    const double tile_count_double = double(tile_count);
    double tile_x = (longitude_deg + 180.0) / 360.0 * tile_count_double;

    const double latitude_rad = latitude_deg * std::numbers::pi / 180.0;
    double tile_y = (1.0 - std::asinh(std::tan(latitude_rad)) / std::numbers::pi) *
                    0.5 * tile_count_double;

    tile_x = std::clamp(tile_x, 0.0, tile_count_double);
    tile_y = std::clamp(tile_y, 0.0, tile_count_double);

    quint64 tile_x_index = 0;
    double tile_x_fraction = 0.0;
    if (tile_x >= tile_count_double)
    {
        tile_x_index = tile_count - 1;
        tile_x_fraction = 1.0;
    }
    else
    {
        tile_x_index = quint64(std::floor(tile_x));
        tile_x_fraction = tile_x - double(tile_x_index);
    }

    quint64 tile_y_index = 0;
    double tile_y_fraction = 0.0;
    if (tile_y >= tile_count_double)
    {
        tile_y_index = tile_count - 1;
        tile_y_fraction = 1.0;
    }
    else
    {
        tile_y_index = quint64(std::floor(tile_y));
        tile_y_fraction = tile_y - double(tile_y_index);
    }

    TerrainGridPosition position;
    position.address.zoom = zoom;
    position.address.x = quint32(tile_x_index);
    position.address.y = quint32(tile_y_index);
    position.column = tile_x_fraction * double(TerrainTileCellCount);
    position.row = tile_y_fraction * double(TerrainTileCellCount);
    return position;
}

std::optional<double> bilinearElevation(const TerrainTile &tile,
                                        double column,
                                        double row)
{
    if (tile.elevations_m.size() != TerrainTileSampleCount ||
        !std::isfinite(column) || !std::isfinite(row) ||
        column < 0.0 || column > double(TerrainTileCellCount) ||
        row < 0.0 || row > double(TerrainTileCellCount))
    {
        return std::nullopt;
    }

    const int column0 = std::clamp(int(std::floor(column)), 0, TerrainTileCellCount);
    const int row0 = std::clamp(int(std::floor(row)), 0, TerrainTileCellCount);
    const int column1 = std::min(column0 + 1, TerrainTileCellCount);
    const int row1 = std::min(row0 + 1, TerrainTileCellCount);
    const double column_fraction = column - double(column0);
    const double row_fraction = row - double(row0);

    const int indices[4] = {
        row0 * TerrainTileGridSize + column0,
        row0 * TerrainTileGridSize + column1,
        row1 * TerrainTileGridSize + column0,
        row1 * TerrainTileGridSize + column1
    };
    const double weights[4] = {
        (1.0 - column_fraction) * (1.0 - row_fraction),
        column_fraction * (1.0 - row_fraction),
        (1.0 - column_fraction) * row_fraction,
        column_fraction * row_fraction
    };

    double weighted_elevation_m = 0.0;
    for (int index = 0; index < 4; ++index)
    {
        if (weights[index] <= std::numeric_limits<double>::epsilon())
            continue;

        const double elevation_m = double(tile.elevations_m.at(indices[index]));
        if (!std::isfinite(elevation_m))
            return std::nullopt;

        weighted_elevation_m += weights[index] * elevation_m;
    }

    if (!std::isfinite(weighted_elevation_m))
        return std::nullopt;
    return weighted_elevation_m;
}

qint64 positiveModulo(qint64 value, qint64 divisor)
{
    const qint64 remainder = value % divisor;
    return remainder < 0 ? remainder + divisor : remainder;
}

// MapTiles' column test: the range wraps around the antimeridian, a range
// at least as wide as the zoom level covers every column.
bool tileXInsideRange(qint64 tile_x, qint64 tile_count, qint64 tile_x_min, qint64 tile_x_max)
{
    const qint64 range_width = tile_x_max - tile_x_min + 1;
    if (range_width >= tile_count)
        return true;

    const qint64 first_wrapped_x = positiveModulo(tile_x_min, tile_count);
    const qint64 offset = positiveModulo(tile_x - first_wrapped_x, tile_count);
    return offset < range_width;
}

double tileYToLatitudeDeg(double tile_y, double tile_count)
{
    const double mercator_y = std::numbers::pi * (1.0 - 2.0 * tile_y / tile_count);
    return std::atan(std::sinh(mercator_y)) * 180.0 / std::numbers::pi;
}
}

QString terrainCacheDeletionStatusId(TerrainCacheDeletionStatus status)
{
    switch (status)
    {
        case TerrainCacheDeletionStatus::Deleted:
            return QStringLiteral("deleted");
        case TerrainCacheDeletionStatus::Disabled:
            return QStringLiteral("disabled");
        case TerrainCacheDeletionStatus::NotInitialized:
            return QStringLiteral("not-initialized");
        case TerrainCacheDeletionStatus::InvalidDataset:
            return QStringLiteral("invalid-dataset");
        case TerrainCacheDeletionStatus::InvalidRange:
            return QStringLiteral("invalid-range");
        case TerrainCacheDeletionStatus::Failed:
        default:
            return QStringLiteral("failed");
    }
}

QString terrainTileLookupStatusId(TerrainTileLookupStatus status)
{
    switch (status)
    {
        case TerrainTileLookupStatus::Ready:
            return QStringLiteral("ready");
        case TerrainTileLookupStatus::Disabled:
            return QStringLiteral("disabled");
        case TerrainTileLookupStatus::NotInitialized:
            return QStringLiteral("not-initialized");
        case TerrainTileLookupStatus::InvalidDataset:
            return QStringLiteral("invalid-dataset");
        case TerrainTileLookupStatus::InvalidAddress:
            return QStringLiteral("invalid-address");
        case TerrainTileLookupStatus::TileUnavailable:
            return QStringLiteral("tile-unavailable");
        case TerrainTileLookupStatus::TileReadError:
            return QStringLiteral("tile-read-error");
        case TerrainTileLookupStatus::CorruptTile:
            return QStringLiteral("corrupt-tile");
        case TerrainTileLookupStatus::RemoteFetchError:
            return QStringLiteral("remote-fetch-error");
        case TerrainTileLookupStatus::ProviderError:
            return QStringLiteral("provider-error");
        default:
            return QStringLiteral("tile-unavailable");
    }
}

QString terrainElevationLookupStatusId(TerrainElevationLookupStatus status)
{
    switch (status)
    {
        case TerrainElevationLookupStatus::Ready:
            return QStringLiteral("ready");
        case TerrainElevationLookupStatus::Disabled:
            return QStringLiteral("disabled");
        case TerrainElevationLookupStatus::NotInitialized:
            return QStringLiteral("not-initialized");
        case TerrainElevationLookupStatus::InvalidCoordinate:
            return QStringLiteral("invalid-coordinate");
        case TerrainElevationLookupStatus::InvalidDataset:
            return QStringLiteral("invalid-dataset");
        case TerrainElevationLookupStatus::InvalidVerticalDatum:
            return QStringLiteral("invalid-vertical-datum");
        case TerrainElevationLookupStatus::VerticalDatumConversionUnavailable:
            return QStringLiteral("vertical-datum-conversion-unavailable");
        case TerrainElevationLookupStatus::OutsideCoverage:
            return QStringLiteral("outside-coverage");
        case TerrainElevationLookupStatus::TileUnavailable:
            return QStringLiteral("tile-unavailable");
        case TerrainElevationLookupStatus::TileReadError:
            return QStringLiteral("tile-read-error");
        case TerrainElevationLookupStatus::CorruptTile:
            return QStringLiteral("corrupt-tile");
        case TerrainElevationLookupStatus::RemoteFetchError:
            return QStringLiteral("remote-fetch-error");
        case TerrainElevationLookupStatus::ProviderError:
            return QStringLiteral("provider-error");
        case TerrainElevationLookupStatus::NoData:
            return QStringLiteral("no-data");
        default:
            return QStringLiteral("tile-unavailable");
    }
}

TerrainData::TerrainData(const Config &config, QObject *parent)
    : QObject(parent),
      config(config)
{
    this->storage_paths.root = resolveCacheDirectory(config);
    this->storage_paths.normalized = QDir(this->storage_paths.root).filePath(QStringLiteral("normalized"));
    this->storage_paths.providers = QDir(this->storage_paths.root).filePath(QStringLiteral("providers"));
    this->storage_paths.offline_packages = config.permanent_data_directory.isEmpty()
        ? QDir(this->storage_paths.root).filePath(QStringLiteral("offline-packages"))
        : QDir(config.permanent_data_directory).filePath(QStringLiteral("terrain"));
    if (config.copernicus_enabled)
        this->providers.push_back(std::make_unique<CopernicusTerrainProvider>(
            config.connect_timeout_seconds, config.inactivity_timeout_seconds,
            config.download_deadline_seconds, config.integration_source_url));
}

TerrainData::~TerrainData() = default;

bool TerrainData::initialize(QString *error_message)
{
    if (error_message != nullptr)
        error_message->clear();

    if (!this->config.enabled)
    {
        this->initialized = true;
        return true;
    }

    if (this->storage_paths.root.trimmed().isEmpty())
    {
        if (error_message != nullptr)
            *error_message = QStringLiteral("Terrain cache directory resolved to an empty path");
        return false;
    }

    if (!ensureStorageDirectory(this->storage_paths.root, QStringLiteral("terrain cache"), error_message))
        return false;
    if (!ensureStorageDirectory(this->storage_paths.normalized,
                                QStringLiteral("normalized terrain store"), error_message))
    {
        return false;
    }
    if (!ensureStorageDirectory(this->storage_paths.providers,
                                QStringLiteral("terrain provider cache"), error_message))
    {
        return false;
    }
    if (!ensureStorageDirectory(this->storage_paths.offline_packages,
                                QStringLiteral("offline terrain package store"), error_message))
    {
        return false;
    }

    this->initialized = true;
    return true;
}

bool TerrainData::isEnabled() const
{
    return this->config.enabled;
}

bool TerrainData::isInitialized() const
{
    return this->initialized;
}

bool TerrainData::isRemoteFetchEnabled() const
{
    return this->config.enabled && this->config.remote_fetch_enabled;
}

TerrainData::StoragePaths TerrainData::storagePaths() const
{
    return this->storage_paths;
}

QString TerrainData::normalizedTilePath(const QString &dataset,
                                        const TerrainTileAddress &address) const
{
    const QString relative_path = normalizedTerrainTileRelativePath(dataset, address);
    if (relative_path.isEmpty())
        return QString();

    return QDir(this->storage_paths.normalized).filePath(relative_path);
}

QVector<TerrainDatasetInfo> TerrainData::datasets() const
{
    QVector<TerrainDatasetInfo> result;
    for (const std::unique_ptr<TerrainProvider> &provider : this->providers)
        result.append(provider->datasets());
    return result;
}

TerrainUpstreamActivity TerrainData::upstreamActivity() const
{
    TerrainUpstreamActivity total;
    for (const std::unique_ptr<TerrainProvider> &provider : this->providers)
    {
        const TerrainUpstreamActivity activity = provider->upstreamActivity();
        total.active += activity.active;
        total.queued += activity.queued;
    }
    return total;
}

void TerrainData::cancelUpstreamDownloads()
{
    for (const std::unique_ptr<TerrainProvider> &provider : this->providers)
        provider->cancelUpstreamDownloads();
}

std::optional<TerrainCacheDeletionResult> TerrainData::rejectCacheDeletion(
    const QString &dataset, int zoom, int tile_x_min, int tile_x_max,
    int tile_y_min, int tile_y_max) const
{
    TerrainCacheDeletionResult result;
    if (!this->config.enabled)
    {
        result.status = TerrainCacheDeletionStatus::Disabled;
        result.error_message = QStringLiteral("Terrain subsystem is disabled");
        return result;
    }
    if (!this->initialized)
    {
        result.status = TerrainCacheDeletionStatus::NotInitialized;
        result.error_message = QStringLiteral("Terrain subsystem is not initialized");
        return result;
    }
    if (!isValidTerrainDatasetId(dataset))
    {
        result.status = TerrainCacheDeletionStatus::InvalidDataset;
        result.error_message = QStringLiteral("Invalid terrain dataset identifier");
        return result;
    }
    if (zoom < 0 || zoom > TerrainTileMaximumZoom ||
        tile_x_min > tile_x_max || tile_y_min > tile_y_max)
    {
        result.status = TerrainCacheDeletionStatus::InvalidRange;
        result.error_message = QStringLiteral("Invalid terrain cache deletion range");
        return result;
    }
    return std::nullopt;
}

TerrainCacheDeletionResult TerrainData::deleteNormalizedTiles(
    const QString &dataset, int zoom, int tile_x_min, int tile_x_max,
    int tile_y_min, int tile_y_max) const
{
    const std::optional<TerrainCacheDeletionResult> rejected =
        rejectCacheDeletion(dataset, zoom, tile_x_min, tile_x_max, tile_y_min, tile_y_max);
    if (rejected.has_value())
        return rejected.value();

    TerrainCacheDeletionResult result;
    const qint64 tile_count = qint64(1) << zoom;
    const qint64 bounded_y_min = std::max<qint64>(0, tile_y_min);
    const qint64 bounded_y_max = std::min<qint64>(tile_count - 1, tile_y_max);
    if (bounded_y_min > bounded_y_max)
        return result;

    // The zoom directory from the canonical tile path (the layout is defined
    // once, normalizedTerrainTileRelativePath()): <zoom>/<x>/<y>.aowterrain.
    TerrainTileAddress origin_address;
    origin_address.zoom = zoom;
    const QString origin_path = normalizedTilePath(dataset, origin_address);
    if (origin_path.isEmpty())
    {
        result.status = TerrainCacheDeletionStatus::InvalidRange;
        result.error_message = QStringLiteral("Invalid terrain cache deletion range");
        return result;
    }
    const QString zoom_path = QFileInfo(QFileInfo(origin_path).absolutePath()).absolutePath();
    QDir zoom_directory(zoom_path);
    if (!zoom_directory.exists())
        return result;

    const QString tile_suffix = QStringLiteral(".") + terrainTileFileExtension();
    const QStringList x_directory_names =
        zoom_directory.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &x_directory_name : x_directory_names)
    {
        bool tile_x_valid = false;
        const qint64 tile_x = x_directory_name.toLongLong(&tile_x_valid);
        if (!tile_x_valid || tile_x < 0 || tile_x >= tile_count ||
            !tileXInsideRange(tile_x, tile_count, tile_x_min, tile_x_max))
        {
            continue;
        }

        QDir x_directory(zoom_directory.filePath(x_directory_name));
        const QStringList tile_file_names = x_directory.entryList(
            QStringList() << (QStringLiteral("*") + tile_suffix), QDir::Files);
        for (const QString &tile_file_name : tile_file_names)
        {
            bool tile_y_valid = false;
            const qint64 tile_y =
                tile_file_name.left(tile_file_name.size() - tile_suffix.size()).toLongLong(&tile_y_valid);
            if (!tile_y_valid || tile_y < bounded_y_min || tile_y > bounded_y_max)
                continue;

            // A fill of this tile (terrainTile(), per-tile lock) only runs
            // while the file is missing or corrupt and replaces it atomically
            // (QSaveFile): removing it underneath is safe either way.
            const QString tile_path = x_directory.filePath(tile_file_name);
            if (QFile::remove(tile_path))
            {
                ++result.deleted_count;
                continue;
            }

            result.status = TerrainCacheDeletionStatus::Failed;
            if (result.error_message.isEmpty())
            {
                result.error_message =
                    QStringLiteral("Failed to delete normalized terrain tile: %1").arg(tile_path);
            }
        }

        // Only succeeds when nothing is left (lock files of running fills).
        zoom_directory.rmdir(x_directory_name);
    }

    QDir(QFileInfo(zoom_path).absolutePath()).rmdir(QString::number(zoom));
    return result;
}

TerrainCacheDeletionResult TerrainData::deleteSourceData(
    const QString &dataset, int zoom, int tile_x_min, int tile_x_max,
    int tile_y_min, int tile_y_max)
{
    const std::optional<TerrainCacheDeletionResult> rejected =
        rejectCacheDeletion(dataset, zoom, tile_x_min, tile_x_max, tile_y_min, tile_y_max);
    if (rejected.has_value())
        return rejected.value();

    TerrainCacheDeletionResult result;
    const qint64 tile_count = qint64(1) << zoom;
    const qint64 bounded_y_min = std::max<qint64>(0, tile_y_min);
    const qint64 bounded_y_max = std::min<qint64>(tile_count - 1, tile_y_max);
    if (bounded_y_min > bounded_y_max)
        return result;

    // The range's geographic box: unwrapped longitudes (the x range may
    // start west of column 0 or end east of the last one), latitudes from
    // the clamped rows' outer edges.
    const double tile_count_double = double(tile_count);
    TerrainGeographicBox box;
    if (qint64(tile_x_max) - tile_x_min + 1 >= tile_count)
    {
        box.west_deg = -180.0;
        box.east_deg = 180.0;
    }
    else
    {
        box.west_deg = double(tile_x_min) / tile_count_double * 360.0 - 180.0;
        box.east_deg = double(qint64(tile_x_max) + 1) / tile_count_double * 360.0 - 180.0;
    }
    box.north_deg = tileYToLatitudeDeg(double(bounded_y_min), tile_count_double);
    box.south_deg = tileYToLatitudeDeg(double(bounded_y_max + 1), tile_count_double);

    bool dataset_supported = false;
    for (const std::unique_ptr<TerrainProvider> &provider : this->providers)
    {
        if (!provider->supportsDataset(dataset))
            continue;

        dataset_supported = true;
        const TerrainSourceDeletionResult provider_result =
            provider->deleteSourceData(dataset, box, this->storage_paths.providers);
        result.deleted_count += provider_result.deleted_count;
        if (!provider_result.ok)
        {
            result.status = TerrainCacheDeletionStatus::Failed;
            if (result.error_message.isEmpty())
                result.error_message = provider_result.error_message;
        }
    }

    if (!dataset_supported)
    {
        result.status = TerrainCacheDeletionStatus::InvalidDataset;
        result.error_message = QStringLiteral(
            "No terrain provider supports dataset '%1'").arg(dataset);
    }
    return result;
}

TerrainAnalysisGridResult TerrainData::analysisGrid(
    const TerrainAnalysisGridRequest &request, bool allow_remote_fetch) const
{
    TerrainAnalysisGridResult result;
    if (!this->config.enabled || !this->initialized)
    {
        result.status = TerrainProviderFetchStatus::SourceUnavailable;
        result.error_message = QStringLiteral("Terrain subsystem disabled or not initialized");
        return result;
    }
    if (!isValidTerrainDatasetId(request.dataset))
    {
        result.status = TerrainProviderFetchStatus::UnsupportedDataset;
        result.error_message = QStringLiteral("Invalid terrain dataset identifier");
        return result;
    }
    for (const std::unique_ptr<TerrainProvider> &provider : this->providers)
    {
        if (provider->supportsDataset(request.dataset))
            return provider->fetchAnalysisGrid(request, this->storage_paths.providers,
                                              this->config.remote_fetch_enabled && allow_remote_fetch);
    }
    result.status = TerrainProviderFetchStatus::UnsupportedDataset;
    result.error_message = QStringLiteral("Unrecognized terrain dataset");
    return result;
}

TerrainProviderFetchResult TerrainData::acquireGridSources(
    const TerrainAnalysisGridRequest &request) const
{
    TerrainProviderFetchResult result;
    result.status = TerrainProviderFetchStatus::Ready;
    if (!this->initialized || !this->config.remote_fetch_enabled)
        return result;
    for (const std::unique_ptr<TerrainProvider> &provider : this->providers)
    {
        if (provider->supportsDataset(request.dataset))
            return provider->acquireGridSources(request, this->storage_paths.providers, true);
    }
    return result;
}

TerrainProviderFetchResult TerrainData::acquireTileSources(
    const QString &dataset, const TerrainTileAddress &address) const
{
    TerrainProviderFetchResult result;
    result.status = TerrainProviderFetchStatus::Ready;
    if (!this->initialized || !this->config.remote_fetch_enabled ||
        !this->config.download_coverage.permitsTile(
            this->config.coverage_profile, address.zoom, address.x, address.y))
        return result;
    // A normalized cache hit requires no provider-native acquisition.
    if (QFileInfo::exists(normalizedTilePath(dataset, address)))
        return result;
    for (const std::unique_ptr<TerrainProvider> &provider : this->providers)
    {
        if (provider->supportsDataset(dataset) && provider->supportsAddress(dataset, address))
            return provider->acquireTileSources(dataset, address,
                                                this->storage_paths.providers, true);
    }
    return result;
}

TerrainTileLookupResult TerrainData::terrainTile(const QString &dataset,
                                                  const TerrainTileAddress &address,
                                                  bool allow_remote_fetch) const
{
    TerrainTileLookupResult result;

    if (!this->config.enabled)
    {
        result.status = TerrainTileLookupStatus::Disabled;
        result.error_message = QStringLiteral("Terrain subsystem is disabled");
        return result;
    }
    if (!this->initialized)
    {
        result.status = TerrainTileLookupStatus::NotInitialized;
        result.error_message = QStringLiteral("Terrain subsystem is not initialized");
        return result;
    }
    if (!isValidTerrainDatasetId(dataset))
    {
        result.status = TerrainTileLookupStatus::InvalidDataset;
        result.error_message = QStringLiteral("Invalid terrain dataset identifier");
        return result;
    }
    if (!isValidTerrainTileAddress(address))
    {
        result.status = TerrainTileLookupStatus::InvalidAddress;
        result.error_message = QStringLiteral("Invalid normalized terrain tile address");
        return result;
    }

    result = readNormalizedTile(dataset, address);
    if (result.status == TerrainTileLookupStatus::Ready ||
        result.status == TerrainTileLookupStatus::TileReadError)
    {
        return result;
    }

    const bool initial_tile_was_corrupt =
        result.status == TerrainTileLookupStatus::CorruptTile;
    const TerrainTileLookupResult initial_corrupt_result = result;

    const QString normalized_path = normalizedTilePath(dataset, address);
    const QFileInfo normalized_file_info(normalized_path);
    if (!ensureNormalizedTileDirectory(normalized_file_info.absolutePath(),
                                       &result.error_message))
    {
        result.status = TerrainTileLookupStatus::TileReadError;
        return result;
    }

    QLockFile normalized_lock(normalized_path + QStringLiteral(".lock"));
    normalized_lock.setStaleLockTime(NormalizedTileStaleLockMs);
    if (!normalized_lock.tryLock(NormalizedTileLockTimeoutMs))
    {
        result.status = TerrainTileLookupStatus::ProviderError;
        result.error_message =
            QStringLiteral("Timed out waiting for normalized terrain cache fill: %1")
                .arg(normalized_path);
        return result;
    }

    // Another request/process may have filled or repaired this tile while this request
    // waited for the per-tile lock. Different XYZ tiles remain free to fill concurrently.
    result = readNormalizedTile(dataset, address);
    if (result.status == TerrainTileLookupStatus::Ready ||
        result.status == TerrainTileLookupStatus::TileReadError)
    {
        return result;
    }

    const bool locked_tile_was_corrupt =
        result.status == TerrainTileLookupStatus::CorruptTile;
    const TerrainTileLookupResult locked_corrupt_result = result;

    const TerrainTileLookupResult fetched_result =
        fetchAndStoreNormalizedTile(dataset, address, allow_remote_fetch);
    if (fetched_result.status == TerrainTileLookupStatus::Ready)
        return fetched_result;

    if (locked_tile_was_corrupt)
    {
        result = locked_corrupt_result;
        if (!fetched_result.error_message.isEmpty())
        {
            result.error_message +=
                QStringLiteral("; automatic cache repair failed: %1")
                    .arg(fetched_result.error_message);
        }
        return result;
    }

    if (initial_tile_was_corrupt)
    {
        result = initial_corrupt_result;
        if (!fetched_result.error_message.isEmpty())
        {
            result.error_message +=
                QStringLiteral("; automatic cache repair failed: %1")
                    .arg(fetched_result.error_message);
        }
        return result;
    }

    return fetched_result;
}

TerrainTileLookupResult TerrainData::readNormalizedTile(
    const QString &dataset, const TerrainTileAddress &address) const
{
    TerrainTileLookupResult result;
    const QString path = normalizedTilePath(dataset, address);
    const QFileInfo file_info(path);
    if (!file_info.exists())
    {
        result.status = TerrainTileLookupStatus::TileUnavailable;
        result.error_message = QStringLiteral("Normalized terrain tile is not cached");
        return result;
    }
    if (!file_info.isFile() || !file_info.isReadable())
    {
        result.status = TerrainTileLookupStatus::TileReadError;
        result.error_message = QStringLiteral("Normalized terrain tile is not a readable file: %1")
                                   .arg(path);
        return result;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        result.status = TerrainTileLookupStatus::TileReadError;
        result.error_message = QStringLiteral("Failed to read normalized terrain tile %1: %2")
                                   .arg(path, file.errorString());
        return result;
    }

    const QByteArray data = file.readAll();
    QString decode_error;
    const std::optional<TerrainTile> tile = decodeTerrainTile(data, &decode_error);
    if (!tile.has_value())
    {
        result.status = TerrainTileLookupStatus::CorruptTile;
        result.error_message = QStringLiteral("Failed to decode normalized terrain tile %1: %2")
                                   .arg(path, decode_error);
        return result;
    }
    if (tile->dataset != dataset ||
        tile->address.zoom != address.zoom ||
        tile->address.x != address.x ||
        tile->address.y != address.y)
    {
        result.status = TerrainTileLookupStatus::CorruptTile;
        result.error_message = QStringLiteral(
            "Normalized terrain tile metadata does not match its cache path: %1")
                                   .arg(path);
        return result;
    }

    result.status = TerrainTileLookupStatus::Ready;
    result.data = data;
    result.origin = TerrainDataOrigin::Cache;
    return result;
}

TerrainTileLookupResult TerrainData::fetchAndStoreNormalizedTile(
    const QString &dataset, const TerrainTileAddress &address,
    bool allow_remote_fetch) const
{
    TerrainTileLookupResult result;
    bool dataset_supported = false;
    bool address_supported = false;

    for (const std::unique_ptr<TerrainProvider> &provider : this->providers)
    {
        if (!provider->supportsDataset(dataset))
            continue;

        dataset_supported = true;
        if (!provider->supportsAddress(dataset, address))
            continue;

        address_supported = true;
        const TerrainProviderFetchResult provider_result =
            provider->fetchTile(dataset, address, this->storage_paths.providers,
                                allow_remote_fetch && this->config.remote_fetch_enabled &&
                                this->config.download_coverage.permitsTile(
                                    this->config.coverage_profile, address.zoom,
                                    address.x, address.y));
        if (provider_result.status == TerrainProviderFetchStatus::SourceUnavailable ||
            provider_result.status == TerrainProviderFetchStatus::UnsupportedAddress)
        {
            continue;
        }
        if (provider_result.status == TerrainProviderFetchStatus::NetworkError ||
            provider_result.status == TerrainProviderFetchStatus::Cancelled)
        {
            result.status = TerrainTileLookupStatus::RemoteFetchError;
            result.error_message = provider_result.error_message;
            return result;
        }
        if (provider_result.status != TerrainProviderFetchStatus::Ready ||
            !provider_result.tile.has_value())
        {
            result.status = TerrainTileLookupStatus::ProviderError;
            result.error_message = provider_result.error_message.isEmpty()
                ? QStringLiteral("Terrain provider %1 failed: %2")
                      .arg(provider->providerId(),
                           terrainProviderFetchStatusId(provider_result.status))
                : provider_result.error_message;
            return result;
        }

        QString encode_error;
        const QByteArray encoded = encodeTerrainTile(provider_result.tile.value(), &encode_error);
        if (encoded.isEmpty())
        {
            result.status = TerrainTileLookupStatus::ProviderError;
            result.error_message = QStringLiteral("Failed to encode normalized terrain tile from provider %1: %2")
                                       .arg(provider->providerId(), encode_error);
            return result;
        }

        const QString path = normalizedTilePath(dataset, address);
        const QFileInfo file_info(path);
        if (!ensureNormalizedTileDirectory(file_info.absolutePath(),
                                           &result.error_message))
        {
            result.status = TerrainTileLookupStatus::TileReadError;
            return result;
        }

        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly))
        {
            result.status = TerrainTileLookupStatus::TileReadError;
            result.error_message = QStringLiteral("Failed to create normalized terrain tile %1: %2")
                                       .arg(path, file.errorString());
            return result;
        }
        if (file.write(encoded) != encoded.size())
        {
            result.status = TerrainTileLookupStatus::TileReadError;
            result.error_message = QStringLiteral("Failed to write normalized terrain tile %1: %2")
                                       .arg(path, file.errorString());
            file.cancelWriting();
            return result;
        }
        if (!file.commit())
        {
            result.status = TerrainTileLookupStatus::TileReadError;
            result.error_message = QStringLiteral("Failed to commit normalized terrain tile %1: %2")
                                       .arg(path, file.errorString());
            return result;
        }

        result.status = TerrainTileLookupStatus::Ready;
        result.data = encoded;
        result.origin = provider_result.origin;
        return result;
    }

    result.status = TerrainTileLookupStatus::TileUnavailable;
    if (!dataset_supported)
    {
        result.error_message = QStringLiteral(
            "No terrain provider supports dataset '%1'").arg(dataset);
    }
    else if (!address_supported)
    {
        result.error_message = QStringLiteral(
            "Terrain provider does not generate this normalized zoom level for dataset '%1'")
                                   .arg(dataset);
    }
    else
    {
        result.error_message = QStringLiteral(
            "Terrain provider has no available source data for this normalized tile and dataset '%1'")
                                   .arg(dataset);
    }
    return result;
}

TerrainProviderFetchResult TerrainData::acquireElevationSources(
    const QString &dataset, double latitude_deg, double longitude_deg) const
{
    TerrainProviderFetchResult result;
    result.status = TerrainProviderFetchStatus::Ready;
    if (!this->initialized || !this->config.remote_fetch_enabled ||
        !isValidWgs84Coordinate(latitude_deg, longitude_deg))
        return result;

    // Match the first provider-backed address visited by sampleElevation.
    // Cached lower-resolution tiles remain available to its CPU-only fallback.
    for (int zoom = TerrainTileMaximumZoom; zoom >= 0; --zoom)
    {
        const std::optional<TerrainGridPosition> position =
            terrainGridPosition(latitude_deg, longitude_deg, zoom);
        if (!position.has_value())
            continue;
        for (const std::unique_ptr<TerrainProvider> &provider : this->providers)
        {
            if (provider->supportsDataset(dataset) &&
                provider->supportsAddress(dataset, position->address))
                return acquireTileSources(dataset, position->address);
        }
    }
    return result;
}

TerrainElevationLookupResult TerrainData::sampleElevation(
    const QString &dataset,
    double latitude_deg,
    double longitude_deg,
    std::optional<TerrainVerticalDatum> requested_vertical_datum,
    bool allow_remote_fetch) const
{
    TerrainElevationLookupResult result;
    result.requested_vertical_datum = requested_vertical_datum;

    if (!this->config.enabled)
    {
        result.status = TerrainElevationLookupStatus::Disabled;
        result.error_message = QStringLiteral("Terrain subsystem is disabled");
        return result;
    }
    if (!this->initialized)
    {
        result.status = TerrainElevationLookupStatus::NotInitialized;
        result.error_message = QStringLiteral("Terrain subsystem is not initialized");
        return result;
    }
    if (!isValidTerrainDatasetId(dataset))
    {
        result.status = TerrainElevationLookupStatus::InvalidDataset;
        result.error_message = QStringLiteral("Invalid terrain dataset identifier");
        return result;
    }
    if (requested_vertical_datum.has_value() &&
        !isRequestableTerrainVerticalDatum(requested_vertical_datum.value()))
    {
        result.status = TerrainElevationLookupStatus::InvalidVerticalDatum;
        result.error_message = QStringLiteral(
            "Requested vertical datum '%1' cannot be used as a terrain output datum")
                                   .arg(terrainVerticalDatumId(requested_vertical_datum.value()));
        return result;
    }
    if (!isValidWgs84Coordinate(latitude_deg, longitude_deg))
    {
        result.status = TerrainElevationLookupStatus::InvalidCoordinate;
        result.error_message = QStringLiteral("Terrain coordinate must be finite WGS84 latitude/longitude");
        return result;
    }
    if (latitude_deg < -WebMercatorMaximumLatitudeDeg ||
        latitude_deg > WebMercatorMaximumLatitudeDeg)
    {
        result.status = TerrainElevationLookupStatus::OutsideCoverage;
        result.error_message = QStringLiteral("Coordinate lies outside normalized Web-Mercator terrain coverage");
        return result;
    }

    bool saw_no_data = false;
    std::optional<TerrainTileAddress> no_data_address;
    for (int zoom = TerrainTileMaximumZoom; zoom >= 0; --zoom)
    {
        const std::optional<TerrainGridPosition> grid_position =
            terrainGridPosition(latitude_deg, longitude_deg, zoom);
        if (!grid_position.has_value())
            continue;

        const TerrainTileLookupResult tile_result =
            terrainTile(dataset, grid_position->address, allow_remote_fetch);
        if (tile_result.status == TerrainTileLookupStatus::TileUnavailable)
            continue;

        result.tile_address = grid_position->address;
        if (tile_result.status == TerrainTileLookupStatus::TileReadError)
        {
            result.status = TerrainElevationLookupStatus::TileReadError;
            result.error_message = tile_result.error_message;
            return result;
        }
        if (tile_result.status == TerrainTileLookupStatus::CorruptTile)
        {
            result.status = TerrainElevationLookupStatus::CorruptTile;
            result.error_message = tile_result.error_message;
            return result;
        }
        if (tile_result.status == TerrainTileLookupStatus::RemoteFetchError)
        {
            result.status = TerrainElevationLookupStatus::RemoteFetchError;
            result.error_message = tile_result.error_message;
            return result;
        }
        if (tile_result.status == TerrainTileLookupStatus::ProviderError)
        {
            result.status = TerrainElevationLookupStatus::ProviderError;
            result.error_message = tile_result.error_message;
            return result;
        }
        if (tile_result.status != TerrainTileLookupStatus::Ready)
        {
            result.status = TerrainElevationLookupStatus::TileUnavailable;
            result.error_message = tile_result.error_message;
            return result;
        }

        QString decode_error;
        const std::optional<TerrainTile> tile = decodeTerrainTile(tile_result.data, &decode_error);
        if (!tile.has_value())
        {
            result.status = TerrainElevationLookupStatus::CorruptTile;
            result.error_message = QStringLiteral(
                "Validated normalized terrain tile could not be decoded for point sampling: %1")
                                       .arg(decode_error);
            return result;
        }

        const std::optional<double> elevation_m =
            bilinearElevation(tile.value(), grid_position->column, grid_position->row);
        if (!elevation_m.has_value())
        {
            saw_no_data = true;
            if (!no_data_address.has_value())
                no_data_address = grid_position->address;
            continue;
        }

        TerrainElevationSample sample;
        sample.elevation_m = elevation_m.value();
        sample.dataset = tile->dataset;
        sample.nominal_resolution_m = tile->nominal_resolution_m;
        sample.vertical_datum = tile->vertical_datum;
        sample.source_vertical_datum = tile->vertical_datum;
        sample.origin = tile_result.origin;
        result.source_vertical_datum = tile->vertical_datum;

        if (requested_vertical_datum.has_value() &&
            requested_vertical_datum.value() != tile->vertical_datum)
        {
            result.status = TerrainElevationLookupStatus::VerticalDatumConversionUnavailable;
            result.error_message = QStringLiteral(
                "Terrain source datum is %1, but %2 was requested. "
                "No vertical datum transformation model is configured; AOWIS will not apply an "
                "implicit or constant height offset")
                                       .arg(verticalDatumDiagnosticLabel(tile->vertical_datum),
                                            verticalDatumDiagnosticLabel(
                                                requested_vertical_datum.value()));
            return result;
        }

        if (!isValidTerrainElevationSample(sample))
        {
            result.status = TerrainElevationLookupStatus::CorruptTile;
            result.error_message = QStringLiteral("Normalized terrain tile produced invalid elevation metadata: %1")
                                       .arg(normalizedTilePath(tile->dataset, grid_position->address));
            return result;
        }

        result.status = TerrainElevationLookupStatus::Ready;
        result.sample = sample;
        result.error_message.clear();
        return result;
    }

    if (saw_no_data)
    {
        result.tile_address = no_data_address;
        result.status = TerrainElevationLookupStatus::NoData;
        result.error_message = QStringLiteral("Available terrain tiles contain no elevation sample for this coordinate");
    }
    else
    {
        result.tile_address.reset();
        result.status = TerrainElevationLookupStatus::TileUnavailable;
        result.error_message = QStringLiteral("No normalized terrain tile is available for this coordinate and dataset");
    }
    return result;
}

QString TerrainData::resolveCacheDirectory(const Config &config)
{
    const QString configured_directory = config.cache_directory.trimmed();
    if (!configured_directory.isEmpty())
        return QDir::cleanPath(configured_directory);

    QString base_directory = config.cache_base_directory.trimmed();
    if (base_directory.isEmpty())
        base_directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);

    if (base_directory.trimmed().isEmpty())
        return QString();

    return QDir::cleanPath(QDir(base_directory).filePath(QStringLiteral("terrain")));
}

bool TerrainData::ensureNormalizedTileDirectory(const QString &path,
                                                QString *error_message) const
{
    QMutexLocker locker(&this->normalized_directory_mutex);

    const QFileInfo existing_info(path);
    if (existing_info.exists())
    {
        if (existing_info.isDir())
            return true;

        if (error_message != nullptr)
        {
            *error_message = QStringLiteral(
                "Normalized terrain cache path exists but is not a directory: %1")
                                 .arg(path);
        }
        return false;
    }

    QDir directory;
    if (directory.mkpath(path))
        return true;

    // A filesystem watcher, antivirus product or another process may have
    // created the directory between the existence check and mkpath(). Treat a
    // directory that exists after the failed call as success.
    const QFileInfo final_info(path);
    if (final_info.exists() && final_info.isDir())
        return true;

    if (error_message != nullptr)
    {
        *error_message = QStringLiteral(
            "Failed to create normalized terrain cache directory: %1")
                             .arg(path);
    }
    return false;
}

bool TerrainData::ensureStorageDirectory(const QString &path, const QString &description,
                                         QString *error_message) const
{
    QDir directory;
    if (!directory.mkpath(path))
    {
        if (error_message != nullptr)
        {
            *error_message = QStringLiteral("Failed to create %1 directory: %2")
                                 .arg(description, path);
        }
        return false;
    }

    const QFileInfo info(path);
    if (!info.exists() || !info.isDir())
    {
        if (error_message != nullptr)
        {
            *error_message = QStringLiteral("%1 path is not a directory: %2")
                                 .arg(description, path);
        }
        return false;
    }
    if (!info.isReadable())
    {
        if (error_message != nullptr)
        {
            *error_message = QStringLiteral("%1 directory is not readable: %2")
                                 .arg(description, path);
        }
        return false;
    }
    if (!info.isWritable())
    {
        if (error_message != nullptr)
        {
            *error_message = QStringLiteral("%1 directory is not writable: %2")
                                 .arg(description, path);
        }
        return false;
    }

    return true;
}

} // namespace Aowis::Map
