#include "server.h"
#include <aowis/map/terrain_data.h>
#include <aowis/map/terrain_tile.h>
#include <aowis/map/terrain_transfer.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>

class TerrainZstdHttpTestAccess
{
public:
    static QString path(const Server &server, const Aowis::Map::TerrainTileAddress &address)
    {
        return server.terrain_data->normalizedTilePath(QStringLiteral("copernicus-glo30"), address);
    }
    static quint16 port(const Server &server) { return server.tcp->serverPort(); }
    static void blockWorker(Server &server, QSemaphore &entered, QSemaphore &release)
    {
        server.terrain_worker_pool.start([&entered, &release]()
        {
            entered.release();
            release.acquire();
        });
    }
    static int waiters(const Server &server) { return server.pending_terrain_tile_count; }
    static int jobs(const Server &server) { return server.pending_terrain_tiles.size(); }
};

namespace
{
bool require(bool condition, const char *message)
{
    if (!condition) std::fprintf(stderr, "%s\n", message);
    return condition;
}
bool await(QCoreApplication &application, QNetworkReply *first, QNetworkReply *second = nullptr)
{
    QElapsedTimer timer;
    timer.start();
    while ((!first->isFinished() || (second != nullptr && !second->isFinished())) &&
           timer.elapsed() < 5000)
    {
        application.processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    return first->isFinished() && (second == nullptr || second->isFinished());
}
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    using namespace Aowis::Map;
    QTemporaryDir directory;
    if (!require(directory.isValid(), "Unable to create temporary cache")) return 1;
    Server::Config config;
    config.port = 0;
    config.cache_directory = directory.path();
    config.terrain_cache_directory = directory.path() + "/terrain";
    config.terrain_remote_fetch_enabled = false;
    config.map_remote_fetch_enabled = false;
    config.maximum_terrain_workers = 1;
    Server server(config);
    if (!require(server.start(), "Unable to start MAP HTTP server")) return 1;

    TerrainTileAddress address{8, 130, 101};
    TerrainTile tile;
    tile.address = address;
    tile.dataset = QStringLiteral("copernicus-glo30");
    tile.vertical_datum = TerrainVerticalDatum::Egm2008;
    tile.nominal_resolution_m = 30;
    tile.elevations_m.fill(300.0f, TerrainTileSampleCount);
    const QByteArray canonical = encodeTerrainTile(tile);
    if (!require(!canonical.isEmpty(), "Unable to create terrain tile")) return 1;
    const QString path = TerrainZstdHttpTestAccess::path(server, address);
    if (!require(QDir().mkpath(QFileInfo(path).absolutePath()),
                 "Unable to create normalized cache directory")) return 1;
    QFile cached(path);
    if (!require(cached.open(QIODevice::WriteOnly) && cached.write(canonical) == canonical.size(),
                 "Unable to seed normalized tile cache")) return 1;
    cached.close();

    const QUrl url(QStringLiteral("http://127.0.0.1:%1/terrain/v1/copernicus-glo30/8/130/101.aowterrain")
                   .arg(TerrainZstdHttpTestAccess::port(server)));
    QNetworkAccessManager network;
    QSemaphore entered;
    QSemaphore release;
    TerrainZstdHttpTestAccess::blockWorker(server, entered, release);
    if (!require(entered.tryAcquire(1, 3000), "Unable to reserve worker for mixed request test"))
    {
        release.release();
        return 1;
    }
    const QByteArray cascade_id = QByteArrayLiteral("00000000-0000-4000-8000-000000000001");
    QNetworkRequest zstd_request(url);
    zstd_request.setRawHeader("X-AOWIS-Cascade-Visited", cascade_id);
    zstd_request.setRawHeader("Accept-Encoding", "zstd");
    QNetworkReply *zstd_reply = network.get(zstd_request);
    QNetworkRequest plain_request(url);
    // Even a GUI client that advertises zstd must receive canonical bytes.
    plain_request.setRawHeader("Accept-Encoding", "zstd");
    QNetworkReply *plain_reply = network.get(plain_request);
    QElapsedTimer timer;
    timer.start();
    while (TerrainZstdHttpTestAccess::waiters(server) != 2 && timer.elapsed() < 5000)
    {
        application.processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    const bool shared = TerrainZstdHttpTestAccess::waiters(server) == 2 &&
                        TerrainZstdHttpTestAccess::jobs(server) == 1;
    release.release();
    if (!require(shared && await(application, zstd_reply, plain_reply),
                 "Mixed compressed/plain requests did not share terrain job")) return 1;
    const QByteArray zstd_wire = zstd_reply->readAll();
    const QByteArray plain_wire = plain_reply->readAll();
    if (!require(zstd_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200 &&
                 plain_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200 &&
                 zstd_reply->rawHeader("Content-Encoding").toLower() == "zstd" &&
                 plain_reply->rawHeader("Content-Encoding").isEmpty(),
                 "HTTP content encoding negotiation failed")) return 1;
    if (!require(plain_wire == canonical && zstd_wire.size() < canonical.size(),
                 "Mixed terrain representations are incorrect")) return 1;
    const std::optional<QByteArray> decoded = decompressTerrainTileFromTransfer(zstd_wire);
    if (!require(decoded.has_value() && *decoded == canonical,
                 "Compressed HTTP terrain differs from cache")) return 1;
    if (!require(zstd_reply->rawHeader("Vary").contains("Accept-Encoding") &&
                 plain_reply->rawHeader("Vary").contains("Accept-Encoding"),
                 "HTTP cache variation missing")) return 1;
    zstd_reply->deleteLater();
    plain_reply->deleteLater();

    QNetworkRequest disabled_request(url);
    disabled_request.setRawHeader("X-AOWIS-Cascade-Visited", cascade_id);
    disabled_request.setRawHeader("Accept-Encoding", "zstd;q=0, gzip");
    QNetworkReply *disabled_reply = network.get(disabled_request);
    if (!require(await(application, disabled_reply), "Disabled zstd request stalled")) return 1;
    if (!require(disabled_reply->rawHeader("Content-Encoding").isEmpty() &&
                 disabled_reply->readAll() == canonical,
                 "Server compressed terrain despite zstd;q=0")) return 1;
    disabled_reply->deleteLater();

    // A normal Qt client may add Accept-Encoding implicitly. That must
    // never opt it into the MAP-to-MAP compressed representation.
    QNetworkRequest ordinary_request(url);
    QNetworkReply *ordinary_reply = network.get(ordinary_request);
    if (!require(await(application, ordinary_reply), "Ordinary GUI request stalled")) return 1;
    if (!require(ordinary_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200 &&
                 ordinary_reply->rawHeader("Content-Encoding").isEmpty() &&
                 ordinary_reply->readAll() == canonical,
                 "Ordinary GUI request received a compressed terrain representation")) return 1;
    ordinary_reply->deleteLater();
    return 0;
}
