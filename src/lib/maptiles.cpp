#include <aowis/map/maptiles.h>

#include "http_client_tilefetch.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QMutexLocker>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>

namespace
{
constexpr int TileMemoryCacheMaximumCostKiB = 256 * 1024;
constexpr int MaximumActiveDownloadsPerOrigin = 6;

int tileMemoryCacheCostKiB(const QByteArray &data)
{
    const qint64 rounded_cost = (qint64(data.size()) + 1023) / 1024;
    return qMax(1, int(qMin<qint64>(rounded_cost, TileMemoryCacheMaximumCostKiB)));
}

int positiveModulo(qint64 value, int divisor)
{
    const qint64 remainder = value % divisor;
    return int(remainder < 0 ? remainder + divisor : remainder);
}

bool tileXInsideRange(int tile_x, int tile_count, int tile_x_min, int tile_x_max)
{
    const qint64 range_width = qint64(tile_x_max) - tile_x_min + 1;
    if (range_width >= tile_count)
        return true;

    const int first_wrapped_x = positiveModulo(tile_x_min, tile_count);
    const int offset = positiveModulo(qint64(tile_x) - first_wrapped_x, tile_count);
    return offset < range_width;
}

bool parseCanonicalTileKey(const QString &key, QString *provider, int *zoom, int *x, int *y)
{
    const QStringList parts = key.split('/', Qt::KeepEmptyParts);
    if (parts.size() != 4 || parts[0].isEmpty())
        return false;

    bool zoom_valid = false;
    bool x_valid = false;
    bool y_valid = false;
    const int parsed_zoom = parts[1].toInt(&zoom_valid);
    const int parsed_x = parts[2].toInt(&x_valid);
    const int parsed_y = parts[3].toInt(&y_valid);
    if (!zoom_valid || !x_valid || !y_valid)
        return false;

    *provider = parts[0];
    *zoom = parsed_zoom;
    *x = parsed_x;
    *y = parsed_y;
    return true;
}
}

MapTiles::MapTiles(QObject *parent)
    : MapTiles(32, 2048, parent)
{
}

MapTiles::MapTiles(int maximum_active_downloads, int maximum_queued_downloads, QObject *parent)
    : MapTiles(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation),
               maximum_active_downloads, maximum_queued_downloads, parent)
{
}

MapTiles::MapTiles(const QString &cache_base_directory, int maximum_active_downloads,
                   int maximum_queued_downloads, QObject *parent)
    : QObject(parent),
      fscache_base(QDir::cleanPath(cache_base_directory.trimmed().isEmpty()
                                      ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                                      : cache_base_directory)),
      network_manager(new QNetworkAccessManager(this)),
      tile_memory_cache(TileMemoryCacheMaximumCostKiB),
      maximum_active_downloads(qMax(1, maximum_active_downloads)),
      maximum_queued_downloads(qMax(0, maximum_queued_downloads))
{
}

QString MapTiles::domainRandomizer(const QString &url) const
{
    static const QStringList subdomains = { "a", "b", "c" };

    const int index = QRandomGenerator::global()->bounded(subdomains.size());
    return url.arg(subdomains.at(index));
}

QString MapTiles::providerCachePath(const QString &provider) const
{
    if (provider == "openstreetmap" || provider == "osmcyclo" ||
        provider == "opentopomap" || provider == "arcgis")
    {
        return this->fscache_base + "/maptiles/" + provider + "/";
    }

    return QString();
}

QString MapTiles::providerUrl(const QString &provider) const
{
    if (provider == "openstreetmap")
        return QStringLiteral("https://tile.openstreetmap.org/");
    if (provider == "osmcyclo")
        return domainRandomizer(QStringLiteral("https://%1.tile-cyclosm.openstreetmap.fr/cyclosm/"));
    if (provider == "opentopomap")
        return QStringLiteral("https://tile.opentopomap.org/");
    if (provider == "arcgis")
        return QStringLiteral("https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/");

    return QString();
}

QString MapTiles::providerTilePath(const QString &provider, int zoom, int x, int y) const
{
    if (provider == "arcgis")
        return QString("%1/%2/%3").arg(zoom).arg(y).arg(x);

    return QString("%1/%2/%3.png").arg(zoom).arg(x).arg(y);
}

QString MapTiles::canonicalTileKey(const QString &provider, int zoom, int x, int y) const
{
    return QString("%1/%2/%3/%4").arg(provider).arg(zoom).arg(x).arg(y);
}

MapTiles::TileRequestResult MapTiles::getTile(const QString &provider, int z, int x, int y,
                                              const QString &key)
{
    const QString cache_path = providerCachePath(provider);
    const QString url = providerUrl(provider);
    if (cache_path.isEmpty() || url.isEmpty() || z < 0 || z > 30)
    {
        qWarning() << "Invalid map tile request:" << provider << z << x << y;
        return { TileRequestStatus::InvalidRequest, {} };
    }

    const int tile_count = 1 << z;
    const int wrapped_x = positiveModulo(x, tile_count);
    if (y < 0 || y >= tile_count)
    {
        qWarning() << "Invalid map tile request:" << provider << z << x << y;
        return { TileRequestStatus::InvalidRequest, {} };
    }

    const QString file_name = QString("%1/%2/%3.png").arg(z).arg(wrapped_x).arg(y);
    const QString tile_path = QDir(cache_path).filePath(file_name);
    const QString remote_path = providerTilePath(provider, z, wrapped_x, y);
    const QString canonical_key = canonicalTileKey(provider, z, wrapped_x, y);

    const QByteArray *memory_tile = this->tile_memory_cache.object(canonical_key);
    if (memory_tile != nullptr && !memory_tile->isEmpty())
        return { TileRequestStatus::Ready, *memory_tile };

    QFile file(tile_path);
    if (file.open(QIODevice::ReadOnly))
    {
        const QByteArray data = file.readAll();
        if (!data.isEmpty())
        {
            this->tile_memory_cache.insert(
                canonical_key, new QByteArray(data), tileMemoryCacheCostKiB(data));
            return { TileRequestStatus::Ready, data };
        }
    }

    const QueuedDownload download = { url, remote_path, tile_path, canonical_key, key };
    if (scheduleMapTileDownload(download) == DownloadScheduleStatus::QueueFull)
        return { TileRequestStatus::ServerBusy, {} };

    return { TileRequestStatus::Pending, {} };
}

int MapTiles::deleteTiles(const QString &provider, int zoom, int tile_x_min, int tile_x_max,
                          int tile_y_min, int tile_y_max)
{
    const QString cache_path = providerCachePath(provider);
    if (cache_path.isEmpty() || zoom < 0 || zoom > 30 ||
        tile_x_min > tile_x_max || tile_y_min > tile_y_max)
    {
        return -1;
    }

    const int tile_count = 1 << zoom;
    const int bounded_y_min = qMax(0, tile_y_min);
    const int bounded_y_max = qMin(tile_count - 1, tile_y_max);
    if (bounded_y_min > bounded_y_max)
        return 0;

    {
        QMutexLocker locker(&this->downloads_mutex);
        QSet<QString> downloads_in_progress = this->downloads_active;
        downloads_in_progress.unite(this->downloads_queued_keys);
        for (const QString &download_key : downloads_in_progress)
        {
            QString active_provider;
            int active_zoom = 0;
            int active_x = 0;
            int active_y = 0;
            if (!parseCanonicalTileKey(download_key, &active_provider, &active_zoom, &active_x, &active_y))
                continue;

            if (active_provider == provider && active_zoom == zoom &&
                active_y >= bounded_y_min && active_y <= bounded_y_max &&
                tileXInsideRange(active_x, tile_count, tile_x_min, tile_x_max))
            {
                this->downloads_invalidated.insert(download_key);
            }
        }
    }

    const QList<QString> memory_keys = this->tile_memory_cache.keys();
    for (const QString &memory_key : memory_keys)
    {
        QString memory_provider;
        int memory_zoom = 0;
        int memory_x = 0;
        int memory_y = 0;
        if (!parseCanonicalTileKey(
                memory_key, &memory_provider, &memory_zoom, &memory_x, &memory_y))
        {
            continue;
        }

        if (memory_provider == provider && memory_zoom == zoom
            && memory_y >= bounded_y_min && memory_y <= bounded_y_max
            && tileXInsideRange(memory_x, tile_count, tile_x_min, tile_x_max))
        {
            this->tile_memory_cache.remove(memory_key);
        }
    }

    QDir provider_dir(cache_path);
    QDir zoom_dir(provider_dir.filePath(QString::number(zoom)));
    if (!zoom_dir.exists())
        return 0;

    const QStringList x_directory_names = zoom_dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    int deleted_count = 0;

    for (const QString &x_directory_name : x_directory_names)
    {
        bool tile_x_valid = false;
        const int tile_x = x_directory_name.toInt(&tile_x_valid);
        if (!tile_x_valid || tile_x < 0 || tile_x >= tile_count ||
            !tileXInsideRange(tile_x, tile_count, tile_x_min, tile_x_max))
        {
            continue;
        }

        QDir x_directory(zoom_dir.filePath(x_directory_name));
        const QStringList tile_file_names = x_directory.entryList(
            QStringList() << QStringLiteral("*.png"), QDir::Files);
        for (const QString &tile_file_name : tile_file_names)
        {
            bool tile_y_valid = false;
            const int tile_y = QFileInfo(tile_file_name).completeBaseName().toInt(&tile_y_valid);
            if (!tile_y_valid || tile_y < bounded_y_min || tile_y > bounded_y_max)
                continue;

            const QString tile_path = x_directory.filePath(tile_file_name);
            if (!QFile::remove(tile_path))
            {
                qWarning() << "Failed to delete cached tile:" << tile_path;
                return -2;
            }

            ++deleted_count;
        }

        zoom_dir.rmdir(x_directory_name);
    }

    provider_dir.rmdir(QString::number(zoom));
    return deleted_count;
}

MapTiles::UpstreamActivity MapTiles::upstreamActivity() const
{
    QMutexLocker locker(&this->downloads_mutex);
    UpstreamActivity activity;
    activity.active = this->downloads_active.size();
    activity.queued = this->downloads_queued.size();
    return activity;
}

void MapTiles::cancelUpstreamDownloads()
{
    QList<TileHttpClient *> active_clients;
    QList<QueuedDownload> queued_downloads;
    {
        QMutexLocker locker(&this->downloads_mutex);
        active_clients = this->downloads_active_clients.values();
        while (!this->downloads_queued.isEmpty())
        {
            const QueuedDownload download = this->downloads_queued.dequeue();
            queued_downloads.append(download);
            this->downloads_invalidated.remove(download.canonical_key);
        }
        this->downloads_queued_keys.clear();
    }

    for (const QueuedDownload &download : queued_downloads)
        emit tileFailed(download.response_key, TileFailureReason::Cancelled);

    for (TileHttpClient *client : active_clients)
    {
        if (client != nullptr)
            client->cancel();
    }

    const UpstreamActivity activity = upstreamActivity();
    emit upstreamActivityChanged(activity.active, activity.queued);
}

MapTiles::DownloadScheduleStatus MapTiles::scheduleMapTileDownload(const QueuedDownload &download)
{
    bool start_immediately = false;
    {
        QMutexLocker locker(&this->downloads_mutex);
        if (this->downloads_active.contains(download.canonical_key) ||
            this->downloads_queued_keys.contains(download.canonical_key))
        {
            return DownloadScheduleStatus::Scheduled;
        }

        if (canStartDownloadLocked(download))
        {
            markDownloadStartedLocked(download);
            start_immediately = true;
        }
        else
        {
            if (this->downloads_queued.size() >= this->maximum_queued_downloads)
                return DownloadScheduleStatus::QueueFull;

            this->downloads_queued.enqueue(download);
            this->downloads_queued_keys.insert(download.canonical_key);
        }
    }

    if (start_immediately)
        startMapTileDownload(download);

    const UpstreamActivity activity = upstreamActivity();
    emit upstreamActivityChanged(activity.active, activity.queued);
    return DownloadScheduleStatus::Scheduled;
}

bool MapTiles::canStartDownloadLocked(const QueuedDownload &download) const
{
    if (this->downloads_active.size() >= this->maximum_active_downloads)
        return false;

    return this->downloads_active_per_origin.value(download.url, 0) <
           MaximumActiveDownloadsPerOrigin;
}

void MapTiles::markDownloadStartedLocked(const QueuedDownload &download)
{
    this->downloads_active.insert(download.canonical_key);
    this->downloads_active_per_origin.insert(
        download.url, this->downloads_active_per_origin.value(download.url, 0) + 1);
}

void MapTiles::markDownloadFinishedLocked(const QueuedDownload &download)
{
    this->downloads_active.remove(download.canonical_key);

    const int remaining = this->downloads_active_per_origin.value(download.url, 0) - 1;
    if (remaining > 0)
        this->downloads_active_per_origin.insert(download.url, remaining);
    else
        this->downloads_active_per_origin.remove(download.url);
}

void MapTiles::startMapTileDownload(const QueuedDownload &download)
{
    TileHttpClient *rest = new TileHttpClient(this->network_manager, download.url, this);
    {
        QMutexLocker locker(&this->downloads_mutex);
        this->downloads_active_clients.insert(download.canonical_key, rest);
    }
    connect(rest, &TileHttpClient::requestFinished, this,
            [this, rest, download](const QByteArray &data)
    {
        bool invalidated = false;
        {
            QMutexLocker locker(&this->downloads_mutex);
            markDownloadFinishedLocked(download);
            this->downloads_active_clients.remove(download.canonical_key);
            invalidated = this->downloads_invalidated.remove(download.canonical_key);
        }

        if (!invalidated)
        {
            saveMapTile(data, download.tile_path);
            if (!data.isEmpty())
            {
                this->tile_memory_cache.insert(
                    download.canonical_key, new QByteArray(data), tileMemoryCacheCostKiB(data));
            }
        }

        emit tileReady(download.response_key, data);
        rest->deleteLater();
        startQueuedDownloads();
        const UpstreamActivity activity = upstreamActivity();
        emit upstreamActivityChanged(activity.active, activity.queued);
    });
    connect(rest, &TileHttpClient::requestError, this,
            [this, rest, download](TileHttpClient::RequestFailureReason reason, const QString &error)
    {
        if (reason == TileHttpClient::RequestFailureReason::Timeout)
            qWarning() << "Tile request timed out:" << download.response_key << error;
        else if (reason != TileHttpClient::RequestFailureReason::Cancelled)
            qWarning() << "Tile request failed:" << download.response_key << error;

        {
            QMutexLocker locker(&this->downloads_mutex);
            markDownloadFinishedLocked(download);
            this->downloads_active_clients.remove(download.canonical_key);
            this->downloads_invalidated.remove(download.canonical_key);
        }

        TileFailureReason failure_reason = TileFailureReason::UpstreamError;
        if (reason == TileHttpClient::RequestFailureReason::Timeout)
            failure_reason = TileFailureReason::Timeout;
        else if (reason == TileHttpClient::RequestFailureReason::Cancelled)
            failure_reason = TileFailureReason::Cancelled;
        emit tileFailed(download.response_key, failure_reason);
        rest->deleteLater();
        startQueuedDownloads();
        const UpstreamActivity activity = upstreamActivity();
        emit upstreamActivityChanged(activity.active, activity.queued);
    });
    rest->get(download.path);
}

void MapTiles::startQueuedDownloads()
{
    QList<QueuedDownload> downloads_to_start;
    {
        QMutexLocker locker(&this->downloads_mutex);
        while (this->downloads_active.size() < this->maximum_active_downloads &&
               !this->downloads_queued.isEmpty())
        {
            qsizetype eligible_index = -1;
            for (qsizetype index = 0; index < this->downloads_queued.size(); ++index)
            {
                if (canStartDownloadLocked(this->downloads_queued.at(index)))
                {
                    eligible_index = index;
                    break;
                }
            }

            if (eligible_index < 0)
                break;

            const QueuedDownload download = this->downloads_queued.takeAt(eligible_index);
            this->downloads_queued_keys.remove(download.canonical_key);
            markDownloadStartedLocked(download);
            downloads_to_start.append(download);
        }
    }

    for (const QueuedDownload &download : downloads_to_start)
        startMapTileDownload(download);
}

bool MapTiles::saveMapTile(const QByteArray &data, const QString &tile_path)
{
    const QFileInfo info(tile_path);
    QDir dir = info.dir();
    if (!dir.exists() && !dir.mkpath("."))
    {
        qWarning() << "Failed to create tile directory:" << dir.path();
        return false;
    }

    QSaveFile file(tile_path);
    if (!file.open(QIODevice::WriteOnly))
    {
        qWarning() << "Failed to open tile cache file:" << tile_path << file.errorString();
        return false;
    }

    const qint64 bytes_written = file.write(data);
    if (bytes_written != data.size())
    {
        qWarning() << "Failed to write complete tile cache file:" << tile_path
                   << bytes_written << "of" << data.size() << "bytes" << file.errorString();
        file.cancelWriting();
        return false;
    }

    if (!file.commit())
    {
        qWarning() << "Failed to commit tile cache file:" << tile_path << file.errorString();
        return false;
    }

    return true;
}
