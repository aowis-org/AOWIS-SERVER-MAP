#include <aowis/map/terrain_data.h>
#include <aowis/map/terrain_transfer.h>

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QtConcurrentRun>

#include <cstdio>

namespace
{
bool require(bool condition, const char *message)
{
    if (!condition)
        std::fprintf(stderr, "%s\n", message);
    return condition;
}

class TerrainUpstream final : public QTcpServer
{
public:
    QByteArray response_body;
    QByteArray required_key;
    QByteArray revision = QByteArray::number(Aowis::Map::TerrainNormalizationRevision);
    int response_status = 200;
    int request_count = 0;
    QByteArray last_request;
    bool offer_zstd = false;
    bool invalid_zstd = false;

    bool start() { return listen(QHostAddress::LocalHost, 0); }
    QUrl origin() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(serverPort()));
    }

protected:
    void incomingConnection(qintptr descriptor) override
    {
        QTcpSocket *socket = new QTcpSocket(this);
        if (!socket->setSocketDescriptor(descriptor))
        {
            socket->deleteLater();
            return;
        }
        QObject::connect(socket, &QTcpSocket::disconnected,
                         socket, &QObject::deleteLater);
        QObject::connect(socket, &QTcpSocket::readyRead, socket,
            [this, socket, received = QByteArray()]() mutable
        {
            received += socket->readAll();
            const qsizetype end = received.indexOf("\r\n\r\n");
            if (end < 0) return;
            this->last_request = received.left(end + 4);
            ++this->request_count;
            const QByteArray lowercase = this->last_request.toLower();
            const bool authorized = this->required_key.isEmpty() ||
                lowercase.contains("x-api-key: " + this->required_key.toLower() + "\r\n");
            const int status = authorized ? this->response_status : 401;
            QByteArray body = status == 200 ? this->response_body : QByteArrayLiteral("error");
            bool compressed = false;
            if (status == 200 && this->offer_zstd &&
                lowercase.contains("accept-encoding: zstd"))
            {
                const std::optional<QByteArray> encoded =
                    Aowis::Map::compressTerrainTileForTransfer(body);
                if (encoded.has_value())
                {
                    body = this->invalid_zstd ? encoded->left(encoded->size() - 1) : *encoded;
                    compressed = true;
                }
            }
            QByteArray response = "HTTP/1.1 " + QByteArray::number(status) +
                (status == 200 ? " OK\r\n" : " Error\r\n");
            response += "Content-Type: application/vnd.aowis.terrain\r\n";
            response += "X-AOWIS-Terrain-Revision: " + this->revision + "\r\n";
            if (compressed)
                response += "Content-Encoding: zstd\r\n";
            response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
            response += "Connection: close\r\n\r\n";
            socket->write(response + body);
            socket->disconnectFromHost();
        });
    }
};

Aowis::Map::TerrainTile makeTile(const Aowis::Map::TerrainTileAddress &address)
{
    Aowis::Map::TerrainTile tile;
    tile.dataset = QStringLiteral("copernicus-glo30");
    tile.address = address;
    tile.vertical_datum = Aowis::Map::TerrainVerticalDatum::Egm2008;
    tile.nominal_resolution_m = 30.0;
    tile.elevations_m.resize(Aowis::Map::TerrainTileSampleCount);
    for (int index = 0; index < tile.elevations_m.size(); ++index)
        tile.elevations_m[index] = 75.0f + float(index % 113) * 0.5f;
    return tile;
}

Aowis::Map::MapCascadeUpstream upstream(const char *name, const TerrainUpstream &server,
                                       const QByteArray &key = {})
{
    Aowis::Map::MapCascadeUpstream connection;
    connection.name = QString::fromLatin1(name);
    connection.url = server.origin();
    connection.api_key = key;
    return connection;
}

Aowis::Map::TerrainProviderFetchResult acquire(
    const Aowis::Map::TerrainData &data,
    const Aowis::Map::TerrainTileAddress &address,
    const QByteArray &visited = {})
{
    QFutureWatcher<Aowis::Map::TerrainProviderFetchResult> watcher;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&watcher, &QFutureWatcherBase::finished,
                     &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run([&data, address, visited]()
    {
        return data.acquireTileSources(QStringLiteral("copernicus-glo30"),
                                       address, {}, visited);
    }));
    timeout.start(16000);
    if (!watcher.isFinished()) loop.exec();
    if (!require(watcher.isFinished(), "Terrain MAP upstream request timed out"))
        return {};
    return watcher.result();
}
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    using namespace Aowis::Map;

    QTemporaryDir directory;
    if (!require(directory.isValid(), "Failed to create test cache")) return 1;

    TerrainUpstream failed;
    TerrainUpstream healthy;
    if (!require(failed.start() && healthy.start(), "Failed to start MAP upstream stubs"))
        return 1;
    failed.response_status = 503;
    healthy.required_key = QByteArrayLiteral("read-key");
    healthy.offer_zstd = true;

    TerrainData::Config config;
    config.cache_directory = directory.path();
    config.cascade.allow_direct_fetch = false;
    config.cascade.poor_internet_mode = true;
    config.cascade.upstreams.append(upstream("failed", failed));
    config.cascade.upstreams.append(upstream("healthy", healthy, healthy.required_key));
    TerrainData terrain(config);
    QString error;
    if (!require(terrain.initialize(&error), "Failed to initialize terrain data")) return 1;

    TerrainTileAddress address;
    address.zoom = 8;
    address.x = 128;
    address.y = 96;
    const QByteArray encoded = encodeTerrainTile(makeTile(address));
    if (!require(!encoded.isEmpty(), "Could not encode valid test tile")) return 1;
    healthy.response_body = encoded;

    const QByteArray visited = QByteArrayLiteral("00000000-0000-4000-8000-000000000001");
    const TerrainProviderFetchResult fetched = acquire(terrain, address, visited);
    if (!require(fetched.status == TerrainProviderFetchStatus::Ready,
                 "Primary failure did not fail over to the healthy terrain MAP upstream"))
        return 1;
    if (!require(failed.request_count == 1 && healthy.request_count == 1,
                 "Terrain cascade did not try upstreams in order")) return 1;
    if (!require(healthy.last_request.toLower().contains("x-api-key: read-key\r\n") &&
                 healthy.last_request.toLower().contains("x-aowis-cascade-visited: " + visited) &&
                 healthy.last_request.toLower().contains("accept-encoding: zstd\r\n"),
                 "Terrain cascade lost authentication or visited trace")) return 1;
    const TerrainTileLookupResult delivered = terrain.terrainTile(
        QStringLiteral("copernicus-glo30"), address, false);
    if (!require(delivered.status == TerrainTileLookupStatus::Ready &&
                 delivered.data == encoded,
                 "Upstream normalized terrain bytes were not persisted exactly")) return 1;

    failed.close();
    healthy.close();
    const TerrainTileLookupResult offline = terrain.terrainTile(
        QStringLiteral("copernicus-glo30"), address, false);
    if (!require(offline.status == TerrainTileLookupStatus::Ready &&
                 offline.data == encoded,
                 "Previously cascaded tile is not available offline")) return 1;

    TerrainUpstream incompatible;
    if (!require(incompatible.start(), "Could not start incompatible upstream")) return 1;
    incompatible.response_body = encoded;
    incompatible.revision = QByteArrayLiteral("1");
    TerrainData::Config incompatible_config = config;
    incompatible_config.cache_directory = QDir(directory.path()).filePath(QStringLiteral("bad-revision"));
    incompatible_config.cascade.upstreams.clear();
    incompatible_config.cascade.upstreams.append(upstream("old", incompatible));
    TerrainData incompatible_data(incompatible_config);
    if (!require(incompatible_data.initialize(&error), "Failed to initialize second terrain cache"))
        return 1;
    const TerrainProviderFetchResult rejected = acquire(incompatible_data, address);
    if (!require(rejected.status != TerrainProviderFetchStatus::Ready &&
                 !QFileInfo::exists(incompatible_data.normalizedTilePath(
                     QStringLiteral("copernicus-glo30"), address)),
                 "Incompatible normalization revision poisoned the local cache"))
        return 1;

    incompatible.revision = QByteArrayLiteral("2");
    incompatible.offer_zstd = true;
    incompatible.response_body = encoded;
    incompatible.invalid_zstd = true;
    const TerrainProviderFetchResult invalid_frame = acquire(incompatible_data, address);
    if (!require(invalid_frame.status != TerrainProviderFetchStatus::Ready &&
                 !QFileInfo::exists(incompatible_data.normalizedTilePath(
                     QStringLiteral("copernicus-glo30"), address)),
                 "Truncated zstd response poisoned the normalized cache")) return 1;

    incompatible.invalid_zstd = false;
    incompatible.response_body = encoded.left(encoded.size() - 7);
    const TerrainProviderFetchResult corrupt = acquire(incompatible_data, address);
    if (!require(corrupt.status != TerrainProviderFetchStatus::Ready &&
                 !QFileInfo::exists(incompatible_data.normalizedTilePath(
                     QStringLiteral("copernicus-glo30"), address)),
                 "Truncated/CRC-invalid terrain poisoned the local cache"))
        return 1;

    incompatible.offer_zstd = false; // Older upstream: identity must still work.
    incompatible.response_body = encoded;
    incompatible_data.cancelUpstreamDownloads();
    const TerrainProviderFetchResult repaired = acquire(incompatible_data, address);
    if (!require(repaired.status == TerrainProviderFetchStatus::Ready,
                 "Rejected terrain could not be fetched correctly later")) return 1;

    // Standalone calls the same core directly, without the server's remote
    // worker split. It must acquire normalized tiles rather than GeoTIFFs.
    TerrainData::Config embedded_config = incompatible_config;
    embedded_config.cache_directory = QDir(directory.path()).filePath(QStringLiteral("embedded"));
    TerrainData embedded(embedded_config);
    if (!require(embedded.initialize(&error), "Failed to initialize embedded terrain"))
        return 1;
    const TerrainTileLookupResult embedded_result = embedded.terrainTile(
        QStringLiteral("copernicus-glo30"), address, true);
    if (!require(embedded_result.status == TerrainTileLookupStatus::Ready &&
                 embedded_result.data == encoded,
                 "Embedded terrain did not acquire canonical bytes from MAP upstream"))
        return 1;

    // A disabled remote-fetch gate must not contact even a working MAP upstream.
    TerrainData::Config offline_config = incompatible_config;
    offline_config.cache_directory = QDir(directory.path()).filePath(QStringLiteral("offline"));
    offline_config.remote_fetch_enabled = false;
    TerrainData strictly_offline(offline_config);
    if (!require(strictly_offline.initialize(&error), "Failed to initialize offline cache"))
        return 1;
    const int requests_before = incompatible.request_count;
    strictly_offline.acquireTileSources(QStringLiteral("copernicus-glo30"), address);
    if (!require(incompatible.request_count == requests_before,
                 "Remote-fetch gate did not disable MAP terrain upstreams")) return 1;

    return 0;
}
