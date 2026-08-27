#include "server.h"

#include <aowis/map/terrain_data.h>

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QUrlQuery>
#include <QtConcurrentRun>

#include <optional>
#include <utility>

namespace
{
QFuture<QHttpServerResponse> makeReadyResponse(QHttpServerResponse response)
{
    QPromise<QHttpServerResponse> promise;
    promise.start();
    QFuture<QHttpServerResponse> future = promise.future();
    promise.addResult(std::move(response));
    promise.finish();
    return future;
}

QHttpServerResponse makeOptionsResponse()
{
    return QHttpServerResponse(QHttpServerResponse::StatusCode::NoContent);
}

QHttpServerResponse makeUnauthorizedResponse()
{
    return QHttpServerResponse(
        "Missing or invalid API key",
        QHttpServerResponse::StatusCode::Unauthorized);
}

QHttpServerResponse makeTerrainTileErrorResponse(
    const Aowis::Map::TerrainTileLookupResult &result)
{
    QHttpServerResponse::StatusCode status = QHttpServerResponse::StatusCode::InternalServerError;
    switch (result.status)
    {
        case Aowis::Map::TerrainTileLookupStatus::Disabled:
        case Aowis::Map::TerrainTileLookupStatus::NotInitialized:
            status = QHttpServerResponse::StatusCode::ServiceUnavailable;
            break;
        case Aowis::Map::TerrainTileLookupStatus::InvalidDataset:
        case Aowis::Map::TerrainTileLookupStatus::InvalidAddress:
            status = QHttpServerResponse::StatusCode::BadRequest;
            break;
        case Aowis::Map::TerrainTileLookupStatus::TileUnavailable:
            status = QHttpServerResponse::StatusCode::NotFound;
            break;
        case Aowis::Map::TerrainTileLookupStatus::RemoteFetchError:
            status = QHttpServerResponse::StatusCode::BadGateway;
            break;
        case Aowis::Map::TerrainTileLookupStatus::TileReadError:
        case Aowis::Map::TerrainTileLookupStatus::CorruptTile:
        case Aowis::Map::TerrainTileLookupStatus::ProviderError:
        case Aowis::Map::TerrainTileLookupStatus::Ready:
        default:
            status = QHttpServerResponse::StatusCode::InternalServerError;
            break;
    }

    const QString message = result.error_message.isEmpty()
        ? QStringLiteral("Terrain tile request failed: %1")
              .arg(Aowis::Map::terrainTileLookupStatusId(result.status))
        : result.error_message;
    return QHttpServerResponse(message, status);
}

QHttpServerResponse makeTerrainElevationJsonResponse(
    const QJsonObject &object,
    QHttpServerResponse::StatusCode status = QHttpServerResponse::StatusCode::Ok)
{
    return QHttpServerResponse(
        QByteArrayLiteral("application/json"),
        QJsonDocument(object).toJson(QJsonDocument::Compact),
        status);
}

QHttpServerResponse makeTerrainElevationErrorResponse(
    const Aowis::Map::TerrainElevationLookupResult &result)
{
    QHttpServerResponse::StatusCode status = QHttpServerResponse::StatusCode::InternalServerError;
    switch (result.status)
    {
        case Aowis::Map::TerrainElevationLookupStatus::Disabled:
        case Aowis::Map::TerrainElevationLookupStatus::NotInitialized:
            status = QHttpServerResponse::StatusCode::ServiceUnavailable;
            break;
        case Aowis::Map::TerrainElevationLookupStatus::InvalidCoordinate:
        case Aowis::Map::TerrainElevationLookupStatus::InvalidDataset:
        case Aowis::Map::TerrainElevationLookupStatus::InvalidVerticalDatum:
        case Aowis::Map::TerrainElevationLookupStatus::VerticalDatumConversionUnavailable:
            status = QHttpServerResponse::StatusCode::BadRequest;
            break;
        case Aowis::Map::TerrainElevationLookupStatus::OutsideCoverage:
        case Aowis::Map::TerrainElevationLookupStatus::TileUnavailable:
        case Aowis::Map::TerrainElevationLookupStatus::NoData:
            status = QHttpServerResponse::StatusCode::NotFound;
            break;
        case Aowis::Map::TerrainElevationLookupStatus::RemoteFetchError:
            status = QHttpServerResponse::StatusCode::BadGateway;
            break;
        case Aowis::Map::TerrainElevationLookupStatus::TileReadError:
        case Aowis::Map::TerrainElevationLookupStatus::CorruptTile:
        case Aowis::Map::TerrainElevationLookupStatus::ProviderError:
        case Aowis::Map::TerrainElevationLookupStatus::Ready:
        default:
            status = QHttpServerResponse::StatusCode::InternalServerError;
            break;
    }

    QJsonObject object;
    object.insert(QStringLiteral("status"),
                  Aowis::Map::terrainElevationLookupStatusId(result.status));
    if (!result.error_message.isEmpty())
        object.insert(QStringLiteral("error"), result.error_message);
    if (result.requested_vertical_datum.has_value())
    {
        object.insert(QStringLiteral("requested_vertical_datum"),
                      Aowis::Map::terrainVerticalDatumId(
                          result.requested_vertical_datum.value()));
    }
    if (result.source_vertical_datum.has_value())
    {
        object.insert(QStringLiteral("source_vertical_datum"),
                      Aowis::Map::terrainVerticalDatumId(
                          result.source_vertical_datum.value()));
    }
    return makeTerrainElevationJsonResponse(object, status);
}

QHttpServerResponse makeTerrainElevationSuccessResponse(
    const Aowis::Map::TerrainElevationLookupResult &result)
{
    if (result.status != Aowis::Map::TerrainElevationLookupStatus::Ready ||
        !result.sample.has_value())
    {
        return makeTerrainElevationErrorResponse(result);
    }

    const Aowis::Map::TerrainElevationSample &sample = result.sample.value();
    QJsonObject object;
    object.insert(QStringLiteral("status"), QStringLiteral("ready"));
    object.insert(QStringLiteral("elevation_m"), sample.elevation_m);
    object.insert(QStringLiteral("dataset"), sample.dataset);
    object.insert(QStringLiteral("nominal_resolution_m"), sample.nominal_resolution_m);
    object.insert(QStringLiteral("vertical_datum"),
                  Aowis::Map::terrainVerticalDatumId(sample.vertical_datum));
    object.insert(QStringLiteral("vertical_datum_name"),
                  Aowis::Map::terrainVerticalDatumDisplayName(sample.vertical_datum));
    const QString vertical_datum_authority =
        Aowis::Map::terrainVerticalDatumAuthorityCode(sample.vertical_datum);
    if (!vertical_datum_authority.isEmpty())
    {
        object.insert(QStringLiteral("vertical_datum_authority"), vertical_datum_authority);
    }
    object.insert(QStringLiteral("vertical_reference"),
                  Aowis::Map::terrainVerticalReferenceId(
                      Aowis::Map::terrainVerticalReference(sample.vertical_datum)));
    object.insert(QStringLiteral("source_vertical_datum"),
                  Aowis::Map::terrainVerticalDatumId(sample.source_vertical_datum));
    object.insert(QStringLiteral("origin"), Aowis::Map::terrainDataOriginId(sample.origin));

    if (result.tile_address.has_value())
    {
        const Aowis::Map::TerrainTileAddress &address = result.tile_address.value();
        QJsonObject tile;
        tile.insert(QStringLiteral("zoom"), address.zoom);
        tile.insert(QStringLiteral("x"), double(address.x));
        tile.insert(QStringLiteral("y"), double(address.y));
        object.insert(QStringLiteral("tile"), tile);
    }

    return makeTerrainElevationJsonResponse(object);
}

bool secureEquals(const QByteArray &left, const QByteArray &right)
{
    const qsizetype maximum_size = qMax(left.size(), right.size());
    quint64 difference = quint64(left.size()) ^ quint64(right.size());
    for (qsizetype index = 0; index < maximum_size; ++index)
    {
        const uchar left_value = index < left.size() ? uchar(left.at(index)) : 0;
        const uchar right_value = index < right.size() ? uchar(right.at(index)) : 0;
        difference |= left_value ^ right_value;
    }

    return difference == 0;
}
}

Server::Server(const Config &config, QObject *parent)
    : QObject(parent),
      config(config),
      tcp(new QTcpServer(this)),
      maptiles(new MapTiles(config.cache_directory, config.maximum_active_downloads,
                            config.maximum_pending_requests, this)),
      terrain_data(new Aowis::Map::TerrainData(
          {
              config.terrain_enabled,
              config.terrain_remote_fetch_enabled,
              config.cache_directory,
              config.terrain_cache_directory
          },
          this)),
      pending_request_count(0)
{
    connect(this->maptiles, &MapTiles::tileReady, this, &Server::onTileReady);
    connect(this->maptiles, &MapTiles::tileFailed, this, &Server::onTileFailed);
    setupRoutes();
}

bool Server::start()
{
    if (this->config.require_api_key && this->config.api_key.isEmpty())
    {
        qCritical() << "The map server requires an API key, but authentication/api_key is empty";
        return false;
    }
    if (this->config.require_delete_api_key && this->config.delete_api_key.isEmpty())
    {
        qCritical() << "The map server requires a delete API key, but authentication/delete_api_key is empty";
        return false;
    }

    QDir cache_directory;
    if (this->config.cache_directory.trimmed().isEmpty()
        || !cache_directory.mkpath(this->config.cache_directory))
    {
        qCritical() << "Failed to create map tile cache directory:"
                    << this->config.cache_directory;
        return false;
    }

    const QFileInfo cache_info(this->config.cache_directory);
    if (!cache_info.isDir() || !cache_info.isWritable())
    {
        qCritical() << "Map tile cache directory is not writable:"
                    << this->config.cache_directory;
        return false;
    }

    QString terrain_error_message;
    if (!this->terrain_data->initialize(&terrain_error_message))
    {
        qCritical().noquote() << terrain_error_message;
        return false;
    }

    if (!this->tcp->listen(this->config.listen_address, this->config.port))
    {
        qCritical() << "Failed to listen on" << this->config.listen_address.toString()
                    << "port" << this->config.port << ':' << this->tcp->errorString();
        return false;
    }

    if (!this->http.bind(this->tcp))
    {
        qCritical() << "Failed to bind QHttpServer to QTcpServer";
        this->tcp->close();
        return false;
    }

    const QString read_authentication_status = this->config.api_key.isEmpty()
        ? QStringLiteral("read API-key authentication disabled")
        : QStringLiteral("read API-key authentication enabled");
    const QString delete_authentication_status = this->config.delete_api_key.isEmpty()
        ? QStringLiteral("delete API-key authentication disabled")
        : QStringLiteral("delete API-key authentication enabled");

    const Aowis::Map::TerrainData::StoragePaths terrain_paths = this->terrain_data->storagePaths();
    const QString terrain_status = !this->terrain_data->isEnabled()
        ? QStringLiteral("terrain subsystem disabled")
        : this->terrain_data->isRemoteFetchEnabled()
              ? QStringLiteral("terrain subsystem enabled with remote fetching")
              : QStringLiteral("terrain subsystem enabled in offline-only mode");

    qInfo() << "AOWIS map server listening on" << this->tcp->serverAddress().toString()
            << "port" << this->tcp->serverPort()
            << "with at most" << this->config.maximum_active_downloads << "active tile downloads and"
            << this->config.maximum_pending_requests << "pending HTTP tile requests"
            << "using cache directory" << this->config.cache_directory
            << terrain_status
            << (this->terrain_data->isEnabled()
                    ? QStringLiteral("terrain cache: %1, default dataset: %2")
                          .arg(terrain_paths.root, this->config.terrain_default_dataset)
                    : QString())
            << read_authentication_status << "and" << delete_authentication_status;
    return true;
}

void Server::setupRoutes()
{
    this->http.route("/status", QHttpServerRequest::Method::Get,
                     [this](const QHttpServerRequest &request)
    {
        if (!isReadAuthorized(request))
            return makeUnauthorizedResponse();

        const Aowis::Map::TerrainData::StoragePaths terrain_paths = this->terrain_data->storagePaths();
        const QString terrain_status = !this->terrain_data->isEnabled()
            ? QStringLiteral("disabled")
            : this->terrain_data->isRemoteFetchEnabled()
                  ? QStringLiteral("enabled, remote fetching enabled")
                  : QStringLiteral("enabled, offline-only");
        QString status = QStringLiteral("AOWIS map server running with Qt %1\nTerrain subsystem: %2")
                             .arg(QString::fromLatin1(QT_VERSION_STR), terrain_status);
        if (this->terrain_data->isEnabled())
        {
            status += QStringLiteral("\nTerrain cache: %1\nTerrain default dataset: %2")
                          .arg(terrain_paths.root, this->config.terrain_default_dataset);
        }

        return QHttpServerResponse(status, QHttpServerResponse::StatusCode::Ok);
    });

    this->http.route("/status", QHttpServerRequest::Method::Options, []()
    {
        return makeOptionsResponse();
    });

    this->http.route("/upstream/v1/activity", QHttpServerRequest::Method::Get,
                     [this](const QHttpServerRequest &request)
    {
        if (!isReadAuthorized(request))
            return makeUnauthorizedResponse();

        const MapTiles::UpstreamActivity tile_activity = this->maptiles->upstreamActivity();
        const Aowis::Map::TerrainUpstreamActivity terrain_activity =
            this->terrain_data->upstreamActivity();

        QJsonObject map_tiles;
        map_tiles.insert(QStringLiteral("active"), tile_activity.active);
        map_tiles.insert(QStringLiteral("queued"), tile_activity.queued);

        QJsonObject terrain;
        terrain.insert(QStringLiteral("active"), terrain_activity.active);
        terrain.insert(QStringLiteral("queued"), terrain_activity.queued);

        QJsonObject root;
        root.insert(QStringLiteral("map_tiles"), map_tiles);
        root.insert(QStringLiteral("terrain"), terrain);
        return QHttpServerResponse(
            QByteArrayLiteral("application/json"),
            QJsonDocument(root).toJson(QJsonDocument::Compact),
            QHttpServerResponse::StatusCode::Ok);
    });

    this->http.route("/upstream/v1/activity", QHttpServerRequest::Method::Options, []()
    {
        return makeOptionsResponse();
    });

    this->http.route("/upstream/v1/map-tiles", QHttpServerRequest::Method::Delete,
                     [this](const QHttpServerRequest &request)
    {
        if (!isDeleteAuthorized(request))
            return makeUnauthorizedResponse();

        this->maptiles->cancelUpstreamDownloads();
        return QHttpServerResponse(
            "Map tile upstream downloads canceled",
            QHttpServerResponse::StatusCode::Ok);
    });

    this->http.route("/upstream/v1/map-tiles", QHttpServerRequest::Method::Options, []()
    {
        return makeOptionsResponse();
    });

    this->http.route("/upstream/v1/terrain", QHttpServerRequest::Method::Delete,
                     [this](const QHttpServerRequest &request)
    {
        if (!isDeleteAuthorized(request))
            return makeUnauthorizedResponse();

        this->terrain_data->cancelUpstreamDownloads();
        return QHttpServerResponse(
            "Terrain upstream downloads canceled",
            QHttpServerResponse::StatusCode::Ok);
    });

    this->http.route("/upstream/v1/terrain", QHttpServerRequest::Method::Options, []()
    {
        return makeOptionsResponse();
    });

    this->http.route("/cache/<arg>/<arg>/<arg>/<arg>/<arg>/<arg>", QHttpServerRequest::Method::Delete,
                     [this](const QString &provider, int zoom, int tile_x_min, int tile_x_max,
                            int tile_y_min, int tile_y_max, const QHttpServerRequest &request)
    {
        if (!isDeleteAuthorized(request))
            return makeUnauthorizedResponse();

        const int deleted_count = this->maptiles->deleteTiles(
            provider, zoom, tile_x_min, tile_x_max, tile_y_min, tile_y_max);
        if (deleted_count == -1)
        {
            return QHttpServerResponse(
                "Invalid tile cache deletion request",
                QHttpServerResponse::StatusCode::BadRequest);
        }
        if (deleted_count < -1)
        {
            return QHttpServerResponse(
                "Failed to delete one or more cached tiles",
                QHttpServerResponse::StatusCode::InternalServerError);
        }

        return QHttpServerResponse(QString::number(deleted_count), QHttpServerResponse::StatusCode::Ok);
    });

    this->http.route("/cache/<arg>/<arg>/<arg>/<arg>/<arg>/<arg>", QHttpServerRequest::Method::Options,
                     [](const QString &, int, int, int, int, int)
    {
        return makeOptionsResponse();
    });

    this->http.route("/terrain/v1/elevation",
                     QHttpServerRequest::Method::Get,
                     [this](const QHttpServerRequest &request) -> QFuture<QHttpServerResponse>
    {
        if (!isReadAuthorized(request))
            return makeReadyResponse(makeUnauthorizedResponse());

        const QUrlQuery query(request.url());
        const QString latitude_text = query.queryItemValue(QStringLiteral("latitude"));
        const QString longitude_text = query.queryItemValue(QStringLiteral("longitude"));
        bool latitude_ok = false;
        bool longitude_ok = false;
        const double latitude_deg = latitude_text.toDouble(&latitude_ok);
        const double longitude_deg = longitude_text.toDouble(&longitude_ok);
        if (!latitude_ok || !longitude_ok || latitude_text.isEmpty() || longitude_text.isEmpty())
        {
            Aowis::Map::TerrainElevationLookupResult invalid_result;
            invalid_result.status = Aowis::Map::TerrainElevationLookupStatus::InvalidCoordinate;
            invalid_result.error_message = QStringLiteral(
                "Terrain elevation requests require numeric latitude and longitude query parameters");
            return makeReadyResponse(makeTerrainElevationErrorResponse(invalid_result));
        }

        std::optional<Aowis::Map::TerrainVerticalDatum> requested_vertical_datum;
        const QString vertical_datum_text =
            query.queryItemValue(QStringLiteral("vertical_datum")).trimmed().toLower();
        if (!vertical_datum_text.isEmpty() && vertical_datum_text != QStringLiteral("native"))
        {
            const std::optional<Aowis::Map::TerrainVerticalDatum> parsed_vertical_datum =
                Aowis::Map::terrainVerticalDatumFromId(vertical_datum_text);
            if (!parsed_vertical_datum.has_value() ||
                !Aowis::Map::isRequestableTerrainVerticalDatum(parsed_vertical_datum.value()))
            {
                Aowis::Map::TerrainElevationLookupResult invalid_result;
                invalid_result.status = Aowis::Map::TerrainElevationLookupStatus::InvalidVerticalDatum;
                invalid_result.error_message = QStringLiteral(
                    "vertical_datum must be native, wgs84-ellipsoid, egm96 or egm2008");
                return makeReadyResponse(makeTerrainElevationErrorResponse(invalid_result));
            }
            requested_vertical_datum = parsed_vertical_datum.value();
        }

        const QString dataset = this->config.terrain_default_dataset;
        return QtConcurrent::run(
            [this, dataset, latitude_deg, longitude_deg, requested_vertical_datum]()
        {
            const Aowis::Map::TerrainElevationLookupResult elevation_result =
                this->terrain_data->sampleElevation(
                    dataset, latitude_deg, longitude_deg, requested_vertical_datum);
            return makeTerrainElevationSuccessResponse(elevation_result);
        });
    });

    this->http.route("/terrain/v1/elevation",
                     QHttpServerRequest::Method::Options,
                     []()
    {
        return makeOptionsResponse();
    });

    this->http.route("/terrain/v1/<arg>/<arg>/<arg>/<arg>.aowterrain",
                     QHttpServerRequest::Method::Get,
                     [this](const QString &dataset, int zoom, int tile_x, int tile_y,
                            const QHttpServerRequest &request) -> QFuture<QHttpServerResponse>
    {
        if (!isReadAuthorized(request))
            return makeReadyResponse(makeUnauthorizedResponse());

        Aowis::Map::TerrainTileAddress address;
        address.zoom = zoom;
        if (tile_x >= 0)
            address.x = quint32(tile_x);
        if (tile_y >= 0)
            address.y = quint32(tile_y);

        if (tile_x < 0 || tile_y < 0)
        {
            Aowis::Map::TerrainTileLookupResult invalid_result;
            invalid_result.status = Aowis::Map::TerrainTileLookupStatus::InvalidAddress;
            invalid_result.error_message = QStringLiteral("Terrain tile X and Y must be non-negative");
            return makeReadyResponse(makeTerrainTileErrorResponse(invalid_result));
        }

        return QtConcurrent::run([this, dataset, address]()
        {
            const Aowis::Map::TerrainTileLookupResult terrain_result =
                this->terrain_data->terrainTile(dataset, address);
            if (terrain_result.status != Aowis::Map::TerrainTileLookupStatus::Ready)
                return makeTerrainTileErrorResponse(terrain_result);

            return QHttpServerResponse(
                Aowis::Map::terrainTileMimeType().toUtf8(),
                terrain_result.data);
        });
    });

    this->http.route("/terrain/v1/<arg>/<arg>/<arg>/<arg>.aowterrain",
                     QHttpServerRequest::Method::Options,
                     [](const QString &, int, int, int)
    {
        return makeOptionsResponse();
    });

    this->http.route("/<arg>/<arg>/<arg>/<arg>.png", QHttpServerRequest::Method::Get,
                     [this](const QString &provider, int z, int x, int y,
                            const QHttpServerRequest &request) -> QFuture<QHttpServerResponse>
    {
        if (!isReadAuthorized(request))
            return makeReadyResponse(makeUnauthorizedResponse());

        const QString key = QString("%1_%2_%3_%4").arg(provider).arg(z).arg(x).arg(y);
        const QUrlQuery tile_query(request.url());
        const bool cache_first_delivery =
            tile_query.queryItemValue(QStringLiteral("delivery")).compare(
                QStringLiteral("cache-first"), Qt::CaseInsensitive) == 0
            || request.value("x-aowis-tile-delivery").compare(
                QByteArrayLiteral("cache-first"), Qt::CaseInsensitive) == 0;
        const MapTiles::TileRequestResult tile_result = this->maptiles->getTile(provider, z, x, y, key);

        if (tile_result.status == MapTiles::TileRequestStatus::Ready)
            return makeReadyResponse(QHttpServerResponse("image/png", tile_result.data));

        if (tile_result.status == MapTiles::TileRequestStatus::InvalidRequest)
        {
            return makeReadyResponse(QHttpServerResponse(
                "Invalid map tile request",
                QHttpServerResponse::StatusCode::BadRequest));
        }

        if (tile_result.status == MapTiles::TileRequestStatus::ServerBusy)
        {
            return makeReadyResponse(QHttpServerResponse(
                "Map tile download queue is full",
                QHttpServerResponse::StatusCode::ServiceUnavailable));
        }

        if (cache_first_delivery)
        {
            return makeReadyResponse(QHttpServerResponse(
                "Map tile is being fetched",
                QHttpServerResponse::StatusCode::Accepted));
        }

        PendingPromise promise = PendingPromise::create();
        promise->start();
        QFuture<QHttpServerResponse> future = promise->future();
        if (!appendPendingPromise(key, promise))
        {
            promise->addResult(QHttpServerResponse(
                "Too many pending map tile requests",
                QHttpServerResponse::StatusCode::ServiceUnavailable));
            promise->finish();
        }

        return future;
    });

    this->http.route("/<arg>/<arg>/<arg>/<arg>.png", QHttpServerRequest::Method::Options,
                     [](const QString &, int, int, int)
    {
        return makeOptionsResponse();
    });
}

bool Server::isReadAuthorized(const QHttpServerRequest &request) const
{
    if (this->config.api_key.isEmpty())
        return true;

    return requestContainsKey(request, this->config.api_key);
}

bool Server::isDeleteAuthorized(const QHttpServerRequest &request) const
{
    if (this->config.delete_api_key.isEmpty())
        return true;

    return requestContainsKey(request, this->config.delete_api_key);
}

bool Server::requestContainsKey(const QHttpServerRequest &request, const QByteArray &expected_key) const
{
    const QByteArray direct_key = request.value("x-api-key");
    if (secureEquals(direct_key, expected_key))
        return true;

    const QByteArray authorization = request.value("authorization");
    static const QByteArray bearer_prefix("Bearer ");
    if (authorization.size() <= bearer_prefix.size()
        || authorization.first(bearer_prefix.size()).compare(bearer_prefix, Qt::CaseInsensitive) != 0)
    {
        return false;
    }

    return secureEquals(authorization.sliced(bearer_prefix.size()), expected_key);
}

bool Server::appendPendingPromise(const QString &key, const PendingPromise &promise)
{
    QMutexLocker locker(&this->mutex_pending);
    if (this->pending_request_count >= this->config.maximum_pending_requests)
        return false;

    this->connections_pending[key].append(promise);
    ++this->pending_request_count;
    return true;
}

Server::PendingPromises Server::takePendingPromises(const QString &key)
{
    QMutexLocker locker(&this->mutex_pending);
    PendingPromises promises = this->connections_pending.take(key);
    this->pending_request_count -= promises.size();
    return promises;
}

void Server::onTileReady(QString key, QByteArray data)
{
    PendingPromises promises = takePendingPromises(key);
    for (const PendingPromise &promise : promises)
    {
        promise->addResult(QHttpServerResponse("image/png", data));
        promise->finish();
    }
}

void Server::onTileFailed(const QString &key, MapTiles::TileFailureReason reason)
{
    PendingPromises promises = takePendingPromises(key);
    QHttpServerResponse::StatusCode status = QHttpServerResponse::StatusCode::BadGateway;
    QString message = QStringLiteral("Failed to download map tile");
    if (reason == MapTiles::TileFailureReason::Timeout)
    {
        status = QHttpServerResponse::StatusCode::GatewayTimeout;
        message = QStringLiteral("Map tile download timed out");
    }
    else if (reason == MapTiles::TileFailureReason::Cancelled)
    {
        status = QHttpServerResponse::StatusCode::ServiceUnavailable;
        message = QStringLiteral("Map tile upstream download canceled");
    }

    for (const PendingPromise &promise : promises)
    {
        promise->addResult(QHttpServerResponse(message, status));
        promise->finish();
    }
}
