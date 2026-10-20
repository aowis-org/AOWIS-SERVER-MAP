#ifndef AOWIS_MAP_TERRAIN_PROVIDER_H
#define AOWIS_MAP_TERRAIN_PROVIDER_H

#include <aowis/map/terrain_dataset.h>
#include <aowis/map/terrain_tile.h>

#include <QString>
#include <QVector>

#include <optional>

namespace Aowis::Map
{

enum class TerrainProviderFetchStatus
{
    Ready,
    UnsupportedDataset,
    UnsupportedAddress,
    SourceUnavailable,
    NetworkError,
    SourceReadError,
    ConversionError,
    Cancelled
};

struct TerrainUpstreamActivity
{
    int active = 0;
    int queued = 0;
};

struct TerrainProviderFetchResult
{
    TerrainProviderFetchStatus status = TerrainProviderFetchStatus::SourceUnavailable;
    std::optional<TerrainTile> tile;
    TerrainDataOrigin origin = TerrainDataOrigin::Unknown;
    QString error_message;
};

QString terrainProviderFetchStatusId(TerrainProviderFetchStatus status);

// A geographic box in degrees. Longitudes are unwrapped: a box crossing the
// antimeridian has west_deg < -180 or east_deg > 180 (west_deg < east_deg
// always), so a provider never has to guess which way round a box goes.
struct TerrainGeographicBox
{
    double west_deg = 0.0;
    double south_deg = 0.0;
    double east_deg = 0.0;
    double north_deg = 0.0;
};

struct TerrainSourceDeletionResult
{
    // False when at least one covered source file could not be deleted; the
    // files that could be deleted are gone regardless (deleted_count).
    bool ok = true;
    int deleted_count = 0;
    QString error_message;
};

class TerrainProvider
{
public:
    virtual ~TerrainProvider();

    virtual QString providerId() const = 0;
    // The datasets this provider serves, with their native resolution and
    // generated zoom range (GET /terrain/v1/datasets).
    virtual QVector<TerrainDatasetInfo> datasets() const = 0;
    virtual bool supportsDataset(const QString &dataset) const = 0;
    virtual bool supportsAddress(const QString &dataset,
                                 const TerrainTileAddress &address) const = 0;
    virtual TerrainProviderFetchResult fetchTile(const QString &dataset,
                                                 const TerrainTileAddress &address,
                                                 const QString &provider_cache_directory,
                                                 bool allow_remote_fetch) = 0;
    virtual TerrainUpstreamActivity upstreamActivity() const = 0;
    virtual void cancelUpstreamDownloads() = 0;
    // Deletes the provider-native source files of `dataset` (downloaded
    // rasters below provider_cache_directory) that cover any part of `box`,
    // and forgets any "source confirmed unavailable" state for them, so the
    // next normalization of that area fetches the source again (when remote
    // fetching is enabled). Files being filled right now are left alone:
    // they are fresh by definition.
    virtual TerrainSourceDeletionResult deleteSourceData(const QString &dataset,
                                                         const TerrainGeographicBox &box,
                                                         const QString &provider_cache_directory) = 0;
};

} // namespace Aowis::Map

#endif // AOWIS_MAP_TERRAIN_PROVIDER_H
