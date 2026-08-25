#ifndef AOWIS_MAP_TERRAIN_DATA_H
#define AOWIS_MAP_TERRAIN_DATA_H

#include <aowis/map/terrain_tile.h>

#include <QObject>


#include <QByteArray>
#include <QString>

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

    TerrainTileLookupResult terrainTile(const QString &dataset,
                                        const TerrainTileAddress &address) const;

    TerrainElevationLookupResult sampleElevation(
        const QString &dataset,
        double latitude_deg,
        double longitude_deg,
        std::optional<TerrainVerticalDatum> requested_vertical_datum = std::nullopt) const;

private:
    static QString resolveCacheDirectory(const Config &config);
    bool ensureStorageDirectory(const QString &path, const QString &description,
                                QString *error_message) const;
    TerrainTileLookupResult readNormalizedTile(const QString &dataset,
                                               const TerrainTileAddress &address) const;
    TerrainTileLookupResult fetchAndStoreNormalizedTile(const QString &dataset,
                                                        const TerrainTileAddress &address) const;

    Config config;
    StoragePaths storage_paths;
    std::vector<std::unique_ptr<TerrainProvider>> providers;
    bool initialized = false;
};

} // namespace Aowis::Map

#endif // AOWIS_MAP_TERRAIN_DATA_H
