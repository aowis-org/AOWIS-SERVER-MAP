#ifndef AOWIS_MAP_TERRAIN_PROVIDER_H
#define AOWIS_MAP_TERRAIN_PROVIDER_H

#include <aowis/map/terrain_tile.h>

#include <QString>

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
    ConversionError
};

struct TerrainProviderFetchResult
{
    TerrainProviderFetchStatus status = TerrainProviderFetchStatus::SourceUnavailable;
    std::optional<TerrainTile> tile;
    TerrainDataOrigin origin = TerrainDataOrigin::Unknown;
    QString error_message;
};

QString terrainProviderFetchStatusId(TerrainProviderFetchStatus status);

class TerrainProvider
{
public:
    virtual ~TerrainProvider();

    virtual QString providerId() const = 0;
    virtual bool supportsDataset(const QString &dataset) const = 0;
    virtual bool supportsAddress(const QString &dataset,
                                 const TerrainTileAddress &address) const = 0;
    virtual TerrainProviderFetchResult fetchTile(const QString &dataset,
                                                 const TerrainTileAddress &address,
                                                 const QString &provider_cache_directory,
                                                 bool allow_remote_fetch) = 0;
};

} // namespace Aowis::Map

#endif // AOWIS_MAP_TERRAIN_PROVIDER_H
