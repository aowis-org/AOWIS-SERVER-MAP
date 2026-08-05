#include "server.h"

#include <QDebug>
#include <QMutexLocker>

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
}

Server::Server(const Config &config, QObject *parent)
    : QObject(parent),
      config(config),
      tcp(new QTcpServer(this)),
      maptiles(new MapTiles(config.maximum_active_downloads, config.maximum_pending_requests, this)),
      pending_request_count(0)
{
    connect(this->maptiles, &MapTiles::tileReady, this, &Server::onTileReady);
    connect(this->maptiles, &MapTiles::tileFailed, this, &Server::onTileFailed);
    setupRoutes();
}

bool Server::start()
{
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

    qInfo() << "AOWIS map server listening on" << this->tcp->serverAddress().toString()
            << "port" << this->tcp->serverPort()
            << "with at most" << this->config.maximum_active_downloads << "active tile downloads and"
            << this->config.maximum_pending_requests << "pending HTTP tile requests";
    return true;
}

void Server::setupRoutes()
{
    this->http.route("/status", QHttpServerRequest::Method::Get, []()
    {
        return QHttpServerResponse(
            QStringLiteral("AOWIS map server running with Qt %1").arg(QString::fromLatin1(QT_VERSION_STR)),
            QHttpServerResponse::StatusCode::Ok);
    });

    this->http.route("/status", QHttpServerRequest::Method::Options, []()
    {
        return makeOptionsResponse();
    });

    this->http.route("/cache/<arg>/<arg>/<arg>/<arg>/<arg>/<arg>", QHttpServerRequest::Method::Delete,
                     [this](const QString &provider, int zoom, int tile_x_min, int tile_x_max,
                            int tile_y_min, int tile_y_max)
    {
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

    this->http.route("/<arg>/<arg>/<arg>/<arg>.png", QHttpServerRequest::Method::Get,
                     [this](const QString &provider, int z, int x, int y) -> QFuture<QHttpServerResponse>
    {
        const QString key = QString("%1_%2_%3_%4").arg(provider).arg(z).arg(x).arg(y);
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
    const QHttpServerResponse::StatusCode status = reason == MapTiles::TileFailureReason::Timeout
        ? QHttpServerResponse::StatusCode::GatewayTimeout
        : QHttpServerResponse::StatusCode::BadGateway;
    const QString message = reason == MapTiles::TileFailureReason::Timeout
        ? QStringLiteral("Map tile download timed out")
        : QStringLiteral("Failed to download map tile");

    for (const PendingPromise &promise : promises)
    {
        promise->addResult(QHttpServerResponse(message, status));
        promise->finish();
    }
}
