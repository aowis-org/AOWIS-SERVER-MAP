#ifndef MAPTILES_H
#define MAPTILES_H

#include <QObject>

#include <QByteArray>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QQueue>
#include <QSet>
#include <QString>

class MapTiles : public QObject
{
    Q_OBJECT

public:
    enum class TileFailureReason
    {
        UpstreamError,
        Timeout
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

    explicit MapTiles(QObject *parent = nullptr);
    MapTiles(int maximum_active_downloads, int maximum_queued_downloads,
             QObject *parent = nullptr);
    MapTiles(const QString &cache_base_directory, int maximum_active_downloads,
             int maximum_queued_downloads, QObject *parent = nullptr);

    TileRequestResult getTile(const QString &provider, int z, int x, int y, const QString &key);
    int deleteTiles(const QString &provider, int zoom, int tile_x_min, int tile_x_max,
                    int tile_y_min, int tile_y_max);

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
    bool saveMapTile(const QByteArray &data, const QString &tile_path);

    QString domainRandomizer(const QString &url) const;

    QString fscache_base;
    QNetworkAccessManager *network_manager;
    int maximum_active_downloads;
    int maximum_queued_downloads;

    QMutex downloads_mutex;
    QSet<QString> downloads_active;
    QSet<QString> downloads_invalidated;
    QQueue<QueuedDownload> downloads_queued;
    QSet<QString> downloads_queued_keys;

signals:
    void tileReady(QString key, QByteArray data);
    void tileFailed(const QString &key, TileFailureReason reason);
};

#endif // MAPTILES_H
