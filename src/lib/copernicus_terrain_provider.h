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
    explicit CopernicusTerrainProvider(int connect_timeout_seconds = 30,
                                      int inactivity_timeout_seconds = 120,
                                      int download_deadline_seconds = 1800);
    QString providerId() const override;
    QVector<TerrainDatasetInfo> datasets() const override;
    bool supportsDataset(const QString &dataset) const override;
    bool supportsAddress(const QString &dataset,
                         const TerrainTileAddress &address) const override;
    TerrainProviderFetchResult acquireTileSources(
        const QString &dataset, const TerrainTileAddress &address,
        const QString &provider_cache_directory, bool allow_remote_fetch) override;
    TerrainProviderFetchResult fetchTile(const QString &dataset,
                                         const TerrainTileAddress &address,
                                         const QString &provider_cache_directory,
                                         bool allow_remote_fetch) override;
    TerrainAnalysisGridResult fetchAnalysisGrid(
        const TerrainAnalysisGridRequest &request,
        const QString &provider_cache_directory, bool allow_remote_fetch) override;
    TerrainUpstreamActivity upstreamActivity() const override;
    void cancelUpstreamDownloads() override;
    TerrainSourceDeletionResult deleteSourceData(const QString &dataset,
                                                 const TerrainGeographicBox &box,
                                                 const QString &provider_cache_directory) override;

private:
    const int connect_timeout_ms;
    const int inactivity_timeout_ms;
    const int download_deadline_ms;
    QHash<QString, QDateTime> unavailable_source_until;
    QMutex unavailable_source_mutex;
    mutable QMutex upstream_mutex;
    QSet<QNetworkReply *> upstream_active_replies;
    quint64 upstream_cancel_generation = 0;
};

} // namespace Aowis::Map

#endif // COPERNICUS_TERRAIN_PROVIDER_H
