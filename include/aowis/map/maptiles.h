#ifndef MAPTILES_H
#define MAPTILES_H

#include <QObject>
#include <aowis/map/map_provider.h>
#include <aowis/map/cache_storage.h>
#include <aowis/map/map_cascade_config.h>
#include <aowis/map/cascade_upstream_health.h>
#include <aowis/map/download_coverage.h>

#include <QByteArray>
#include <QCache>
#include <QHash>
#include <QMutex>
#include <QNetworkAccessManager>
#include <QQueue>
#include <QSharedPointer>
#include <atomic>
#include <functional>
#include <QSet>
#include <QThreadPool>
#include <QString>
#include <QStringList>
#include <QVector>

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
        Unavailable,
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
    // May be called at any time on the MapTiles thread (live
    // reconfiguration); see the definition for downloads in flight.
    void setCascadeConfig(const Aowis::Map::MapCascadeConfig &config);
    void setPermanentDataDirectory(const QString &directory);
    void configureProviders(const QVector<Aowis::Map::MapProvider> &providers,
                            const Aowis::Map::DownloadCoverage &coverage,
                            const QString &default_coverage,
                            const QHash<QString, QSet<QString>> &inherited_upstreams = {},
                            const QHash<QString, QStringList> &upstream_routes = {},
                            const QHash<QString, Aowis::Map::MapProviderAcquisitionMode> &modes = {});

    TileRequestResult getTile(const QString &provider, int z, int x, int y, const QString &key,
                              const QByteArray &cascade_trace = {});
    // A source digest binds a provider UUID to its actual imagery/style URL.
    QByteArray rasterSourceFingerprint(const QString &provider) const;
    bool allowsLossyRasterTransfer(const QString &provider) const;
    // Reject malformed traces, exhausted hop budgets, and our own instance ID.
    bool acceptsCascadeTrace(const QByteArray &trace) const;
    // True while a remote download of this tile is running or queued (not
    // when getTileForHttp() is merely reading it from the disk cache).
    bool tileDownloadInProgress(const QString &provider, int z, int x, int y) const;
    QByteArray cascadeInstanceId() const { return this->cascade_instance_id; }
    // HTTP path: offload established cold disk hits without occupying the event loop.
    TileRequestResult getTileForHttp(const QString &provider, int z, int x, int y, const QString &key,
                                     const QByteArray &cascade_trace = {});
    // Count of deleted tiles, -1 for an invalid request, < -1 on failure.
    int deleteTiles(const QString &provider, int zoom, int tile_x_min, int tile_x_max,
                    int tile_y_min, int tile_y_max);
    // deleteTiles() with the file removal on a worker: cache state is
    // invalidated at once, completed() runs later on the MapTiles thread.
    void deleteTilesAsync(const QString &provider, int zoom, int tile_x_min, int tile_x_max,
                          int tile_y_min, int tile_y_max,
                          const std::function<void(int)> &completed);
    UpstreamActivity upstreamActivity() const;
    // Reachability, cooldown and learned instance ID per upstream name.
    QHash<QString, Aowis::Map::CascadeUpstreamState> upstreamHealth() const;
    void cancelUpstreamDownloads();
    // Disk scan: run off the event-loop thread. Permanent data is excluded.
    Aowis::Map::CacheStorageStatistics cacheStorageStatistics() const;
    bool flushCacheAccessIndex();
    // Opt-in soft limit of ordinary raster payloads only. Call on the
    // MapTiles thread; policy changes may wait for current maintenance.
    bool setCacheStoragePolicy(const Aowis::Map::CacheStoragePolicy &policy);
    Aowis::Map::CacheCleanupResult lastCacheCleanup() const;
    // Resolve the current ordinary provider variant on the MapTiles thread.
    // Empty means unknown provider. Existing pins stay with their old variant.
    std::string cacheProtectionNamespace(const QString &provider) const;
    bool replaceCacheProtectionGroup(const Aowis::Map::CacheProtectionGroup &group);
    bool removeCacheProtectionGroup(const std::string &id);
    std::optional<std::vector<Aowis::Map::CacheProtectionGroup>> cacheProtectionGroups() const;

private:
    friend class MapTilesCacheWriteTestAccess;

    struct CacheWriteState
    {
        QMutex mutex;
        std::atomic_bool invalidated{false};
    };

    struct DownloadSource
    {
        QString url;
        QByteArray api_key;
        bool cascade = false;
        QString upstream_name; // Cascade sources only
        // A WireGuard upstream: requested through its tunnel's proxy.
        bool tunnelled = false;
        QNetworkProxy proxy;
    };

    struct QueuedDownload
    {
        // URL of the attempt in progress (the first source until a failover):
        // per-origin admission is charged to the origin actually contacted.
        QString url;
        QString tile_path; // Without extension; saveMapTile() adds the format's.
        QString canonical_key;
        QString response_key;
        QByteArray source_fingerprint;
        QByteArray cascade_trace;
        QVector<DownloadSource> sources;
    };

    // One raster cache deletion, captured on the MapTiles thread.
    struct TileDeletionPlan
    {
        QString cache_path;
        QString provider;
        int zoom = 0;
        int tile_count = 1;
        int tile_x_min = 0;
        int tile_x_max = 0;
        int tile_y_min = 0; // Clamped to the zoom's rows.
        int tile_y_max = 0;
        QList<QSharedPointer<CacheWriteState>> overlapping_writes;
    };

    struct TileDeletionOutcome
    {
        int deleted_count = 0;
        QStringList removed_keys;
    };

    // -1: invalid, 0: nothing to delete, 1: plan ready (state invalidated).
    int prepareTileDeletion(const QString &provider, int zoom, int tile_x_min, int tile_x_max,
                            int tile_y_min, int tile_y_max, TileDeletionPlan *plan);
    void evictMemoryTiles(const TileDeletionPlan &plan);
    TileDeletionOutcome removeTileFiles(const TileDeletionPlan &plan) const;
    void finishTileDeletion(const TileDeletionPlan &plan, const TileDeletionOutcome &outcome);

    enum class DownloadScheduleStatus
    {
        Scheduled,
        QueueFull,
        // The tile is already being downloaded from an upstream that is
        // itself waiting for this request (see scheduleMapTileDownload).
        LoopRefused
    };

    QString providerCachePath(const QString &provider) const;
    QString providerUrl(const QString &provider, int z, int x, int y) const;
    QString canonicalTileKey(const QString &provider, int zoom, int x, int y) const;

    DownloadScheduleStatus scheduleMapTileDownload(const QueuedDownload &download);
    void startMapTileDownload(const QueuedDownload &download);
    void attemptMapTileDownload(const QueuedDownload &download, int source_index);
    void startQueuedDownloads();
    bool canStartDownloadLocked(const QueuedDownload &download) const;
    void markDownloadStartedLocked(const QueuedDownload &download);
    void markDownloadFinishedLocked(const QueuedDownload &download);
    void moveDownloadOriginLocked(const QString &from_url, const QString &to_url);
    // The download's own response key plus those of later requests for the
    // same canonical tile that joined it (e.g. x and x + 2^z).
    QStringList takeResponseKeysLocked(const QueuedDownload &download);
    // Writes <tile_base_path>.png/.jpg/.webp according to the data's format
    // and removes the tile's files in other formats.
    static bool saveMapTile(const QByteArray &data, const QString &tile_base_path);

    QString domainRandomizer(const QString &url) const;
    std::string cacheStorageKey(const QString &tile_base_path) const;
    void recordCacheUse(const QString &tile_base_path) const;
    void recordMemoryCacheUse(const QString &provider, int z, int x, int y,
                              const QString &canonical_key) const;

    QString fscache_base;
    mutable Aowis::Map::CacheAccessIndex cache_access_index;
    QString permanent_data_directory;
    bool remote_fetch_enabled = true;
    Aowis::Map::MapCascadeConfig cascade_config;
    QByteArray cascade_instance_id;
    // Exponential cooldowns of unreachable upstreams and their learned
    // instance IDs. An answered failure never suppresses an upstream.
    Aowis::Map::CascadeUpstreamHealth upstream_health;
    QHash<QString, Aowis::Map::MapProvider> configured_providers;
    // Remote-only providers can never use a direct origin URL fallback.
    QHash<QString, QSet<QString>> inherited_upstreams;
    QHash<QString, QStringList> upstream_routes;
    QHash<QString, Aowis::Map::MapProviderAcquisitionMode> acquisition_modes;
    // First-use URL fingerprint selection. Separates cache contents when an
    // administrator changes the upstream URL without changing the UUID.
    mutable QHash<QString, QString> provider_cache_paths;
    Aowis::Map::DownloadCoverage download_coverage;
    QString default_coverage;
    QNetworkAccessManager *network_manager;
    // Per running WireGuard tunnel (cascadeProxyKey), see setCascadeConfig.
    QHash<QString, QNetworkAccessManager *> tunnel_network_managers;
    QNetworkAccessManager *tunnelNetworkManager(const QNetworkProxy &proxy);
    QThreadPool cache_write_pool;
    QThreadPool cache_read_pool;
    // One worker: deletions run in request order.
    QThreadPool cache_delete_pool;
    QHash<QString, QStringList> cache_reads_pending;
    quint64 cache_generation = 0;
    qint64 pending_cache_read_bytes = 0;
    int pending_cache_writes = 0;
    qint64 pending_cache_write_bytes = 0;
    QSet<QString> cache_writes_pending_keys;
    QHash<QString, QSharedPointer<CacheWriteState>> cache_write_states;
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
    QHash<QString, QStringList> downloads_joined_response_keys;
    // Upstream name each running or queued download is asking now (empty:
    // the original provider). A cascade request must not join a download
    // that is waiting for the very upstream the request came from.
    QHash<QString, QString> downloads_current_upstream;

signals:
    void tileReady(QString key, QByteArray data);
    void tileFailed(const QString &key, TileFailureReason reason);
    void upstreamActivityChanged(int active, int queued);
};

#endif // MAPTILES_H
