#include <aowis/map/maptiles.h>

#include "http_client_tilefetch.h"

#include <QDebug>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QtConcurrentRun>
#include <QList>
#include <QMutexLocker>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>
#include <QUrl>

namespace
{
constexpr int TileMemoryCacheMaximumCostKiB = 256 * 1024;
constexpr int MaximumActiveDownloadsPerOrigin = 6;
constexpr int MaximumPendingCacheWrites = 64;
constexpr int MaximumPendingCacheReads = 64;
constexpr qint64 MaximumPendingCacheReadBytes = 64LL * 1024 * 1024;
constexpr qint64 MaximumPendingCacheWriteBytes = 64LL * 1024 * 1024;

QString downloadOrigin(const QString &url)
{
    const QUrl parsed(url);
    return QStringLiteral("%1://%2:%3")
        .arg(parsed.scheme(), parsed.host(), QString::number(parsed.port(-1)));
}

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
    this->cache_write_pool.setMaxThreadCount(2);
    this->cache_read_pool.setMaxThreadCount(2);
    configureProviders(Aowis::Map::defaultMapProviders(), {}, {});
}

MapTiles::~MapTiles()
{
    this->cache_read_pool.waitForDone();
    this->cache_write_pool.waitForDone();
}

void MapTiles::setPermanentDataDirectory(const QString &directory)
{
    ++this->cache_generation;
    this->permanent_data_directory = directory;
    this->permanent_cached_keys.clear();
}

void MapTiles::setRemoteFetchEnabled(bool enabled)
{
    this->remote_fetch_enabled = enabled;
}

void MapTiles::configureProviders(const QVector<Aowis::Map::MapProvider> &providers,
                                  const Aowis::Map::DownloadCoverage &coverage,
                                  const QString &default_coverage)
{
    ++this->cache_generation;
    this->configured_providers.clear();
    this->permanent_cached_keys.clear();
    this->provider_cache_paths.clear();
    this->tile_memory_cache.clear();
    for (const Aowis::Map::MapProvider &provider : providers)
        if (provider.enabled) this->configured_providers.insert(provider.id, provider);
    this->download_coverage = coverage;
    this->default_coverage = default_coverage;
}

QString MapTiles::domainRandomizer(const QString &url) const
{
    static const QStringList subdomains = {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")};
    return QString(url).replace(QStringLiteral("{s}"),
          subdomains.at(QRandomGenerator::global()->bounded(subdomains.size())));
}

QString MapTiles::providerCachePath(const QString &provider) const
{
    const QHash<QString, Aowis::Map::MapProvider>::const_iterator source =
        this->configured_providers.constFind(provider);
    if (source == this->configured_providers.cend()) return {};
    const QHash<QString, QString>::const_iterator existing =
        this->provider_cache_paths.constFind(provider);
    if (existing != this->provider_cache_paths.cend()) return existing.value();

    const QString base_path = QDir(this->fscache_base).filePath(
        QStringLiteral("maptiles/%1").arg(provider));
    const QByteArray fingerprint = QCryptographicHash::hash(
        source->url_template.toUtf8(), QCryptographicHash::Sha256).toHex();
    QFile marker(QDir(base_path).filePath(QStringLiteral(".upstream-url.sha256")));
    QByteArray recorded;
    if (marker.open(QIODevice::ReadOnly)) recorded = marker.readAll().trimmed();
    QString selected_path = base_path;
    if (!recorded.isEmpty() && recorded != fingerprint)
    {
        // Old tiles must not be silently treated as tiles of the new URL.
        selected_path = QDir(base_path).filePath(
            QStringLiteral("variants/%1").arg(QString::fromLatin1(fingerprint.left(16))));
    }
    else if (recorded.isEmpty() && QDir().mkpath(base_path))
    {
        // Existing pre-fingerprint caches are kept as the initial variant.
        QSaveFile new_marker(marker.fileName());
        if (new_marker.open(QIODevice::WriteOnly))
        {
            if (new_marker.write(fingerprint) != fingerprint.size())
                new_marker.cancelWriting();
            else
                new_marker.commit();
        }
    }
    this->provider_cache_paths.insert(provider, selected_path);
    return selected_path;
}

QString MapTiles::providerUrl(const QString &provider, int zoom, int x, int y) const
{
    const auto it = this->configured_providers.constFind(provider);
    if (it == this->configured_providers.cend()) return {};
    QString url = domainRandomizer(it->url_template);
    url.replace(QStringLiteral("{z}"), QString::number(zoom));
    url.replace(QStringLiteral("{x}"), QString::number(x));
    url.replace(QStringLiteral("{y}"), QString::number(y));
    return url;
}

QString MapTiles::canonicalTileKey(const QString &provider, int zoom, int x, int y) const
{
    return QString("%1/%2/%3/%4").arg(provider).arg(zoom).arg(x).arg(y);
}

MapTiles::TileRequestResult MapTiles::getTile(const QString &provider, int z, int x, int y,
                                              const QString &key)
{
    // Reject invalid providers/coordinates before consulting either cache.
    // A warm RAM hit should not require providerCachePath(), which may perform
    // filesystem reads and writes when the provider variant is first resolved.
    if (!this->configured_providers.contains(provider) || z < 0 || z > 30)
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

    const QString canonical_key = canonicalTileKey(provider, z, wrapped_x, y);
    // Unknown permanent files still take precedence over the ordinary RAM
    // cache. Only bypass that lookup when no permanent directory is set or
    // this key has already been verified as a permanent tile.
    if (this->permanent_data_directory.isEmpty() ||
        this->permanent_cached_keys.contains(canonical_key))
    {
        const QByteArray *cached = this->tile_memory_cache.object(canonical_key);
        if (cached != nullptr && !cached->isEmpty())
            return { TileRequestStatus::Ready, *cached };
    }

    const QString cache_path = providerCachePath(provider);
    const QString file_name = QString("%1/%2/%3.png").arg(z).arg(wrapped_x).arg(y);
    const QString tile_path = QDir(cache_path).filePath(file_name);
    const Aowis::Map::MapProvider selected_provider = this->configured_providers.value(provider);

    if (!this->permanent_data_directory.isEmpty())
    {
        if (this->permanent_cached_keys.contains(canonical_key))
        {
            const QByteArray *cached = this->tile_memory_cache.object(canonical_key);
            if (cached != nullptr) return { TileRequestStatus::Ready, *cached };
        }
        QFile permanent_tile(QDir(this->permanent_data_directory).filePath(
            QStringLiteral("maptiles/%1/%2").arg(provider, file_name)));
        if (permanent_tile.open(QIODevice::ReadOnly))
        {
            const QByteArray permanent_bytes = permanent_tile.readAll();
            if (!permanent_bytes.isEmpty())
            {
                this->tile_memory_cache.insert(canonical_key,
                    new QByteArray(permanent_bytes), tileMemoryCacheCostKiB(permanent_bytes));
                this->permanent_cached_keys.insert(canonical_key);
                return {TileRequestStatus::Ready, permanent_bytes};
            }
        }
    }

    // Local tiles never expire. Upstream fetching is only considered when no
    // usable tile exists in the memory cache, on disk, or in permanent storage.
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

    const QString policy = selected_provider.coverage.isEmpty() ? this->default_coverage
                                                                   : selected_provider.coverage;
    const bool may_fetch = this->remote_fetch_enabled && selected_provider.remote_fetch_enabled &&
        z >= selected_provider.minimum_zoom && z <= selected_provider.maximum_zoom &&
        this->download_coverage.permitsTile(policy, z, wrapped_x, y);
    if (!may_fetch)
        return { TileRequestStatus::InvalidRequest, {} };

    const QueuedDownload download = {
        providerUrl(provider, z, wrapped_x, y), {}, tile_path, canonical_key, key
    };
    if (scheduleMapTileDownload(download) == DownloadScheduleStatus::QueueFull)
        return { TileRequestStatus::ServerBusy, {} };

    return { TileRequestStatus::Pending, {} };
}

MapTiles::TileRequestResult MapTiles::getTileForHttp(
    const QString &provider, int z, int x, int y, const QString &key)
{
    if (!this->configured_providers.contains(provider) || z < 0 || z > 30)
        return { TileRequestStatus::InvalidRequest, {} };
    const int tile_count = 1 << z;
    if (y < 0 || y >= tile_count)
        return { TileRequestStatus::InvalidRequest, {} };
    const int wrapped_x = positiveModulo(x, tile_count);
    const QString canonical_key = canonicalTileKey(provider, z, wrapped_x, y);

    // Known permanent tiles and ordinary RAM hits can return immediately.
    if (this->permanent_data_directory.isEmpty() ||
        this->permanent_cached_keys.contains(canonical_key))
    {
        const QByteArray *cached = this->tile_memory_cache.object(canonical_key);
        if (cached != nullptr && !cached->isEmpty())
            return { TileRequestStatus::Ready, *cached };
    }

    const QString file_name = QStringLiteral("%1/%2/%3.png").arg(z).arg(wrapped_x).arg(y);
    QString read_path;
    bool permanent = false;
    if (!this->permanent_data_directory.isEmpty())
    {
        const QString path = QDir(this->permanent_data_directory).filePath(
            QStringLiteral("maptiles/%1/%2").arg(provider, file_name));
        if (QFile::exists(path))
        {
            read_path = path;
            permanent = true;
        }
    }
    if (read_path.isEmpty())
    {
        const QByteArray *cached = this->tile_memory_cache.object(canonical_key);
        if (cached != nullptr && !cached->isEmpty())
            return { TileRequestStatus::Ready, *cached };
        const QString path = QDir(providerCachePath(provider)).filePath(file_name);
        if (QFile::exists(path)) read_path = path;
    }

    // Missing files follow the established download/coverage path unchanged.
    if (read_path.isEmpty()) return getTile(provider, z, x, y, key);

    // Coalesce disk reads for the same tile. Each HTTP request retains its own
    // response key; the first worker fans out completion to all requesters.
    const QString read_key = canonical_key + QLatin1Char(':') +
        QString::number(this->cache_generation);
    QHash<QString, QStringList>::iterator existing = this->cache_reads_pending.find(read_key);
    if (existing != this->cache_reads_pending.end())
    {
        existing.value().append(key);
        return { TileRequestStatus::Pending, {} };
    }
    if (this->cache_reads_pending.size() >= MaximumPendingCacheReads)
        return { TileRequestStatus::ServerBusy, {} };

    // Reserve bytes before scheduling a worker, not after the read completes.
    // Large or growing files must not exhaust memory beyond the job-count limit.
    const qint64 file_size = QFileInfo(read_path).size();
    if (file_size <= 0 || file_size > MaximumPendingCacheReadBytes ||
        file_size > MaximumPendingCacheReadBytes - this->pending_cache_read_bytes)
        return { TileRequestStatus::ServerBusy, {} };

    this->pending_cache_read_bytes += file_size;
    this->cache_reads_pending.insert(read_key, QStringList{key});
    const quint64 generation = this->cache_generation;
    QFutureWatcher<QByteArray> *watcher = new QFutureWatcher<QByteArray>(this);
    connect(watcher, &QFutureWatcher<QByteArray>::finished, this,
            [this, watcher, read_key, canonical_key, generation, permanent, provider, z, wrapped_x, y, file_size]()
    {
        this->pending_cache_read_bytes -= file_size;
        const QStringList request_keys = this->cache_reads_pending.take(read_key);
        const QByteArray data = watcher->result();
        watcher->deleteLater();
        if (generation == this->cache_generation && !data.isEmpty())
        {
            this->tile_memory_cache.insert(canonical_key,
                new QByteArray(data), tileMemoryCacheCostKiB(data));
            if (permanent) this->permanent_cached_keys.insert(canonical_key);
            for (const QString &response_key : request_keys) emit tileReady(response_key, data);
            return;
        }
        // Cache deletion or provider reconfiguration invalidates the original
        // request. Do not restart a remote fetch for a tile just deleted by an
        // administrator, and never deliver data from the old configuration.
        if (generation != this->cache_generation)
        {
            for (const QString &response_key : request_keys)
                emit tileFailed(response_key, TileFailureReason::Cancelled);
            return;
        }
        // An unchanged cache file may have vanished or become unreadable.
        // Fall back to the normal request path only for that genuine miss.
        for (const QString &response_key : request_keys)
        {
            const TileRequestResult result = getTile(provider, z, wrapped_x, y, response_key);
            if (result.status == TileRequestStatus::Ready)
                emit tileReady(response_key, result.data);
            else if (result.status == TileRequestStatus::InvalidRequest ||
                     result.status == TileRequestStatus::ServerBusy)
                emit tileFailed(response_key, TileFailureReason::UpstreamError);
        }
    });
    watcher->setFuture(QtConcurrent::run(&this->cache_read_pool, [read_path, file_size]()
    {
        QFile file(read_path);
        if (!file.open(QIODevice::ReadOnly)) return QByteArray();
        // An externally modified file can grow after admission; cap the actual
        // allocation and reject it rather than loading an arbitrary amount.
        const QByteArray data = file.read(file_size + 1);
        return data.size() == file_size && file.atEnd() ? data : QByteArray();
    }));
    return { TileRequestStatus::Pending, {} };
}

int MapTiles::deleteTiles(const QString &provider, int zoom, int tile_x_min, int tile_x_max,
                          int tile_y_min, int tile_y_max)
{
    // Reject malformed requests before invalidating unrelated pending reads or
    // waiting for any outstanding writes. The provider path may be empty for
    // an unknown provider, so validate it before changing cache state.
    if (zoom < 0 || zoom > 30 || tile_x_min > tile_x_max ||
        tile_y_min > tile_y_max)
        return -1;
    const QString cache_path = providerCachePath(provider);
    if (cache_path.isEmpty()) return -1;

    const int tile_count = 1 << zoom;
    const int bounded_y_min = qMax(0, tile_y_min);
    const int bounded_y_max = qMin(tile_count - 1, tile_y_max);
    if (bounded_y_min > bounded_y_max)
        return 0;

    // Invalidate outstanding cold reads without waiting for disk workers.
    // They only read independent file paths; their completion callback checks
    // this generation before publishing bytes or retrying a remote request.
    ++this->cache_generation;
    // Only overlapping writes can recreate files inside the deletion range.
    // Writes to unrelated tiles must not stall this administrative request.
    // The pending-key set is maintained on the owning Qt thread, so the
    // decision cannot race a newly scheduled write until this method returns.
    bool overlapping_write = false;
    for (const QString &write_key : this->cache_writes_pending_keys)
    {
        QString write_provider;
        int write_zoom = 0;
        int write_x = 0;
        int write_y = 0;
        if (!parseCanonicalTileKey(write_key, &write_provider, &write_zoom,
                                   &write_x, &write_y))
            continue;
        if (write_provider == provider && write_zoom == zoom &&
            write_y >= bounded_y_min && write_y <= bounded_y_max &&
            tileXInsideRange(write_x, tile_count, tile_x_min, tile_x_max))
        {
            overlapping_write = true;
            break;
        }
    }
    // Preserve the strict write-before-delete guarantee for overlapping
    // tiles. This remaining wait will be removed when writes and deletion
    // have a per-tile coordination mechanism.
    if (overlapping_write)
        this->cache_write_pool.waitForDone();

    {
        QMutexLocker locker(&this->downloads_mutex);
        QSet<QString> downloads_in_progress = this->downloads_active;
        downloads_in_progress.unite(this->downloads_queued_keys);
        downloads_in_progress.unite(this->cache_writes_pending_keys);
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

            QFile::remove(tile_path + QStringLiteral(".http.json"));
            const QString removed_key = canonicalTileKey(provider, zoom, tile_x, tile_y);
            this->permanent_cached_keys.remove(removed_key);
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

    return this->downloads_active_per_origin.value(downloadOrigin(download.url), 0) <
           MaximumActiveDownloadsPerOrigin;
}

void MapTiles::markDownloadStartedLocked(const QueuedDownload &download)
{
    this->downloads_active.insert(download.canonical_key);
    const QString origin = downloadOrigin(download.url);
    this->downloads_active_per_origin.insert(
        origin, this->downloads_active_per_origin.value(origin, 0) + 1);
}

void MapTiles::markDownloadFinishedLocked(const QueuedDownload &download)
{
    this->downloads_active.remove(download.canonical_key);

    const QString origin = downloadOrigin(download.url);
    const int remaining = this->downloads_active_per_origin.value(origin, 0) - 1;
    if (remaining > 0)
        this->downloads_active_per_origin.insert(origin, remaining);
    else
        this->downloads_active_per_origin.remove(origin);
}

void MapTiles::startMapTileDownload(const QueuedDownload &download)
{
    TileHttpClient *rest = new TileHttpClient(this->network_manager, download.url, this);
    {
        QMutexLocker locker(&this->downloads_mutex);
        this->downloads_active_clients.insert(download.canonical_key, rest);
    }
    connect(rest, &TileHttpClient::requestFinished, this,
            [this, rest, download](const QByteArray &data, bool may_store)
    {
        bool invalidated = false;
        {
            QMutexLocker locker(&this->downloads_mutex);
            markDownloadFinishedLocked(download);
            this->downloads_active_clients.remove(download.canonical_key);
            invalidated = this->downloads_invalidated.remove(download.canonical_key);
        }

        rest->deleteLater();
        startQueuedDownloads();
        const UpstreamActivity activity = upstreamActivity();
        emit upstreamActivityChanged(activity.active, activity.queued);

        // Keep persistence off the network event loop. Response completion is
        // deliberately deferred until the disk write has finished, preserving
        // the existing tileReady / on-disk cache contract.
        if (invalidated || !may_store)
        {
            if (!download.response_key.isEmpty()) emit tileReady(download.response_key, data);
            return;
        }

        if (this->pending_cache_writes >= MaximumPendingCacheWrites ||
            qint64(data.size()) > MaximumPendingCacheWriteBytes - this->pending_cache_write_bytes)
        {
            // Bounded overload: return the tile rather than grow an unlimited
            // write backlog. The tile is returned without persistence when saturated.
            qWarning() << "Tile cache write capacity exhausted:" << download.tile_path;
            if (!download.response_key.isEmpty()) emit tileReady(download.response_key, data);
            return;
        }

        // Publish to RAM before scheduling persistence so other requests for
        // the same tile do not launch duplicate upstream fetches while the
        // disk worker is busy. Disk publication still finishes before tileReady.
        this->tile_memory_cache.insert(download.canonical_key,
            new QByteArray(data), tileMemoryCacheCostKiB(data));
        ++this->pending_cache_writes;
        this->pending_cache_write_bytes += qint64(data.size());
        this->cache_writes_pending_keys.insert(download.canonical_key);
        QFutureWatcher<bool> *watcher = new QFutureWatcher<bool>(this);
        connect(watcher, &QFutureWatcher<bool>::finished, this,
                [this, watcher, download, data]()
        {
            --this->pending_cache_writes;
            this->pending_cache_write_bytes -= qint64(data.size());
            this->cache_writes_pending_keys.remove(download.canonical_key);
            // Cache deletion may have invalidated the write while it ran.
            // The deleting code waits for writers before removing disk files.
            {
                QMutexLocker locker(&this->downloads_mutex);
                this->downloads_invalidated.remove(download.canonical_key);
            }
            if (!watcher->result())
                qWarning() << "Tile cache persistence failed:" << download.tile_path;
            if (!download.response_key.isEmpty()) emit tileReady(download.response_key, data);
            watcher->deleteLater();
        });
        watcher->setFuture(QtConcurrent::run(&this->cache_write_pool,
            [data, tile_path = download.tile_path]()
        {
            return MapTiles::saveMapTile(data, tile_path);
        }));
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
        if (!download.response_key.isEmpty()) emit tileFailed(download.response_key, failure_reason);
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
