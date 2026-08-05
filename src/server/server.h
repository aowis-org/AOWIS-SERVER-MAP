#ifndef SERVER_H
#define SERVER_H

#include <QObject>

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
#include <QTcpServer>

#include <aowis/map/maptiles.h>

class Server : public QObject
{
    Q_OBJECT

public:
    struct Config
    {
        QHostAddress listen_address { QHostAddress::LocalHost };
        quint16 port = 8122;
        int maximum_pending_requests = 2048;
        int maximum_active_downloads = 32;
    };

    explicit Server(const Config &config, QObject *parent = nullptr);

    bool start();

private:
    using PendingPromise = QSharedPointer<QPromise<QHttpServerResponse>>;
    using PendingPromises = QList<PendingPromise>;

    void setupRoutes();
    bool appendPendingPromise(const QString &key, const PendingPromise &promise);
    PendingPromises takePendingPromises(const QString &key);

    Config config;
    QHttpServer http;
    QTcpServer *tcp;
    MapTiles *maptiles;

    QMutex mutex_pending;
    QHash<QString, PendingPromises> connections_pending;
    int pending_request_count;

private slots:
    void onTileReady(QString key, QByteArray data);
    void onTileFailed(const QString &key, MapTiles::TileFailureReason reason);
};

#endif // SERVER_H
