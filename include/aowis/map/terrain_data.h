#ifndef AOWIS_MAP_TERRAIN_DATA_H
#define AOWIS_MAP_TERRAIN_DATA_H

#include <aowis/map/terrain_tile.h>
#include <aowis/map/terrain_provider.h>
#include <aowis/map/download_coverage.h>

#include <QObject>


#include <QByteArray>
#include <QMutex>
#include <QString>
#include <QVector>

#include <memory>
#include <optional>
#include <vector>

namespace Aowis::Map
{

enum class TerrainTileLookupStatus
{
    Ready,
    Disabled,
    NotInitialized,
    InvalidDataset,
    InvalidAddress,
    TileUnavailable,
    TileReadError,
    CorruptTile,
    RemoteFetchError,
    ProviderError
};

struct TerrainTileLookupResult
{
    TerrainTileLookupStatus status = TerrainTileLookupStatus::TileUnavailable;
    QByteArray data;
    TerrainDataOrigin origin = TerrainDataOrigin::Unknown;
    QString error_message;
};

QString terrainTileLookupStatusId(TerrainTileLookupStatus status);

enum class TerrainElevationLookupStatus
{
    Ready,
    Disabled,
    NotInitialized,
    InvalidCoordinate,
    InvalidDataset,
    InvalidVerticalDatum,
    VerticalDatumConversionUnavailable,
    OutsideCoverage,
    TileUnavailable,
    TileReadError,
    CorruptTile,
    RemoteFetchError,
    ProviderError,
    NoData
};

struct TerrainElevationLookupResult
{
    TerrainElevationLookupStatus status = TerrainElevationLookupStatus::TileUnavailable;
    std::optional<TerrainElevationSample> sample;
    std::optional<TerrainTileAddress> tile_address;
    std::optional<TerrainVerticalDatum> requested_vertical_datum;
    std::optional<TerrainVerticalDatum> source_vertical_datum;
    QString error_message;
};

QString terrainElevationLookupStatusId(TerrainElevationLookupStatus status);

enum class TerrainCacheDeletionStatus
{
    Deleted,
    Disabled,
    NotInitialized,
    InvalidDataset,
    InvalidRange,
    // At least one file could not be deleted (the others are gone).
    Failed
};

struct TerrainCacheDeletionResult
{
    TerrainCacheDeletionStatus status = TerrainCacheDeletionStatus::Deleted;
    int deleted_count = 0;
    QString error_message;
};

QString terrainCacheDeletionStatusId(TerrainCacheDeletionStatus status);

class TerrainProvider;

class TerrainData : public QObject
{
public:
    struct Config
    {
        bool enabled = true;
        bool remote_fetch_enabled = true;
        QString cache_base_directory;
        QString cache_directory;
        QString permanent_data_directory;
        bool copernicus_enabled = true;
        int connect_timeout_seconds = 30;
        int inactivity_timeout_seconds = 120;
        int download_deadline_seconds = 1800;
        QString coverage_profile;
        DownloadCoverage download_coverage;
    };

    struct StoragePaths
    {
        QString root;
        QString normalized;
        QString providers;
        QString offline_packages;
    };

    explicit TerrainData(const Config &config, QObject *parent = nullptr);
    ~TerrainData() override;

    bool initialize(QString *error_message);

    bool isEnabled() const;
    bool isInitialized() const;
    bool isRemoteFetchEnabled() const;
    StoragePaths storagePaths() const;
    QString normalizedTilePath(const QString &dataset,
                               const TerrainTileAddress &address) const;

    // Bulk DEM access for Hydrology. Shared between Map's HTTP and in-process
    // clients. Never creates a separate provider instance/cache in Hydrology.
    TerrainAnalysisGridResult analysisGrid(
        const TerrainAnalysisGridRequest &request,
        bool allow_remote_fetch = true) const;
    TerrainProviderFetchResult acquireGridSources(
        const TerrainAnalysisGridRequest &request) const;

    TerrainTileLookupResult terrainTile(const QString &dataset,
                                        const TerrainTileAddress &address,
                                        bool allow_remote_fetch = true) const;
    // Network-only stage used before CPU normalization for uncached tiles.
    TerrainProviderFetchResult acquireTileSources(
        const QString &dataset, const TerrainTileAddress &address) const;

    TerrainElevationLookupResult sampleElevation(
        const QString &dataset,
        double latitude_deg,
        double longitude_deg,
        std::optional<TerrainVerticalDatum> requested_vertical_datum = std::nullopt) const;

    // Every dataset of every registered provider, in provider order. Static
    // provider knowledge: available before initialize() and while the
    // subsystem is disabled (callers decide whether to expose it).
    QVector<TerrainDatasetInfo> datasets() const;

    TerrainUpstreamActivity upstreamActivity() const;
    void cancelUpstreamDownloads();

    // Cache deletion for one XYZ tile range of one zoom level (the map-tile
    // cache's DELETE semantics: y is clamped to the zoom's rows, x wraps
    // around the antimeridian, a range at least as wide as the zoom covers
    // every column).
    //
    // deleteNormalizedTiles(): the normalized .aowterrain tiles of the range.
    // They are regenerated on the next request, from the provider-native
    // source data when it is still cached.
    TerrainCacheDeletionResult deleteNormalizedTiles(const QString &dataset, int zoom,
                                                     int tile_x_min, int tile_x_max,
                                                     int tile_y_min, int tile_y_max) const;
    // deleteSourceData(): the provider-native source data (e.g. Copernicus
    // GeoTIFF cells) covering the range's geographic area, so regenerated
    // tiles are built from freshly downloaded source data. Source files can
    // be shared with normalized tiles outside the range; those simply
    // download it again when they are next regenerated.
    TerrainCacheDeletionResult deleteSourceData(const QString &dataset, int zoom,
                                                int tile_x_min, int tile_x_max,
                                                int tile_y_min, int tile_y_max);

private:
    // Shared validation of the deletion entry points; nullopt when valid.
    std::optional<TerrainCacheDeletionResult> rejectCacheDeletion(
        const QString &dataset, int zoom, int tile_x_min, int tile_x_max,
        int tile_y_min, int tile_y_max) const;
    static QString resolveCacheDirectory(const Config &config);
    bool ensureStorageDirectory(const QString &path, const QString &description,
                                QString *error_message) const;
    bool ensureNormalizedTileDirectory(const QString &path, QString *error_message) const;
    TerrainTileLookupResult readNormalizedTile(const QString &dataset,
                                               const TerrainTileAddress &address) const;
    TerrainTileLookupResult fetchAndStoreNormalizedTile(const QString &dataset,
                                                        const TerrainTileAddress &address,
                                                        bool allow_remote_fetch) const;

    Config config;
    StoragePaths storage_paths;
    std::vector<std::unique_ptr<TerrainProvider>> providers;
    mutable QMutex normalized_directory_mutex;
    bool initialized = false;
};

} // namespace Aowis::Map

#endif // AOWIS_MAP_TERRAIN_DATA_H
