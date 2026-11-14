#ifndef SERVER_H
#define SERVER_H

#include <QObject>

#include <QByteArray>
#include <QFuture>
#include <QHash>
#include <QHostAddress>
#include <QHttpServer>
#include <QUrl>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QList>
#include <QMutex>
#include <QPromise>
#include <QTcpSocket>
#include <QSharedPointer>
#include <QSet>
#include <QSemaphore>
#include <QString>
#include <QTcpServer>
#include <QThreadPool>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <aowis/map/terrain_elevation.h>
#include <aowis/map/terrain_provider.h>
#include <aowis/map/terrain_data.h>
#include "terrain_admission.h"
#include "terrain_byte_admission.h"

#include <aowis/map/maptiles.h>
#include <aowis/map/map_cascade_config.h>
#include <aowis/map/raster_transfer.h>
#include <aowis/map/map_provider.h>
#include <aowis/map/provider_catalog.h>
#include <aowis/map/download_coverage.h>

namespace Aowis::Map
{
class TerrainData;
}

class TerrainSingleFlightTestAccess;

class Server : public QObject
{
    Q_OBJECT
    friend class TerrainSingleFlightTestAccess;
    friend class TerrainZstdHttpTestAccess;
    friend class RasterJpegHttpTestAccess;
    friend class MapCascadeChainTestAccess;
    friend class MapCascadeMeshTestAccess;
    friend class MapManagedSettingsHttpTestAccess;
    friend class ProviderDiscoveryTestAccess;

public:
    struct Config
    {
        QHostAddress listen_address { QHostAddress::LocalHost };
        quint16 port = 8122;
        int maximum_pending_requests = 2048;
        int maximum_terrain_jobs = 128;
        int maximum_terrain_workers = 4;
        int maximum_terrain_remote_workers = 2;
        int terrain_connect_timeout_seconds = 30;
        int terrain_inactivity_timeout_seconds = 120;
        int terrain_download_deadline_seconds = 1800;
        // In-process test injection only; not accepted from map-server.ini.
        QUrl terrain_integration_source_url;
        int maximum_active_downloads = 32;
        bool map_remote_fetch_enabled = true;
        Aowis::Map::MapCascadeConfig cascade;
        QString provider_state_file; // Legacy import path only; never written by the REST server.
        QString managed_settings_root; // MAP-owned AOWIS-CONFIG owner root, set by main.
        QString provider_revision; // Optimistic concurrency token for provider PUT.
        QSet<QString> disabled_inherited_providers;
        QString inherited_provider_revision;
        QHash<QString, QStringList> provider_upstream_routes;
        QString provider_upstream_routes_revision;
        QHash<QString, Aowis::Map::MapProviderAcquisitionMode> provider_acquisition_modes;
        QString provider_acquisition_modes_revision;
        bool cascade_defined_in_admin_ini = false;
        QString cache_directory;
        QString permanent_data_directory;
        QString map_coverage;
        Aowis::Map::DownloadCoverage download_coverage;
        QVector<Aowis::Map::MapProvider> map_providers = Aowis::Map::defaultMapProviders();
        bool terrain_enabled = true;
        bool terrain_remote_fetch_enabled = true;
        QString terrain_default_dataset = QStringLiteral("copernicus-glo30");
        QString terrain_cache_directory;
        QString terrain_coverage;
        bool copernicus_enabled = true;
        QByteArray api_key;
        QByteArray delete_api_key;
        bool require_api_key = false;
        bool require_delete_api_key = false;
    };

    explicit Server(const Config &config, QObject *parent = nullptr);
    ~Server() override;

    bool start();

private:
    using PendingPromise = QSharedPointer<QPromise<QHttpServerResponse>>;
    using PendingPromises = QList<PendingPromise>;
    using TerrainCancellation = std::shared_ptr<std::atomic_bool>;

    void setupRoutes();
    QFuture<QHttpServerResponse> submitTerrainJob(std::function<QHttpServerResponse()> task);
    QFuture<QHttpServerResponse> submitTerrainTile(const QString &dataset,
                                                   int zoom, int tile_x, int tile_y,
                                                   const QHttpServerRequest *client_request = nullptr);
    QFuture<QHttpServerResponse> submitTerrainElevation(
        const QString &dataset, double latitude_deg, double longitude_deg,
        std::optional<Aowis::Map::TerrainVerticalDatum> requested_vertical_datum,
        const QHttpServerRequest *client_request = nullptr);
    QFuture<QHttpServerResponse> submitTerrainGrid(
        const Aowis::Map::TerrainAnalysisGridRequest &request,
        const QHttpServerRequest *client_request = nullptr);
    bool isReadAuthorized(const QHttpServerRequest &request) const;
    bool isDeleteAuthorized(const QHttpServerRequest &request) const;
    bool requestContainsKey(const QHttpServerRequest &request, const QByteArray &expected_key) const;
    static QString terrainClientKey(const QHostAddress &address, quint16 port);
    void trackTerrainSocket(QTcpSocket *socket);
    void abandonTerrainWaiters(QTcpSocket *socket);
    void attachTerrainWaiter(const PendingPromise &promise,
                             const QHttpServerRequest *request);
    void forgetTerrainWaiter(const PendingPromise &promise);
    bool appendPendingPromise(const QString &key, const PendingPromise &promise);
    PendingPromises takePendingPromises(const QString &key);
    void completeRasterPromises(const PendingPromises &promises, const QByteArray &data,
                                const QByteArray &source, int zoom, int requested_quality);
    // Marks a tile/catalog response as answered by this MAP server (see
    // aowis/map/cascade_upstream_health.h); busy adds the admission marker.
    QHttpServerResponse cascadeResponse(QHttpServerResponse response, bool busy = false) const;
    // 202 for a cache-first terrain request whose acquisition is running.
    QHttpServerResponse terrainAcceptedResponse() const;

    void applyAvailableProviders();

    Config config;
    Aowis::Map::ProviderCatalog *provider_catalog = nullptr;
    QHttpServer http;
    QTcpServer *tcp;
    QThreadPool terrain_worker_pool;
    // Slow source acquisition is isolated from cached terrain processing.
    QThreadPool terrain_remote_pool;
    QThreadPool raster_encode_pool;
    QSemaphore raster_encode_admission{2};
    Aowis::Map::RasterTransferOptimizer raster_transfer;
    TerrainAdmission terrain_admission;
    // Bounds queued and running remote-capable jobs independently of all terrain jobs.
    TerrainAdmission terrain_remote_admission;
    TerrainByteAdmission terrain_grid_bytes;
    MapTiles *maptiles;
    Aowis::Map::TerrainData *terrain_data;
    // Copy of maptiles->cascadeInstanceId(): immutable, read from any thread.
    QByteArray cascade_instance_id;

    // Socket identity is used only on the HTTP thread. Empty single-flight
    // entries are retained until their running job finishes, allowing a new
    // client to join without creating a second job for the same key.
    QHash<QString, QTcpSocket *> terrain_client_sockets;
    QHash<QPromise<QHttpServerResponse> *, QTcpSocket *> terrain_waiter_sockets;
    // Individual HTTP representations must not alter single-flight terrain work.
    QSet<QPromise<QHttpServerResponse> *> terrain_zstd_waiters;
    QHash<QString, PendingPromises> pending_terrain_tiles;
    QHash<QString, PendingPromises> pending_terrain_elevations;
    QHash<QString, PendingPromises> pending_terrain_grids;
    // Identity is the cancellation token: stale callbacks must not drain a
    // replacement job started after all clients left the previous flight.
    QHash<QString, TerrainCancellation> tile_cancellations;
    QHash<QString, TerrainCancellation> elevation_cancellations;
    QHash<QString, TerrainCancellation> grid_cancellations;
    // Terrain tile jobs started by a cache-first (MAP-to-MAP) request: they
    // run without any waiting client and are not canceled when clients
    // disconnect; the downstream polls until the tile is cached.
    QSet<QString> detached_terrain_tiles;
    // Outcome of a failed detached job, handed to the next cache-first poll
    // for that tile (then the next poll starts a new attempt). Without it a
    // polling downstream would only ever see 202 while every attempt fails.
    struct DetachedTerrainFailure
    {
        qint64 failed_at_ms = 0;
        Aowis::Map::TerrainTileLookupResult result;
    };
    QHash<QString, DetachedTerrainFailure> detached_terrain_failures;
    void recordDetachedTerrainFailure(const QString &key,
                                      const Aowis::Map::TerrainTileLookupResult &result);
    // Upstreams each running terrain tile job may ask (loop-safe joins).
    QHash<QString, QStringList> terrain_job_upstreams;
    int pending_terrain_tile_count = 0;
    QMutex mutex_pending;
    QHash<QString, PendingPromises> connections_pending;
    // 0 means an ordinary client; otherwise the requested JPEG quality.
    QHash<QPromise<QHttpServerResponse> *, int> raster_transfer_waiters;
    int pending_request_count;
    bool shutting_down = false;

private slots:
    void onTileReady(QString key, QByteArray data);
    void onTileFailed(const QString &key, MapTiles::TileFailureReason reason);
};

#endif // SERVER_H
