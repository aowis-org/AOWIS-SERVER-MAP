#include "../src/server/configuration.h"

#include <aowis/map/maptiles.h>
#include <aowis/map/provider_catalog.h>
#include <aowis/map/terrain_data.h>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QEventLoop>
#include <QHostAddress>
#include <QNetworkProxy>
#include <QtEndian>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

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

// Minimal authenticated SOCKS5 relay standing in for a WireGuard tunnel's
// local proxy (AOWIS-NETWORK): verifies that a WireGuard upstream is reached
// only through the proxy given by the host, with its credentials.
class Socks5Relay final : public QTcpServer
{
public:
    QByteArray user = QByteArrayLiteral("tunnel-user");
    QByteArray password = QByteArrayLiteral("tunnel-secret");
    int connections = 0;

    bool start() { return listen(QHostAddress::LocalHost, 0); }
    QNetworkProxy proxy() const
    {
        QNetworkProxy result(QNetworkProxy::Socks5Proxy, QStringLiteral("127.0.0.1"),
                             serverPort(), QString::fromLatin1(this->user),
                             QString::fromLatin1(this->password));
        result.setCapabilities(QNetworkProxy::TunnelingCapability |
                               QNetworkProxy::HostNameLookupCapability);
        return result;
    }

protected:
    void incomingConnection(qintptr descriptor) override
    {
        QTcpSocket *client = new QTcpSocket(this);
        if (!client->setSocketDescriptor(descriptor))
        {
            client->deleteLater();
            return;
        }
        QTcpSocket *target = new QTcpSocket(client);
        QObject::connect(client, &QTcpSocket::disconnected, client, &QObject::deleteLater);
        QObject::connect(target, &QTcpSocket::readyRead, client,
                         [client, target]() { client->write(target->readAll()); });
        QObject::connect(target, &QTcpSocket::disconnected, client,
                         [client]() { client->disconnectFromHost(); });
        QObject::connect(client, &QTcpSocket::readyRead, client,
            [this, client, target, stage = 0, buffer = QByteArray()]() mutable
        {
            buffer += client->readAll();
            if (stage == 3)
            {
                target->write(buffer);
                buffer.clear();
                return;
            }
            if (stage == 0)
            {
                if (buffer.size() < 2 || buffer.size() < 2 + quint8(buffer.at(1))) return;
                const bool offers_password = buffer.mid(2, quint8(buffer.at(1))).contains('\x02');
                buffer.remove(0, 2 + quint8(buffer.at(1)));
                client->write(QByteArray("\x05", 1) + (offers_password ? QByteArray("\x02", 1)
                                                                       : QByteArray("\xff", 1)));
                if (!offers_password) { client->disconnectFromHost(); return; }
                stage = 1;
            }
            if (stage == 1)
            {
                if (buffer.size() < 2) return;
                const int user_length = quint8(buffer.at(1));
                if (buffer.size() < 3 + user_length) return;
                const int password_length = quint8(buffer.at(2 + user_length));
                if (buffer.size() < 3 + user_length + password_length) return;
                const bool authorized = buffer.mid(2, user_length) == this->user &&
                    buffer.mid(3 + user_length, password_length) == this->password;
                buffer.remove(0, 3 + user_length + password_length);
                client->write(authorized ? QByteArray("\x01\x00", 2) : QByteArray("\x01\x01", 2));
                if (!authorized) { client->disconnectFromHost(); return; }
                stage = 2;
            }
            if (stage == 2)
            {
                if (buffer.size() < 5) return;
                const char type = buffer.at(3);
                QString host;
                int length = 0;
                if (type == 1)
                {
                    length = 4 + 4 + 2;
                    if (buffer.size() < length) return;
                    host = QHostAddress(qFromBigEndian<quint32>(
                        reinterpret_cast<const uchar *>(buffer.constData() + 4))).toString();
                }
                else if (type == 3)
                {
                    length = 4 + 1 + quint8(buffer.at(4)) + 2;
                    if (buffer.size() < length) return;
                    host = QString::fromLatin1(buffer.mid(5, quint8(buffer.at(4))));
                }
                else
                {
                    client->disconnectFromHost();
                    return;
                }
                const quint16 port = qFromBigEndian<quint16>(
                    reinterpret_cast<const uchar *>(buffer.constData() + length - 2));
                buffer.remove(0, length);
                ++this->connections;
                stage = 3;
                QObject::connect(target, &QTcpSocket::connected, client,
                    [client, target, &buffer]()
                {
                    client->write(QByteArray("\x05\x00\x00\x01\x00\x00\x00\x00\x00\x00", 10));
                    if (!buffer.isEmpty()) target->write(buffer);
                    buffer.clear();
                });
                target->connectToHost(host, port);
            }
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
               config.cascade.poor_internet_mode && config.cascade.jpeg_quality == 80,
               "Cascade configuration changed order or values (adaptive_compression=false is ignored)"))
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

    // An upstream designated for WireGuard is never contacted directly by
    // raster tiles, terrain or provider discovery: the next direct upstream
    // is used instead.
    StubHttpServer tunnelled;
    StubHttpServer plain;
    if (!check(tunnelled.start() && plain.start(), "Could not start WireGuard test servers"))
        return 1;
    Aowis::Map::MapCascadeConfig wireguard_cascade;
    wireguard_cascade.allow_direct_fetch = false;
    Aowis::Map::MapCascadeUpstream via_tunnel;
    via_tunnel.name = QStringLiteral("tunnelled");
    via_tunnel.url = QUrl(QString::fromUtf8(tunnelled.origin()));
    via_tunnel.wireguard_profile_id = QStringLiteral("019a47b1-2040-7af9-8d1c-2670badb6399");
    Aowis::Map::MapCascadeUpstream direct_upstream;
    direct_upstream.name = QStringLiteral("plain");
    direct_upstream.url = QUrl(QString::fromUtf8(plain.origin()));
    wireguard_cascade.upstreams.append(via_tunnel);
    wireguard_cascade.upstreams.append(direct_upstream);
    MapTiles wireguard_tiles(QDir(directory.path()).filePath(QStringLiteral("wireguard")), 1, 2);
    wireguard_tiles.configureProviders(providers, {}, {});
    wireguard_tiles.setCascadeConfig(wireguard_cascade);
    bool wireguard_failed = false;
    QEventLoop wireguard_loop;
    QObject::connect(&wireguard_tiles, &MapTiles::tileFailed, &wireguard_loop,
                     [&wireguard_failed, &wireguard_loop](const QString &key, MapTiles::TileFailureReason)
    {
        if (key != QStringLiteral("wireguard-miss")) return;
        wireguard_failed = true;
        wireguard_loop.quit();
    });
    QTimer wireguard_timeout;
    wireguard_timeout.setSingleShot(true);
    QObject::connect(&wireguard_timeout, &QTimer::timeout, &wireguard_loop, &QEventLoop::quit);
    if (!check(wireguard_tiles.getTile(provider, 1, 0, 0, QStringLiteral("wireguard-miss")).status ==
                   MapTiles::TileRequestStatus::Pending,
               "Direct upstream after a WireGuard upstream was not queued")) return 1;
    wireguard_timeout.start(5000);
    wireguard_loop.exec();
    if (!check(wireguard_failed && plain.requests.size() == 1 && tunnelled.requests.isEmpty(),
               "Raster tiles contacted a WireGuard upstream directly")) return 1;

    Aowis::Map::TerrainData::Config wireguard_terrain_config;
    wireguard_terrain_config.cache_base_directory =
        QDir(directory.path()).filePath(QStringLiteral("wireguard-terrain"));
    wireguard_terrain_config.cascade = wireguard_cascade;
    wireguard_terrain_config.cascade.upstreams.removeLast();
    Aowis::Map::TerrainData wireguard_terrain(wireguard_terrain_config);
    if (!check(wireguard_terrain.initialize(&terrain_error), "Could not initialize WireGuard terrain"))
        return 1;
    wireguard_terrain.acquireTileSources(QStringLiteral("copernicus-glo30"), address);

    Aowis::Map::ProviderCatalog catalog(QDir(directory.path()).filePath(QStringLiteral("catalog")));
    catalog.configure({}, wireguard_terrain_config.cascade);
    catalog.refresh();
    QEventLoop settle;
    QTimer::singleShot(300, &settle, &QEventLoop::quit);
    settle.exec();
    if (!check(tunnelled.requests.isEmpty(),
               "Terrain or provider discovery contacted a WireGuard upstream directly")) return 1;

    // With the host's tunnel proxy set, raster tiles, terrain and provider
    // discovery reach the WireGuard upstream through it (and only through it).
    Socks5Relay relay;
    if (!check(relay.start(), "Could not start SOCKS5 relay")) return 1;
    Aowis::Map::MapCascadeConfig tunnel_cascade = wireguard_cascade;
    tunnel_cascade.upstreams[0].proxy = relay.proxy();
    wireguard_tiles.setCascadeConfig(tunnel_cascade);
    wireguard_failed = false;
    QObject::disconnect(&wireguard_tiles, &MapTiles::tileFailed, nullptr, nullptr);
    QObject::connect(&wireguard_tiles, &MapTiles::tileFailed, &wireguard_loop,
                     [&wireguard_failed, &wireguard_loop](const QString &key, MapTiles::TileFailureReason)
    {
        if (key != QStringLiteral("wireguard-tunnel")) return;
        wireguard_failed = true;
        wireguard_loop.quit();
    });
    if (!check(wireguard_tiles.getTile(provider, 1, 1, 0, QStringLiteral("wireguard-tunnel")).status ==
                   MapTiles::TileRequestStatus::Pending,
               "WireGuard upstream with tunnel was not queued")) return 1;
    wireguard_timeout.start(5000);
    wireguard_loop.exec();
    if (!check(wireguard_failed && tunnelled.requests.size() == 1 && relay.connections == 1 &&
               plain.requests.size() == 2,
               "Raster tiles did not use the tunnel, or did not fail over after it")) return 1;

    Aowis::Map::TerrainData::Config tunnel_terrain_config = wireguard_terrain_config;
    tunnel_terrain_config.cascade.upstreams[0].proxy = relay.proxy();
    wireguard_terrain.setCascadeConfig(tunnel_terrain_config.cascade);
    wireguard_terrain.acquireTileSources(QStringLiteral("copernicus-glo30"), address);
    if (!check(tunnelled.requests.size() == 2 && relay.connections == 2,
               "Terrain did not reach the WireGuard upstream through its tunnel")) return 1;

    catalog.configure({}, tunnel_terrain_config.cascade);
    catalog.refresh();
    QEventLoop discovered;
    QTimer::singleShot(1000, &discovered, &QEventLoop::quit);
    discovered.exec();
    if (!check(tunnelled.requests.size() == 3 && relay.connections == 3 &&
               tunnelled.requests.last().startsWith("GET /map/v1/providers/available "),
               "Provider discovery did not reach the WireGuard upstream through its tunnel")) return 1;
    return 0;
}
