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
#include <QString>
#include <QTcpServer>
#include <QThreadPool>
#include <atomic>
#include <functional>
#include <optional>
#include <aowis/map/terrain_elevation.h>
#include <aowis/map/terrain_provider.h>
#include "terrain_admission.h"
#include "terrain_byte_admission.h"

#include <aowis/map/maptiles.h>
#include <aowis/map/map_provider.h>
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
        QString provider_state_file;
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
    using TerrainCancellation = QSharedPointer<std::atomic_bool>;

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

    Config config;
    QHttpServer http;
    QTcpServer *tcp;
    QThreadPool terrain_worker_pool;
    // Slow source acquisition is isolated from cached terrain processing.
    QThreadPool terrain_remote_pool;
    TerrainAdmission terrain_admission;
    // Bounds queued and running remote-capable jobs independently of all terrain jobs.
    TerrainAdmission terrain_remote_admission;
    TerrainByteAdmission terrain_grid_bytes;
    MapTiles *maptiles;
    Aowis::Map::TerrainData *terrain_data;

    // Socket identity is used only on the HTTP thread. Empty single-flight
    // entries are retained until their running job finishes, allowing a new
    // client to join without creating a second job for the same key.
    QHash<QString, QTcpSocket *> terrain_client_sockets;
    QHash<QPromise<QHttpServerResponse> *, QTcpSocket *> terrain_waiter_sockets;
    QHash<QString, PendingPromises> pending_terrain_tiles;
    QHash<QString, PendingPromises> pending_terrain_elevations;
    QHash<QString, PendingPromises> pending_terrain_grids;
    // Identity is the cancellation token: stale callbacks must not drain a
    // replacement job started after all clients left the previous flight.
    QHash<QString, TerrainCancellation> tile_cancellations;
    QHash<QString, TerrainCancellation> elevation_cancellations;
    QHash<QString, TerrainCancellation> grid_cancellations;
    int pending_terrain_tile_count = 0;
    QMutex mutex_pending;
    QHash<QString, PendingPromises> connections_pending;
    int pending_request_count;
    bool shutting_down = false;

private slots:
    void onTileReady(QString key, QByteArray data);
    void onTileFailed(const QString &key, MapTiles::TileFailureReason reason);
};

#endif // SERVER_H
