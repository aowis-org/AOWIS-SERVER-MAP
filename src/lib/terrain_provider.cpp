#include <aowis/map/terrain_provider.h>

namespace Aowis::Map
{

TerrainProvider::~TerrainProvider() = default;

QString terrainProviderFetchStatusId(TerrainProviderFetchStatus status)
{
    switch (status)
    {
        case TerrainProviderFetchStatus::Ready:
            return QStringLiteral("ready");
        case TerrainProviderFetchStatus::UnsupportedDataset:
            return QStringLiteral("unsupported-dataset");
        case TerrainProviderFetchStatus::UnsupportedAddress:
            return QStringLiteral("unsupported-address");
        case TerrainProviderFetchStatus::SourceUnavailable:
            return QStringLiteral("source-unavailable");
        case TerrainProviderFetchStatus::NetworkError:
            return QStringLiteral("network-error");
        case TerrainProviderFetchStatus::SourceReadError:
            return QStringLiteral("source-read-error");
        case TerrainProviderFetchStatus::ConversionError:
            return QStringLiteral("conversion-error");
        default:
            return QStringLiteral("source-unavailable");
    }
}

} // namespace Aowis::Map
