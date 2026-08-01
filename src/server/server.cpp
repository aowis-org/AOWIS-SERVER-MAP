#include "server.h"

#include <QDebug>

Server::Server(QCoreApplication *app, QObject *parent)
    : QObject{parent}
{
    this->app = app;
    
    setupRoutes();
}

/* deprecated: use reverse proxy instead doing this */
QHttpHeaders makeCorsHeaders() {
    QHttpHeaders h;
    h.append("Access-Control-Allow-Origin", "*");
    h.append("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    h.append("Access-Control-Allow-Headers", "Content-Type");
    return h;
}

void Server::setupRoutes()
{
    qDebug() << "Starting Server";
    
    this->maptiles = new MapTiles(this);
    connect(this->maptiles, &MapTiles::tileReady, this, &Server::onTileReady);
    
    // GET /
    this->http.route("/status", QHttpServerRequest::Method::Get,
                    [](const QHttpServerRequest &)
                    {
                        QHttpServerResponse resp("Hello from Qt 6.11 HttpServer!",
                                            QHttpServerResponse::StatusCode::Ok);
                        //resp.setHeaders(makeCorsHeaders());
                        return resp;
                    });
    // OPTIONS /
    this->http.route("/status", QHttpServerRequest::Method::Options,
                    [](const QHttpServerRequest &)
                    {
                        QHttpServerResponse resp(QHttpServerResponse::StatusCode::Ok);
                        //resp.setHeaders(makeCorsHeaders());
                        return resp;
                    });
    // DELETE /cache/provider/zoom/x-min/x-max/y-min/y-max
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

        return QHttpServerResponse(
            QString::number(deleted_count),
            QHttpServerResponse::StatusCode::Ok);
    });

    this->http.route("/cache/<arg>/<arg>/<arg>/<arg>/<arg>/<arg>", QHttpServerRequest::Method::Options,
                     [](const QString &, int, int, int, int, int)
    {
        return QHttpServerResponse(QHttpServerResponse::StatusCode::Ok);
    });

    // GET /maptiles
    this->http.route("/<arg>/<arg>/<arg>/<arg>.png",
                    [this](const QString &provider, int z, int x, int y, const QHttpServerRequest &request)
                     -> QFuture<QHttpServerResponse>
                    {
                        // We should log this to the database with timestamp!
                        //qDebug() << request.headers();
                        
                        QString key = QString("%1_%2_%3_%4").arg(provider).arg(z).arg(x).arg(y);
                        
                        QByteArray tile_data = this->maptiles->getTile(provider, z, x, y, key);
                        
                        if (!tile_data.isEmpty())
                        {
                            // Tile exists → return it normally
                            
                            QPromise<QHttpServerResponse> promise;
                            auto future = promise.future();
                            
                            QHttpServerResponse resp("image/png", tile_data);
                            promise.addResult(std::move(resp));
                            promise.finish();
                            
                            return future;
                        }
                        
                        // Tile is not cached yet → assync response
                        
                        auto *promise = new QPromise<QHttpServerResponse>();
                        auto future = promise->future();
                        
                        {
                            QMutexLocker locker(&this->mutex_pending);
                            this->connections_pending[key].append(promise);
                        }
                        
                        return future;
                    });
    // OPTIONS /maptiles
    this->http.route("/", QHttpServerRequest::Method::Options,
                    [](const QHttpServerRequest &)
                    {
                        return QHttpServerResponse(QHttpServerResponse::StatusCode::Ok);
                    });
    // Catchall / Fallback (mostly for testing and debug)
    this->http.route("/<arg>", [](const QString &path)
                    {
                        qDebug() << "Fallback route hit. Path =" << path;
                        return QHttpServerResponse("fallback");
                    });
    
    this->tcp = new QTcpServer(this->app);
    
    bool is_running = true;
    if (!this->tcp->listen(QHostAddress::Any, 8122)) {
        qWarning() << "Failed to listen on port 8122";
        
        is_running = false;
    }
    if (!this->http.bind(tcp)) {
        qWarning() << "Failed to bind QHttpServer to QTcpServer";
        
        is_running = false;
    }
    if (is_running)
    {
        qDebug() << "Server Listening";
    }
}

void Server::onTileReady(QString key, QByteArray data)
{
    QMutexLocker locker(&this->mutex_pending);
    
    auto it = this->connections_pending.find(key);
    if (it == this->connections_pending.end())
        return;
    
    PendingList &list = it.value();
    
    for (int i = 0; i < list.count; ++i)
    {
        auto *promise = list.items[i];
        QHttpServerResponse resp("image/png", data);
        promise->addResult(std::move(resp));
        promise->finish();
        delete promise;
    }
    
    this->connections_pending.erase(it);
}
