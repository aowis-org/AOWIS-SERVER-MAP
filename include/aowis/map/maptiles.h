#ifndef MAPTILES_H
#define MAPTILES_H

#include <QObject>
#include <aowis/map/map_provider.h>
#include <aowis/map/download_coverage.h>

#include <QByteArray>
#include <QCache>
#include <QHash>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QQueue>
#include <QSet>
#include <QThreadPool>
#include <QString>
#include <QStringList>

class TileHttpClient;

class MapTiles : public QObject
{
    Q_OBJECT

public:
    enum class TileFailureReason
    {
        UpstreamError,
        Timeout,
        Cancelled
    };
    Q_ENUM(TileFailureReason)

    enum class TileRequestStatus
    {
        Ready,
        Pending,
        InvalidRequest,
        ServerBusy
    };

    struct TileRequestResult
    {
        TileRequestStatus status = TileRequestStatus::InvalidRequest;
        QByteArray data;
    };

    struct UpstreamActivity
    {
        int active = 0;
        int queued = 0;
    };

    explicit MapTiles(QObject *parent = nullptr);
    MapTiles(int maximum_active_downloads, int maximum_queued_downloads,
             QObject *parent = nullptr);
    MapTiles(const QString &cache_base_directory, int maximum_active_downloads,
             int maximum_queued_downloads, QObject *parent = nullptr);

    ~MapTiles() override;

    void setRemoteFetchEnabled(bool enabled);
    void setPermanentDataDirectory(const QString &directory);
    void configureProviders(const QVector<Aowis::Map::MapProvider> &providers,
                            const Aowis::Map::DownloadCoverage &coverage,
                            const QString &default_coverage);

    TileRequestResult getTile(const QString &provider, int z, int x, int y, const QString &key);
    // HTTP path: offload established cold disk hits without occupying the event loop.
    TileRequestResult getTileForHttp(const QString &provider, int z, int x, int y, const QString &key);
    int deleteTiles(const QString &provider, int zoom, int tile_x_min, int tile_x_max,
                    int tile_y_min, int tile_y_max);
    UpstreamActivity upstreamActivity() const;
    void cancelUpstreamDownloads();

private:
    struct QueuedDownload
    {
        QString url;
        QString path;
        QString tile_path;
        QString canonical_key;
        QString response_key;
    };

    enum class DownloadScheduleStatus
    {
        Scheduled,
        QueueFull
    };

    QString providerCachePath(const QString &provider) const;
    QString providerUrl(const QString &provider, int z, int x, int y) const;
    QString canonicalTileKey(const QString &provider, int zoom, int x, int y) const;

    DownloadScheduleStatus scheduleMapTileDownload(const QueuedDownload &download);
    void startMapTileDownload(const QueuedDownload &download);
    void startQueuedDownloads();
    bool canStartDownloadLocked(const QueuedDownload &download) const;
    void markDownloadStartedLocked(const QueuedDownload &download);
    void markDownloadFinishedLocked(const QueuedDownload &download);
    static bool saveMapTile(const QByteArray &data, const QString &tile_path);

    QString domainRandomizer(const QString &url) const;

    QString fscache_base;
    QString permanent_data_directory;
    bool remote_fetch_enabled = true;
    QHash<QString, Aowis::Map::MapProvider> configured_providers;
    // First-use URL fingerprint selection. Separates cache contents when an
    // administrator changes the upstream URL without changing the UUID.
    mutable QHash<QString, QString> provider_cache_paths;
    Aowis::Map::DownloadCoverage download_coverage;
    QString default_coverage;
    QNetworkAccessManager *network_manager;
    QThreadPool cache_write_pool;
    QThreadPool cache_read_pool;
    QHash<QString, QStringList> cache_reads_pending;
    quint64 cache_generation = 0;
    int pending_cache_writes = 0;
    qint64 pending_cache_write_bytes = 0;
    QSet<QString> cache_writes_pending_keys;
    QCache<QString, QByteArray> tile_memory_cache;
    int maximum_active_downloads;
    int maximum_queued_downloads;

    mutable QMutex downloads_mutex;
    QSet<QString> downloads_active;
    QHash<QString, TileHttpClient *> downloads_active_clients;
    QHash<QString, int> downloads_active_per_origin;
    QSet<QString> downloads_invalidated;
    QSet<QString> permanent_cached_keys;
    QQueue<QueuedDownload> downloads_queued;
    QSet<QString> downloads_queued_keys;

signals:
    void tileReady(QString key, QByteArray data);
    void tileFailed(const QString &key, TileFailureReason reason);
    void upstreamActivityChanged(int active, int queued);
};

#endif // MAPTILES_H
