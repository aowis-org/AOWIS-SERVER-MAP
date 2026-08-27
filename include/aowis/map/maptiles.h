#ifndef MAPTILES_H
#define MAPTILES_H

#include <QObject>

#include <QByteArray>
#include <QCache>
#include <QHash>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QQueue>
#include <QSet>
#include <QString>

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

    TileRequestResult getTile(const QString &provider, int z, int x, int y, const QString &key);
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
    QString providerUrl(const QString &provider) const;
    QString providerTilePath(const QString &provider, int zoom, int x, int y) const;
    QString canonicalTileKey(const QString &provider, int zoom, int x, int y) const;

    DownloadScheduleStatus scheduleMapTileDownload(const QueuedDownload &download);
    void startMapTileDownload(const QueuedDownload &download);
    void startQueuedDownloads();
    bool canStartDownloadLocked(const QueuedDownload &download) const;
    void markDownloadStartedLocked(const QueuedDownload &download);
    void markDownloadFinishedLocked(const QueuedDownload &download);
    bool saveMapTile(const QByteArray &data, const QString &tile_path);

    QString domainRandomizer(const QString &url) const;

    QString fscache_base;
    QNetworkAccessManager *network_manager;
    QCache<QString, QByteArray> tile_memory_cache;
    int maximum_active_downloads;
    int maximum_queued_downloads;

    mutable QMutex downloads_mutex;
    QSet<QString> downloads_active;
    QHash<QString, TileHttpClient *> downloads_active_clients;
    QHash<QString, int> downloads_active_per_origin;
    QSet<QString> downloads_invalidated;
    QQueue<QueuedDownload> downloads_queued;
    QSet<QString> downloads_queued_keys;

signals:
    void tileReady(QString key, QByteArray data);
    void tileFailed(const QString &key, TileFailureReason reason);
    void upstreamActivityChanged(int active, int queued);
};

#endif // MAPTILES_H
