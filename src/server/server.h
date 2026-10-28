#ifndef SERVER_H
#define SERVER_H

#include <QObject>

#include <QByteArray>
#include <QFuture>
#include <QHash>
#include <QHostAddress>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QList>
#include <QMutex>
#include <QPromise>
#include <QSharedPointer>
#include <QString>
#include <QTcpServer>
#include <QThreadPool>
#include <functional>
#include <optional>
#include <aowis/map/terrain_elevation.h>
#include "terrain_admission.h"

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

    void setupRoutes();
    QFuture<QHttpServerResponse> submitTerrainJob(std::function<QHttpServerResponse()> task);
    QFuture<QHttpServerResponse> submitTerrainTile(const QString &dataset,
                                                   int zoom, int tile_x, int tile_y);
    QFuture<QHttpServerResponse> submitTerrainElevation(
        const QString &dataset, double latitude_deg, double longitude_deg,
        std::optional<Aowis::Map::TerrainVerticalDatum> requested_vertical_datum);
    bool isReadAuthorized(const QHttpServerRequest &request) const;
    bool isDeleteAuthorized(const QHttpServerRequest &request) const;
    bool requestContainsKey(const QHttpServerRequest &request, const QByteArray &expected_key) const;
    bool appendPendingPromise(const QString &key, const PendingPromise &promise);
    PendingPromises takePendingPromises(const QString &key);

    Config config;
    QHttpServer http;
    QTcpServer *tcp;
    QThreadPool terrain_worker_pool;
    TerrainAdmission terrain_admission;
    MapTiles *maptiles;
    Aowis::Map::TerrainData *terrain_data;

    QHash<QString, PendingPromises> pending_terrain_tiles;
    QHash<QString, PendingPromises> pending_terrain_elevations;
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
