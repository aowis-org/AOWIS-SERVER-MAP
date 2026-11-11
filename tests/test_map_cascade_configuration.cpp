#include "../src/server/configuration.h"

#include <aowis/map/maptiles.h>
#include <aowis/map/terrain_data.h>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QEventLoop>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>

namespace
{
// Loopback-only fixtures verify which URLs the parsed configuration actually
// contacts. No test request may reach the public providers or example.org.
class StubHttpServer final : public QTcpServer
{
public:
    int status = 503;
    QList<QByteArray> requests;

    bool start()
    {
        return listen(QHostAddress::LocalHost, 0);
    }

    QByteArray origin() const
    {
        return QStringLiteral("http://127.0.0.1:%1").arg(serverPort()).toUtf8();
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
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        QObject::connect(socket, &QTcpSocket::readyRead, socket,
                         [this, socket, received = QByteArray()]() mutable
        {
            received.append(socket->readAll());
            if (!received.contains("\r\n\r\n")) return;
            this->requests.append(received.left(received.indexOf("\r\n\r\n") + 4));
            const QByteArray response = QByteArrayLiteral("HTTP/1.1 ") +
                QByteArray::number(this->status) +
                QByteArrayLiteral(" Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            socket->write(response);
            socket->disconnectFromHost();
        });
    }
};

bool loadIni(const QString &path, const QByteArray &ini, Server::Config *config,
             QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        file.write(ini) != ini.size())
        return false;
    file.close();
    error->clear();
    return MapServerConfiguration::loadConfigFile(path, config, error);
}

bool check(bool ok, const char *description)
{
    if (!ok) std::fprintf(stderr, "%s\n", description);
    return ok;
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    if (!check(directory.isValid(), "Temporary directory failed")) return 1;
    const QString path = QDir(directory.path()).filePath(QStringLiteral("config.ini"));
    StubHttpServer primary;
    StubHttpServer secondary;
    StubHttpServer direct_origin;
    primary.status = 503;
    secondary.status = 404;
    if (!check(primary.start() && secondary.start() && direct_origin.start(),
               "Could not start loopback MAP test servers")) return 1;

    Server::Config config;
    QString error;
    const QByteArray ini =
        QByteArrayLiteral("[upstream]\nservers=main,secondary\nallow_direct_fetch=false\n"
                          "[upstream.main]\nurl=") + primary.origin() +
        QByteArrayLiteral("\napi_key=one\n[upstream.secondary]\nurl=") + secondary.origin() +
        QByteArrayLiteral("\napi_key=two\n[bandwidth]\npoor_internet_mode=true\n"
                          "jpeg_quality=80\nadaptive_compression=false\n");
    if (!check(loadIni(path, ini, &config, &error), "Valid cascade INI rejected"))
    {
        std::fprintf(stderr, "%s\n", qPrintable(error));
        return 1;
    }
    if (!check(config.cascade.upstreams.size() == 2 &&
               config.cascade.upstreams[0].name == QStringLiteral("main") &&
               config.cascade.upstreams[1].name == QStringLiteral("secondary") &&
               config.cascade.upstreams[0].api_key == QByteArrayLiteral("one") &&
               !config.cascade.allow_direct_fetch &&
               config.cascade.poor_internet_mode && config.cascade.jpeg_quality == 80 &&
               !config.cascade.adaptive_compression, "Cascade configuration changed order or values"))
        return 1;

    // Step 2 should queue configured MAP upstreams (not return Unavailable),
    // exhaust them in order, and NEVER contact the direct provider when the
    // parsed INI explicitly forbids direct fetching.
    QVector<Aowis::Map::MapProvider> providers = Aowis::Map::defaultMapProviders();
    providers[0].url_template = QString::fromUtf8(direct_origin.origin()) +
        QStringLiteral("/{z}/{x}/{y}.png");
    MapTiles tiles(directory.path(), 1, 2);
    tiles.configureProviders(providers, {}, {});
    tiles.setCascadeConfig(config.cascade);
    const QString provider = providers.first().id;

    bool failed = false;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&tiles, &MapTiles::tileFailed, &loop,
                     [&failed, &loop](const QString &key, MapTiles::TileFailureReason)
    {
        if (key != QStringLiteral("miss")) return;
        failed = true;
        loop.quit();
    });
    const MapTiles::TileRequestResult miss = tiles.getTile(provider, 0, 0, 0,
                                                           QStringLiteral("miss"));
    if (!check(miss.status == MapTiles::TileRequestStatus::Pending,
               "Configured MAP upstream cache miss should be pending")) return 1;
    timeout.start(5000);
    loop.exec();
    if (timeout.isActive()) timeout.stop();
    if (!check(failed && primary.requests.size() == 1 && secondary.requests.size() == 1 &&
               direct_origin.requests.isEmpty(),
               "MAP failover did not try both upstreams or contacted a forbidden direct origin"))
        return 1;
    if (!check(primary.requests[0].toLower().contains("x-api-key: one\r\n") &&
               secondary.requests[0].toLower().contains("x-api-key: two\r\n"),
               "Configured MAP upstream authentication keys were not forwarded"))
        return 1;

    // Cache-first semantics must continue to work with WAN/direct origin disabled.
    const QString cache_path = QDir(directory.path()).filePath(
        QStringLiteral("maptiles/%1/0/0").arg(provider));
    if (!check(QDir().mkpath(cache_path), "Could not create cache directory")) return 1;
    QFile cached_tile(QDir(cache_path).filePath(QStringLiteral("0.png")));
    if (!check(cached_tile.open(QIODevice::WriteOnly) &&
               cached_tile.write("local-tile", 10) == 10,
               "Could not create cached tile")) return 1;
    cached_tile.close();
    const MapTiles::TileRequestResult hit = tiles.getTile(provider, 0, 0, 0,
                                                          QStringLiteral("cached"));
    if (!check(hit.status == MapTiles::TileRequestStatus::Ready &&
               hit.data == QByteArrayLiteral("local-tile"),
               "Cascade-only mode could not serve local cached data")) return 1;

    // Terrain must also be unable to fetch a GeoTIFF when the upstream-only
    // policy is configured. A miss is allowed; direct network acquisition is not.
    Aowis::Map::TerrainData::Config terrain_config;
    terrain_config.cache_base_directory = directory.path();
    terrain_config.cascade = config.cascade;
    Aowis::Map::TerrainData terrain(terrain_config);
    QString terrain_error;
    if (!check(terrain.initialize(&terrain_error), "Could not initialize terrain cache"))
    {
        std::fprintf(stderr, "%s\n", qPrintable(terrain_error));
        return 1;
    }
    Aowis::Map::TerrainTileAddress address;
    address.zoom = 8;
    address.x = 128;
    address.y = 128;
    const Aowis::Map::TerrainTileLookupResult no_origin =
        terrain.terrainTile(QStringLiteral("copernicus-glo30"), address, true);
    if (!check(no_origin.status != Aowis::Map::TerrainTileLookupStatus::Ready,
               "Cascade-only terrain miss unexpectedly acquired an original source")) return 1;

    const QByteArray invalid_cases[] = {
        QByteArrayLiteral("[upstream]\nservers=a,a\n[upstream.a]\nurl=https://example.org\n"),
        QByteArrayLiteral("[upstream]\nservers=a,,b\n[upstream.a]\nurl=https://a.example\n"
                          "[upstream.b]\nurl=https://b.example\n"),
        QByteArrayLiteral("[upstream]\nservers=,a\n[upstream.a]\nurl=https://a.example\n"),
        QByteArrayLiteral("[upstream]\nservers=a,\n[upstream.a]\nurl=https://a.example\n"),
        QByteArrayLiteral("[upstream]\nservers=a\n[upstream.a]\nurl=https://a.example\n"
                          "api_kye=typo\n"),
        QByteArrayLiteral("[upstream]\nservers=a\n[upstream.a]\nurl=file:///tmp/tile\n"),
        QByteArrayLiteral("[upstream]\nservers=a\n[upstream.a]\nurl=https://x/?key=secret\n"),
        QByteArrayLiteral("[upstream]\nservers=a\n[upstream.a]\nurl=https://x/path\n"),
        QByteArrayLiteral("[upstream]\nservers=a,b\n[upstream.a]\nurl=https://x\n"
                          "[upstream.b]\nurl=https://x\n"),
        QByteArrayLiteral("[upstream.z]\nurl=https://z\n"),
        QByteArrayLiteral("[bandwidth]\njpeg_quality=101\n"),
        QByteArrayLiteral("[bandwidth]\njpeg_quality=0\n"),
        QByteArrayLiteral("[bandwidth]\npoor_internet_mode=maybe\n"),
        QByteArrayLiteral("[upstream]\nallow_direct_fetch=maybe\n")
    };
    for (const QByteArray &ini : invalid_cases)
    {
        Server::Config invalid;
        if (!check(!loadIni(path, ini, &invalid, &error),
                   "Invalid cascade INI was accepted")) return 1;
    }
    Server::Config defaults;
    if (!check(loadIni(path, MapServerConfiguration::defaultConfigText(), &defaults, &error) &&
               defaults.cascade.upstreams.isEmpty() && defaults.cascade.allow_direct_fetch &&
               !defaults.cascade.poor_internet_mode,
               "Default INI breaks existing fetch mode")) return 1;
    return 0;
}
