#ifndef COPERNICUS_TERRAIN_PROVIDER_H
#define COPERNICUS_TERRAIN_PROVIDER_H

#include <aowis/map/terrain_provider.h>

#include <QDateTime>
#include <QHash>
#include <QMutex>
#include <QNetworkReply>
#include <QSet>

namespace Aowis::Map
{

class CopernicusTerrainProvider final : public TerrainProvider
{
public:
    QString providerId() const override;
    bool supportsDataset(const QString &dataset) const override;
    bool supportsAddress(const QString &dataset,
                         const TerrainTileAddress &address) const override;
    TerrainProviderFetchResult fetchTile(const QString &dataset,
                                         const TerrainTileAddress &address,
                                         const QString &provider_cache_directory,
                                         bool allow_remote_fetch) override;
    TerrainUpstreamActivity upstreamActivity() const override;
    void cancelUpstreamDownloads() override;

private:
    QHash<QString, QDateTime> unavailable_source_until;
    QMutex unavailable_source_mutex;
    mutable QMutex upstream_mutex;
    QSet<QNetworkReply *> upstream_active_replies;
    quint64 upstream_cancel_generation = 0;
};

} // namespace Aowis::Map

#endif // COPERNICUS_TERRAIN_PROVIDER_H
