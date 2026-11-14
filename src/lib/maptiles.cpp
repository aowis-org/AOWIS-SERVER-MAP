#include <aowis/map/maptiles.h>

#include "http_client_tilefetch.h"

#include <QDebug>
#include <QDateTime>
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
#include <QUuid>

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

// Cached tiles are stored under the extension of their actual format. A MAP
// upstream may answer a .png URL with JPEG (adaptive slow-link transfer), and
// some providers deliver JPEG or WebP; storing those as .png misled every
// tool looking at the cache. Unknown bytes keep the historical .png name.
QString rasterTileExtension(const QByteArray &data)
{
    if (data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xff &&
        static_cast<unsigned char>(data[1]) == 0xd8 && static_cast<unsigned char>(data[2]) == 0xff)
        return QStringLiteral("jpg");
    if (data.size() >= 12 && data.startsWith("RIFF") && data.mid(8, 4) == "WEBP")
        return QStringLiteral("webp");
    return QStringLiteral("png");
}

// Every name a cached tile may have, in lookup order. .png first: it is the
// name of all tiles cached before formats were told apart (whatever their
// bytes), so existing caches keep working unchanged.
const QStringList &rasterTileExtensions()
{
    static const QStringList extensions = {
        QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("webp")};
    return extensions;
}

// "<directory>/<z>/<x>/<y>" without extension.
QString rasterTileBasePath(const QString &directory, int z, int x, int y)
{
    return QDir(directory).filePath(QStringLiteral("%1/%2/%3").arg(z).arg(x).arg(y));
}

// The existing cached file of a tile, or empty.
QString existingRasterTilePath(const QString &base_path)
{
    for (const QString &extension : rasterTileExtensions())
    {
        const QString path = base_path + QLatin1Char('.') + extension;
        if (QFile::exists(path)) return path;
    }
    return {};
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
    this->cascade_instance_id = QUuid::createUuid().toString(QUuid::WithoutBraces).toLatin1();
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

void MapTiles::setCascadeConfig(const Aowis::Map::MapCascadeConfig &config)
{
    // Live reconfiguration: later requests use the new upstream order,
    // credentials and options. When the upstreams or the direct-fetch policy
    // change, downloads in flight were chosen under the previous policy: they
    // are canceled (requesters see TileFailureReason::Cancelled and retry)
    // and the previous upstreams' cooldowns are forgotten. Cached tiles stay
    // valid.
    const bool sources_changed = this->cascade_config.upstreams != config.upstreams ||
        this->cascade_config.allow_direct_fetch != config.allow_direct_fetch;
    this->cascade_config = config;
    if (!sources_changed)
        return;
    this->upstream_health.reset();
    cancelUpstreamDownloads();
    // Managers of tunnels no longer configured (stopped or restarted on
    // another port) go once the canceled replies are finished.
    QSet<QString> current_tunnels;
    for (const Aowis::Map::MapCascadeUpstream &upstream : config.upstreams)
        if (Aowis::Map::hasWireGuardTransport(upstream))
            current_tunnels.insert(Aowis::Map::cascadeProxyKey(upstream.proxy));
    for (QHash<QString, QNetworkAccessManager *>::iterator manager = this->tunnel_network_managers.begin();
         manager != this->tunnel_network_managers.end();)
    {
        if (current_tunnels.contains(manager.key()))
        {
            ++manager;
            continue;
        }
        manager.value()->deleteLater();
        manager = this->tunnel_network_managers.erase(manager);
    }
}

QNetworkAccessManager *MapTiles::tunnelNetworkManager(const QNetworkProxy &proxy)
{
    const QString key = Aowis::Map::cascadeProxyKey(proxy);
    QNetworkAccessManager *manager = this->tunnel_network_managers.value(key);
    if (manager != nullptr)
        return manager;
    // One manager per tunnel: the proxy applies to its requests only.
    manager = new QNetworkAccessManager(this);
    manager->setProxy(proxy);
    this->tunnel_network_managers.insert(key, manager);
    return manager;
}

QByteArray MapTiles::rasterSourceFingerprint(const QString &provider) const
{
    const QHash<QString, Aowis::Map::MapProvider>::const_iterator source =
        this->configured_providers.constFind(provider);
    if (source == this->configured_providers.cend()) return {};
    return QCryptographicHash::hash(source->url_template.toUtf8(),
                                    QCryptographicHash::Sha256).toHex();
}

bool MapTiles::allowsLossyRasterTransfer(const QString &provider) const
{
    const QHash<QString, Aowis::Map::MapProvider>::const_iterator found =
        this->configured_providers.constFind(provider);
    return found != this->configured_providers.constEnd() && found->allow_lossy_transfer;
}

bool MapTiles::acceptsCascadeTrace(const QByteArray &trace) const
{
    if (trace.isEmpty()) return true;
    // Eight total hops maximum. Strict ASCII syntax avoids header injection and
    // forwards an unambiguous visited-instance set across all cascade levels.
    if (trace.size() > 8 * 36 + 7) return false;
    const QList<QByteArray> ids = trace.split(',');
    if (ids.size() >= 8) return false;
    QSet<QByteArray> visited;
    for (const QByteArray &id : ids)
    {
        if (id.size() != 36 || id == this->cascade_instance_id || visited.contains(id))
            return false;
        for (int index = 0; index < id.size(); ++index)
        {
            const char character = id.at(index);
            const bool hyphen = index == 8 || index == 13 || index == 18 || index == 23;
            if (hyphen ? character != '-' :
                !((character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f')))
                return false;
        }
        visited.insert(id);
    }
    return true;
}

void MapTiles::configureProviders(const QVector<Aowis::Map::MapProvider> &providers,
                                  const Aowis::Map::DownloadCoverage &coverage,
                                  const QString &default_coverage,
                                  const QHash<QString, QSet<QString>> &inherited_upstreams,
                                  const QHash<QString, QStringList> &upstream_routes,
                                  const QHash<QString, Aowis::Map::MapProviderAcquisitionMode> &modes)
{
    QHash<QString, Aowis::Map::MapProvider> next;
    for (const Aowis::Map::MapProvider &provider : providers)
        if (provider.enabled) next.insert(provider.id, provider);
    // Cached tiles belong to a provider ID and its source URL (see
    // providerCachePath). Only a change of that pairing invalidates them.
    // Provider discovery reapplies the catalog whenever an upstream's
    // advertisement changes (routes, inherited upstreams, modes); resetting
    // the caches each time canceled in-flight disk reads (HTTP 503) and
    // emptied the RAM cache for nothing.
    bool sources_changed = next.size() != this->configured_providers.size();
    for (QHash<QString, Aowis::Map::MapProvider>::const_iterator provider = next.constBegin();
         !sources_changed && provider != next.constEnd(); ++provider)
    {
        const QHash<QString, Aowis::Map::MapProvider>::const_iterator current =
            this->configured_providers.constFind(provider.key());
        sources_changed = current == this->configured_providers.constEnd() ||
                          current->url_template != provider->url_template;
    }
    this->inherited_upstreams = inherited_upstreams;
    this->upstream_routes = upstream_routes;
    this->acquisition_modes = modes;
    this->configured_providers = next;
    this->download_coverage = coverage;
    this->default_coverage = default_coverage;
    if (!sources_changed)
        return;
    ++this->cache_generation;
    this->permanent_cached_keys.clear();
    this->provider_cache_paths.clear();
    this->tile_memory_cache.clear();
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
    const QHash<QString, Aowis::Map::MapProvider>::const_iterator it =
        this->configured_providers.constFind(provider);
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
                                              const QString &key,
                                              const QByteArray &cascade_trace)
{
    if (!acceptsCascadeTrace(cascade_trace))
        return { TileRequestStatus::Unavailable, {} };
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
    const QString tile_base_path = rasterTileBasePath(cache_path, z, wrapped_x, y);
    const Aowis::Map::MapProvider selected_provider = this->configured_providers.value(provider);

    if (!this->permanent_data_directory.isEmpty())
    {
        if (this->permanent_cached_keys.contains(canonical_key))
        {
            const QByteArray *cached = this->tile_memory_cache.object(canonical_key);
            if (cached != nullptr) return { TileRequestStatus::Ready, *cached };
        }
        QFile permanent_tile(existingRasterTilePath(rasterTileBasePath(
            QDir(this->permanent_data_directory).filePath(QStringLiteral("maptiles/%1").arg(provider)),
            z, wrapped_x, y)));
        if (!permanent_tile.fileName().isEmpty() && permanent_tile.open(QIODevice::ReadOnly))
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

    QFile file(existingRasterTilePath(tile_base_path));
    if (!file.fileName().isEmpty() && file.open(QIODevice::ReadOnly))
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
    QueuedDownload download;
    download.tile_path = tile_base_path;
    download.canonical_key = canonical_key;
    download.response_key = key;
    download.source_fingerprint = rasterSourceFingerprint(provider);
    download.cascade_trace = cascade_trace.isEmpty()
        ? this->cascade_instance_id : cascade_trace + ',' + this->cascade_instance_id;
    const QString path = QStringLiteral("/%1/%2/%3/%4.png")
        .arg(provider).arg(z).arg(wrapped_x).arg(y);
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    // IDs of the MAP servers this request already passed through. An
    // upstream among them is waiting for us: asking it can only time out.
    const QList<QByteArray> trace_ids = Aowis::Map::cascadeTraceIds(cascade_trace);
    const bool inherited = this->inherited_upstreams.contains(provider);
    Aowis::Map::MapProviderAcquisitionMode mode = this->acquisition_modes.value(
        provider, inherited ? Aowis::Map::MapProviderAcquisitionMode::Cascade
                            : Aowis::Map::MapProviderAcquisitionMode::Automatic);
    // Administrative policy always wins over stale per-provider preferences.
    // Direct is never permitted for inherited providers.
    if ((inherited || !this->cascade_config.allow_direct_fetch) &&
        mode == Aowis::Map::MapProviderAcquisitionMode::Direct)
        mode = Aowis::Map::MapProviderAcquisitionMode::Cascade;
    // A configured route is an ordered *subset*, never an extra origin.
    // Without an override preserve the globally configured priority.
    QStringList preferred = this->upstream_routes.value(provider);
    if (preferred.isEmpty())
        for (const Aowis::Map::MapCascadeUpstream &upstream : this->cascade_config.upstreams)
            preferred.append(upstream.name);
    if (mode != Aowis::Map::MapProviderAcquisitionMode::Direct)
    for (const QString &name : preferred)
    {
        for (const Aowis::Map::MapCascadeUpstream &upstream : this->cascade_config.upstreams)
        {
            if (upstream.name != name) continue;
            // A WireGuard upstream only through its running tunnel, never directly.
            if (!Aowis::Map::cascadeUpstreamUsable(upstream)) break;
            const QString upstream_origin = upstream.url.toString(
                QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment);
            if (this->upstream_health.coolingDown(upstream.name, now_ms) ||
                this->upstream_health.instanceInTrace(upstream.name, trace_ids) ||
                (this->inherited_upstreams.contains(provider) &&
                 !this->inherited_upstreams.value(provider).contains(upstream.name)))
                break;
            DownloadSource source;
            source.url = upstream_origin + path;
            source.api_key = upstream.api_key;
            source.cascade = true;
            source.upstream_name = upstream.name;
            if (Aowis::Map::hasWireGuardTransport(upstream))
            {
                source.tunnelled = true;
                source.proxy = upstream.proxy;
            }
            download.sources.append(source);
            break;
        }
    }
    if (this->cascade_config.allow_direct_fetch && !inherited &&
        mode != Aowis::Map::MapProviderAcquisitionMode::Cascade)
    {
        DownloadSource source;
        source.url = providerUrl(provider, z, wrapped_x, y);
        download.sources.append(source);
    }
    if (download.sources.isEmpty())
        return { TileRequestStatus::Unavailable, {} };
    download.url = download.sources.first().url;
    const DownloadScheduleStatus scheduled = scheduleMapTileDownload(download);
    if (scheduled == DownloadScheduleStatus::QueueFull)
        return { TileRequestStatus::ServerBusy, {} };
    if (scheduled == DownloadScheduleStatus::LoopRefused)
        return { TileRequestStatus::Unavailable, {} };

    return { TileRequestStatus::Pending, {} };
}

MapTiles::TileRequestResult MapTiles::getTileForHttp(
    const QString &provider, int z, int x, int y, const QString &key,
    const QByteArray &cascade_trace)
{
    if (!acceptsCascadeTrace(cascade_trace))
        return { TileRequestStatus::Unavailable, {} };
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

    QString read_path;
    bool permanent = false;
    if (!this->permanent_data_directory.isEmpty())
    {
        read_path = existingRasterTilePath(rasterTileBasePath(
            QDir(this->permanent_data_directory).filePath(QStringLiteral("maptiles/%1").arg(provider)),
            z, wrapped_x, y));
        permanent = !read_path.isEmpty();
    }
    if (read_path.isEmpty())
    {
        const QByteArray *cached = this->tile_memory_cache.object(canonical_key);
        if (cached != nullptr && !cached->isEmpty())
            return { TileRequestStatus::Ready, *cached };
        read_path = existingRasterTilePath(
            rasterTileBasePath(providerCachePath(provider), z, wrapped_x, y));
    }

    // Missing files follow the established download/coverage path unchanged.
    if (read_path.isEmpty()) return getTile(provider, z, x, y, key, cascade_trace);

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
            [this, watcher, read_key, canonical_key, generation, permanent, provider, z, wrapped_x, y, file_size, cascade_trace]()
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
            const TileRequestResult result = getTile(provider, z, wrapped_x, y, response_key, cascade_trace);
            if (result.status == TileRequestStatus::Ready)
                emit tileReady(response_key, result.data);
            else if (result.status == TileRequestStatus::InvalidRequest ||
                     result.status == TileRequestStatus::Unavailable ||
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
    // Invalidate only overlapping writes. A worker holds its tile's mutex
    // while publishing with QSaveFile; once we acquire and release that
    // mutex, an already-running write has finished. Queued workers see the
    // invalidation marker and must not publish after deletion.
    QList<QSharedPointer<CacheWriteState>> overlapping_writes;
    for (QHash<QString, QSharedPointer<CacheWriteState>>::const_iterator it =
             this->cache_write_states.cbegin();
         it != this->cache_write_states.cend(); ++it)
    {
        QString write_provider;
        int write_zoom = 0;
        int write_x = 0;
        int write_y = 0;
        if (!parseCanonicalTileKey(it.key(), &write_provider, &write_zoom,
                                   &write_x, &write_y))
            continue;
        if (write_provider == provider && write_zoom == zoom &&
            write_y >= bounded_y_min && write_y <= bounded_y_max &&
            tileXInsideRange(write_x, tile_count, tile_x_min, tile_x_max))
        {
            it.value()->invalidated.store(true, std::memory_order_release);
            overlapping_writes.append(it.value());
        }
    }
    // This waits only for writes that actually overlap the requested tiles,
    // never for unrelated queued work. New workers skip invalidated writes.
    for (const QSharedPointer<CacheWriteState> &state : overlapping_writes)
    {
        QMutexLocker locker(&state->mutex);
    }

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
        QStringList name_filters;
        for (const QString &extension : rasterTileExtensions())
            name_filters << QStringLiteral("*.") + extension;
        const QStringList tile_file_names = x_directory.entryList(name_filters, QDir::Files);
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

QHash<QString, Aowis::Map::CascadeUpstreamState> MapTiles::upstreamHealth() const
{
    return this->upstream_health.snapshot();
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
    QStringList queued_response_keys;
    {
        QMutexLocker locker(&this->downloads_mutex);
        active_clients = this->downloads_active_clients.values();
        while (!this->downloads_queued.isEmpty())
        {
            const QueuedDownload download = this->downloads_queued.dequeue();
            this->downloads_current_upstream.remove(download.canonical_key);
            queued_response_keys.append(takeResponseKeysLocked(download));
            this->downloads_invalidated.remove(download.canonical_key);
        }
        this->downloads_queued_keys.clear();
    }

    for (const QString &response_key : queued_response_keys)
        emit tileFailed(response_key, TileFailureReason::Cancelled);

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
            // A, B upstreams of each other, both missing the same tile: A's
            // download asks B, and B's download asks A. Joining each other's
            // request would make both wait until the transfer timeout. The
            // one with the higher instance ID refuses: the other gets a fast,
            // answered "unavailable", tries its next source, and its result
            // then also completes the download waiting for it.
            // download.cascade_trace is the incoming trace plus our own ID.
            const QString asked = this->downloads_current_upstream.value(download.canonical_key);
            if (!asked.isEmpty() &&
                this->upstream_health.refusesJoin(
                    asked, Aowis::Map::cascadeTraceIds(download.cascade_trace),
                    this->cascade_instance_id))
                return DownloadScheduleStatus::LoopRefused;
            // Join the running or queued download. Without this, a request
            // with a different response key would never receive a result.
            if (!download.response_key.isEmpty())
            {
                QStringList &joined = this->downloads_joined_response_keys[download.canonical_key];
                if (!joined.contains(download.response_key))
                    joined.append(download.response_key);
            }
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
        this->downloads_current_upstream.insert(download.canonical_key,
                                                download.sources.first().upstream_name);
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
    this->downloads_current_upstream.remove(download.canonical_key);

    const QString origin = downloadOrigin(download.url);
    const int remaining = this->downloads_active_per_origin.value(origin, 0) - 1;
    if (remaining > 0)
        this->downloads_active_per_origin.insert(origin, remaining);
    else
        this->downloads_active_per_origin.remove(origin);
}

void MapTiles::moveDownloadOriginLocked(const QString &from_url, const QString &to_url)
{
    const QString from = downloadOrigin(from_url);
    const QString to = downloadOrigin(to_url);
    if (from == to) return;
    const int remaining = this->downloads_active_per_origin.value(from, 0) - 1;
    if (remaining > 0)
        this->downloads_active_per_origin.insert(from, remaining);
    else
        this->downloads_active_per_origin.remove(from);
    // May briefly exceed MaximumActiveDownloadsPerOrigin: the slot is
    // already held, and a failover must not wait behind new requests.
    this->downloads_active_per_origin.insert(to, this->downloads_active_per_origin.value(to, 0) + 1);
}

QStringList MapTiles::takeResponseKeysLocked(const QueuedDownload &download)
{
    QStringList keys;
    if (!download.response_key.isEmpty())
        keys.append(download.response_key);
    const QStringList joined = this->downloads_joined_response_keys.take(download.canonical_key);
    for (const QString &key : joined)
    {
        if (!keys.contains(key))
            keys.append(key);
    }
    return keys;
}

void MapTiles::startMapTileDownload(const QueuedDownload &download)
{
    attemptMapTileDownload(download, 0);
}

void MapTiles::attemptMapTileDownload(const QueuedDownload &download, int source_index)
{
    const DownloadSource &source = download.sources.at(source_index);
    TileHttpClient *rest = new TileHttpClient(
        source.tunnelled ? tunnelNetworkManager(source.proxy) : this->network_manager,
        source.url, this);
    if (source.cascade)
        rest->configureCascade(source.api_key, download.source_fingerprint,
                               download.cascade_trace, this->cascade_config.poor_internet_mode,
                               this->cascade_config.jpeg_quality);
    {
        QMutexLocker locker(&this->downloads_mutex);
        this->downloads_active_clients.insert(download.canonical_key, rest);
        this->downloads_current_upstream.insert(download.canonical_key, source.upstream_name);
    }
    connect(rest, &TileHttpClient::requestFinished, this,
            [this, rest, download, source_index](const QByteArray &data, bool may_store)
    {
        const DownloadSource &succeeded_source = download.sources.at(source_index);
        if (succeeded_source.cascade)
            this->upstream_health.recordAnswered(succeeded_source.upstream_name,
                                                 rest->responderInstance(),
                                                 QDateTime::currentMSecsSinceEpoch());
        bool invalidated = false;
        QStringList response_keys;
        {
            QMutexLocker locker(&this->downloads_mutex);
            markDownloadFinishedLocked(download);
            this->downloads_active_clients.remove(download.canonical_key);
            invalidated = this->downloads_invalidated.remove(download.canonical_key);
            response_keys = takeResponseKeysLocked(download);
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
            for (const QString &response_key : response_keys) emit tileReady(response_key, data);
            return;
        }

        if (this->pending_cache_writes >= MaximumPendingCacheWrites ||
            qint64(data.size()) > MaximumPendingCacheWriteBytes - this->pending_cache_write_bytes)
        {
            // Bounded overload: return the tile rather than grow an unlimited
            // write backlog. The tile is returned without persistence when saturated.
            qWarning() << "Tile cache write capacity exhausted:" << download.tile_path;
            for (const QString &response_key : response_keys) emit tileReady(response_key, data);
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
        const QSharedPointer<CacheWriteState> write_state =
            QSharedPointer<CacheWriteState>::create();
        this->cache_write_states.insert(download.canonical_key, write_state);
        QFutureWatcher<bool> *watcher = new QFutureWatcher<bool>(this);
        connect(watcher, &QFutureWatcher<bool>::finished, this,
                [this, watcher, download, data, write_state, response_keys]()
        {
            --this->pending_cache_writes;
            this->pending_cache_write_bytes -= qint64(data.size());
            this->cache_writes_pending_keys.remove(download.canonical_key);
            this->cache_write_states.remove(download.canonical_key);
            // Cache deletion may have invalidated the write while it ran.
            // The deleting code waits for writers before removing disk files.
            {
                QMutexLocker locker(&this->downloads_mutex);
                this->downloads_invalidated.remove(download.canonical_key);
            }
            if (!watcher->result())
                qWarning() << "Tile cache persistence failed:" << download.tile_path;
            for (const QString &response_key : response_keys) emit tileReady(response_key, data);
            watcher->deleteLater();
        });
        watcher->setFuture(QtConcurrent::run(&this->cache_write_pool,
            [data, tile_path = download.tile_path, write_state]()
        {
            QMutexLocker locker(&write_state->mutex);
            if (write_state->invalidated.load(std::memory_order_acquire))
                return true; // deletion intentionally discarded persistence
            return MapTiles::saveMapTile(data, tile_path);
        }));
    });
    connect(rest, &TileHttpClient::requestError, this,
            [this, rest, download, source_index](TileHttpClient::RequestFailureReason reason, const QString &error)
    {
        if (reason == TileHttpClient::RequestFailureReason::Timeout)
            qWarning() << "Tile request timed out:" << download.response_key << error;
        else if (reason != TileHttpClient::RequestFailureReason::Cancelled)
            qWarning() << "Tile request failed:" << download.response_key << error;

        // Temporarily skip unreachable upstreams for new tile requests (with
        // an exponential cooldown). Answered failures (404, busy, source
        // failed, mismatch) describe this tile only and end any cooldown.
        const DownloadSource &failed_source = download.sources.at(source_index);
        if (failed_source.cascade && reason != TileHttpClient::RequestFailureReason::Cancelled)
        {
            const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
            if (rest->upstreamAnswered())
                this->upstream_health.recordAnswered(failed_source.upstream_name,
                                                     rest->responderInstance(), now_ms);
            else if (reason == TileHttpClient::RequestFailureReason::TransientError ||
                     reason == TileHttpClient::RequestFailureReason::Timeout)
                this->upstream_health.recordUnreachable(failed_source.upstream_name, error, now_ms);
        }

        // A failed attempt still owns the same active slot and joined waiters.
        // Do not release that slot until all configured sources have failed.
        bool invalidated = false;
        {
            QMutexLocker locker(&this->downloads_mutex);
            invalidated = this->downloads_invalidated.contains(download.canonical_key);
            if (this->downloads_active_clients.value(download.canonical_key) == rest)
                this->downloads_active_clients.remove(download.canonical_key);
        }
        if (reason != TileHttpClient::RequestFailureReason::Cancelled && !invalidated &&
            source_index + 1 < download.sources.size())
        {
            rest->deleteLater();
            QueuedDownload next = download;
            next.url = download.sources.at(source_index + 1).url;
            {
                QMutexLocker locker(&this->downloads_mutex);
                moveDownloadOriginLocked(download.url, next.url);
            }
            attemptMapTileDownload(next, source_index + 1);
            return;
        }

        QStringList response_keys;
        {
            QMutexLocker locker(&this->downloads_mutex);
            markDownloadFinishedLocked(download);
            this->downloads_invalidated.remove(download.canonical_key);
            response_keys = takeResponseKeysLocked(download);
        }

        TileFailureReason failure_reason = TileFailureReason::UpstreamError;
        if (reason == TileHttpClient::RequestFailureReason::Timeout)
            failure_reason = TileFailureReason::Timeout;
        else if (reason == TileHttpClient::RequestFailureReason::Cancelled)
            failure_reason = TileFailureReason::Cancelled;
        for (const QString &response_key : response_keys) emit tileFailed(response_key, failure_reason);
        rest->deleteLater();
        startQueuedDownloads();
        const UpstreamActivity activity = upstreamActivity();
        emit upstreamActivityChanged(activity.active, activity.queued);
    });
    rest->get(QString());
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

bool MapTiles::saveMapTile(const QByteArray &data, const QString &tile_base_path)
{
    // Accept a path with a known extension too (older callers, tests).
    QString base_path = tile_base_path;
    for (const QString &extension : rasterTileExtensions())
        if (base_path.endsWith(QLatin1Char('.') + extension))
        {
            base_path.chop(extension.size() + 1);
            break;
        }
    const QString extension = rasterTileExtension(data);
    const QString tile_path = base_path + QLatin1Char('.') + extension;
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

    // One file per tile: a tile cached before in another format (or under the
    // old always-.png name) would otherwise shadow the new one on lookup.
    for (const QString &other : rasterTileExtensions())
        if (other != extension)
        {
            const QString stale = base_path + QLatin1Char('.') + other;
            if (QFile::exists(stale))
            {
                QFile::remove(stale);
                QFile::remove(stale + QStringLiteral(".http.json"));
            }
        }
    return true;
}
