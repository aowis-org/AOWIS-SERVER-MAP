#include <aowis/map/maptiles.h>
#include <aowis/map/map_provider.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

#include <cstdio>

namespace
{
bool require(bool condition, const char *message)
{
    if (!condition) std::fprintf(stderr, "%s\n", message);
    return condition;
}

QByteArray validPng()
{
    return QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+j"
        "mZkAAAAASUVORK5CYII=");
}

class StubTileServer final : public QTcpServer
{
public:
    QByteArray data = validPng();
    QByteArray source;
    QByteArray required_key;
    int status = 200;
    QByteArray cache_control;
    QList<QByteArray> requests;

    bool start()
    {
        return listen(QHostAddress::LocalHost, 0);
    }

    QUrl origin() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(serverPort()));
    }

protected:
    void incomingConnection(qintptr socket_descriptor) override
    {
        QTcpSocket *socket = new QTcpSocket(this);
        if (!socket->setSocketDescriptor(socket_descriptor))
        {
            socket->deleteLater();
            return;
        }
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        QObject::connect(socket, &QTcpSocket::readyRead, socket,
            [this, socket, received = QByteArray()]() mutable
        {
            received.append(socket->readAll());
            if (!received.contains("\r\n\r\n")) return;
            const QByteArray request = received.left(received.indexOf("\r\n\r\n") + 4);
            this->requests.append(request);
            const QByteArray lowercase = request.toLower();
            const bool authenticated = this->required_key.isEmpty() ||
                lowercase.contains("x-api-key: " + this->required_key.toLower() + "\r\n");
            const int response_status = authenticated ? this->status : 401;
            const QByteArray body = response_status == 200 ? this->data : QByteArrayLiteral("failure");
            QByteArray response = "HTTP/1.1 " + QByteArray::number(response_status) +
                (response_status == 200 ? " OK\r\n" : " Error\r\n");
            response += "Content-Type: image/png\r\nContent-Length: " +
                QByteArray::number(body.size()) + "\r\n";
            if (!this->source.isEmpty())
                response += "X-AOWIS-Raster-Source: " + this->source + "\r\n";
            if (!this->cache_control.isEmpty())
                response += "Cache-Control: " + this->cache_control + "\r\n";
            response += "Connection: close\r\n\r\n";
            socket->write(response + body);
            socket->disconnectFromHost();
        });
    }
};

struct Result
{
    QByteArray data;
    bool failed = false;
    MapTiles::TileFailureReason failure = MapTiles::TileFailureReason::UpstreamError;
};

Result fetch(MapTiles *tiles, const QString &provider, int z, int x, int y,
             const QString &request_key)
{
    Result result;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    const QMetaObject::Connection ready = QObject::connect(
        tiles, &MapTiles::tileReady, &loop,
        [&result, &loop, request_key](const QString &key, const QByteArray &data)
    {
        if (key != request_key) return;
        result.data = data;
        loop.quit();
    });
    const QMetaObject::Connection failed = QObject::connect(
        tiles, &MapTiles::tileFailed, &loop,
        [&result, &loop, request_key](const QString &key, MapTiles::TileFailureReason reason)
    {
        if (key != request_key) return;
        result.failed = true;
        result.failure = reason;
        loop.quit();
    });
    const MapTiles::TileRequestResult initial = tiles->getTile(
        provider, z, x, y, request_key);
    if (initial.status == MapTiles::TileRequestStatus::Ready)
        result.data = initial.data;
    else if (initial.status == MapTiles::TileRequestStatus::Pending)
    {
        timeout.start(6000);
        loop.exec();
        if (timeout.isActive()) timeout.stop();
        else if (!result.failed && result.data.isEmpty()) result.failed = true;
    }
    else result.failed = true;
    QObject::disconnect(ready);
    QObject::disconnect(failed);
    return result;
}

Aowis::Map::MapCascadeUpstream upstream(const char *name, const StubTileServer &server,
                                        const QByteArray &key = {})
{
    Aowis::Map::MapCascadeUpstream item;
    item.name = QString::fromLatin1(name);
    item.url = server.origin();
    item.api_key = key;
    return item;
}
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QTemporaryDir directory;
    if (!require(directory.isValid(), "Could not create test cache directory")) return 1;

    StubTileServer failed_primary;
    StubTileServer valid_secondary;
    StubTileServer origin;
    if (!require(failed_primary.start() && valid_secondary.start() && origin.start(),
                 "Could not listen on loopback for raster cascade test")) return 1;
    failed_primary.status = 503;
    valid_secondary.required_key = QByteArrayLiteral("read-key");

    QVector<Aowis::Map::MapProvider> providers = Aowis::Map::defaultMapProviders();
    const QString provider_id = providers[0].id;
    providers[0].url_template = origin.origin().toString() + QStringLiteral("/{z}/{x}/{y}.png");
    const QByteArray fingerprint = QCryptographicHash::hash(
        providers[0].url_template.toUtf8(), QCryptographicHash::Sha256).toHex();
    valid_secondary.source = fingerprint;

    Aowis::Map::MapCascadeConfig config;
    config.allow_direct_fetch = false;
    config.upstreams = {upstream("primary", failed_primary),
                        upstream("secondary", valid_secondary, QByteArrayLiteral("read-key"))};

    const QString cache = QDir(directory.path()).filePath(QStringLiteral("downstream"));
    {
        MapTiles tiles(cache, 4, 16);
        tiles.configureProviders(providers, {}, {});
        tiles.setCascadeConfig(config);
        const Result result = fetch(&tiles, provider_id, 6, 30, 20, QStringLiteral("failover"));
        if (!require(!result.failed && result.data == validPng(),
                     "Failed primary did not fail over to authenticated MAP upstream")) return 1;
        if (!require(failed_primary.requests.size() == 1 && valid_secondary.requests.size() == 1 &&
                     origin.requests.isEmpty(),
                     "Incorrect upstream order or forbidden direct-provider request")) return 1;
        if (!require(valid_secondary.requests[0].toLower().contains(
                         "x-aowis-raster-source: " + fingerprint) &&
                     valid_secondary.requests[0].toLower().contains("x-api-key: read-key") &&
                     valid_secondary.requests[0].toLower().contains("x-aowis-cascade-visited: "),
                     "MAP-to-MAP request is missing source binding, read key, or loop trace")) return 1;
        const QString tile_path = QDir(cache).filePath(
            QStringLiteral("maptiles/%1/6/30/20.png").arg(provider_id));
        QFile file(tile_path);
        if (!require(file.open(QIODevice::ReadOnly) && file.readAll() == validPng(),
                     "MAP tile did not persist atomically before completion")) return 1;
    }
    {
        MapTiles restarted(cache, 2, 8);
        restarted.configureProviders(providers, {}, {});
        restarted.setCascadeConfig(config);
        const Result cached = fetch(&restarted, provider_id, 6, 30, 20,
                                    QStringLiteral("offline-cache"));
        if (!require(!cached.failed && cached.data == validPng() &&
                     failed_primary.requests.size() == 1 && valid_secondary.requests.size() == 1,
                     "Offline restart did not reuse local cache without another WAN request")) return 1;
    }
    // Auth failures and source mismatches must not poison the persistent cache.
    {
        MapTiles tiles(QDir(directory.path()).filePath(QStringLiteral("mismatch")), 2, 8);
        tiles.configureProviders(providers, {}, {});
        tiles.setCascadeConfig(config);
        valid_secondary.source = QByteArray(64, '0');
        const Result mismatch = fetch(&tiles, provider_id, 6, 30, 21, QStringLiteral("mismatch"));
        if (!require(mismatch.failed && origin.requests.isEmpty(),
                     "Mismatched source was accepted or direct fetching used")) return 1;
        const QString path = QDir(directory.path()).filePath(
            QStringLiteral("mismatch/maptiles/%1/6/30/21.png").arg(provider_id));
        if (!require(!QFile::exists(path), "Mismatched source poisoned cache")) return 1;
        valid_secondary.source = fingerprint;
    }
    // Explicit direct fallback operates only after both MAP upstreams fail.
    {
        MapTiles tiles(QDir(directory.path()).filePath(QStringLiteral("fallback")), 2, 8);
        tiles.configureProviders(providers, {}, {});
        config.allow_direct_fetch = true;
        tiles.setCascadeConfig(config);
        valid_secondary.status = 503;
        const Result fallback = fetch(&tiles, provider_id, 6, 30, 22,
                                      QStringLiteral("direct-fallback"));
        if (!require(!fallback.failed && fallback.data == validPng() &&
                     origin.requests.size() == 1,
                     "Direct fallback was not attempted after all MAP upstreams failed")) return 1;
        valid_secondary.status = 200;
    }
    // Invalid visit trace should never launch a network request.
    {
        MapTiles tiles(QDir(directory.path()).filePath(QStringLiteral("loops")), 2, 8);
        tiles.configureProviders(providers, {}, {});
        tiles.setCascadeConfig(config);
        const MapTiles::TileRequestResult bad = tiles.getTile(
            provider_id, 6, 30, 23, QStringLiteral("loop"), QByteArrayLiteral("invalid"));
        if (!require(bad.status == MapTiles::TileRequestStatus::Unavailable &&
                     origin.requests.size() == 1,
                     "Malformed cascade trace was accepted")) return 1;
    }
    return 0;
}
