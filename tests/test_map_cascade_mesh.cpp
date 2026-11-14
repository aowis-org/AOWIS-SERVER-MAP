// Real MAP servers on loopback, wired the ways a field deployment uses them:
// two hubs that are each other's upstreams, a hub whose own upstream needs
// longer than a request timeout, a busy upstream, and a dead upstream that
// accepts connections but never answers. Each scenario checks the cascade
// behavior end to end (HTTP status, bytes, timing, origin/source requests)
// and the per-upstream state reported by /upstream/v1/activity.
#include "server.h"

#include <aowis/map/cascade_upstream_health.h>
#include <aowis/map/terrain_data.h>
#include <aowis/map/terrain_tile.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <png.h>

#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

class MapCascadeMeshTestAccess
{
public:
    static quint16 port(const Server &server) { return server.tcp->serverPort(); }
    static QByteArray identity(const Server &server) { return server.cascade_instance_id; }
    static QString terrainPath(const Server &server,
                               const Aowis::Map::TerrainTileAddress &address)
    {
        return server.terrain_data->normalizedTilePath(QStringLiteral("copernicus-glo30"), address);
    }
};

namespace
{
bool require(bool condition, const char *message)
{
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

void pump(QCoreApplication &application, int milliseconds)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < milliseconds)
    {
        application.processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
}

QByteArray samplePng(quint32 seed)
{
    std::vector<unsigned char> pixels(256 * 256 * 3);
    quint32 state = seed;
    for (size_t index = 0; index < pixels.size(); ++index)
    {
        state = state * 1664525u + 1013904223u;
        pixels[index] = static_cast<unsigned char>(state >> 24);
    }
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    image.width = 256;
    image.height = 256;
    image.format = PNG_FORMAT_RGB;
    png_alloc_size_t length = 0;
    if (!png_image_write_to_memory(&image, nullptr, &length, 0, pixels.data(), 0, nullptr))
        return {};
    QByteArray encoded(int(length), Qt::Uninitialized);
    if (!png_image_write_to_memory(&image, encoded.data(), &length, 0, pixels.data(), 0, nullptr))
        return {};
    encoded.resize(int(length));
    return encoded;
}

bool saveFile(const QString &path, const QByteArray &content)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(content) == content.size() && file.flush();
}

QByteArray terrainTile(const Aowis::Map::TerrainTileAddress &address, float elevation_m)
{
    Aowis::Map::TerrainTile tile;
    tile.address = address;
    tile.dataset = QStringLiteral("copernicus-glo30");
    tile.vertical_datum = Aowis::Map::TerrainVerticalDatum::Egm2008;
    tile.nominal_resolution_m = 30;
    tile.elevations_m.fill(elevation_m, Aowis::Map::TerrainTileSampleCount);
    return Aowis::Map::encodeTerrainTile(tile);
}

// Minimal HTTP/1.1 peer. respond() builds the complete response for one
// request (empty: never answer, a "black hole" that keeps the connection).
class StubHttp final : public QTcpServer
{
public:
    std::function<QByteArray(const QByteArray &request)> respond;
    int delay_ms = 0;
    QList<QByteArray> requests;

    bool start() { return listen(QHostAddress::LocalHost, 0); }
    QUrl origin() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(serverPort())); }
    int count(const QByteArray &needle) const
    {
        int matches = 0;
        for (const QByteArray &request : this->requests)
            if (request.contains(needle)) ++matches;
        return matches;
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
            [this, socket, buffer = QByteArray()]() mutable
        {
            buffer += socket->readAll();
            while (true)
            {
                const qsizetype end = buffer.indexOf("\r\n\r\n");
                if (end < 0) return;
                const QByteArray request = buffer.left(end + 4);
                buffer.remove(0, end + 4);
                this->requests.append(request.toLower());
                const QByteArray response = this->respond ? this->respond(request.toLower())
                                                          : QByteArray();
                if (response.isEmpty()) continue; // Black hole.
                const QPointer<QTcpSocket> target(socket);
                QTimer::singleShot(this->delay_ms, socket, [target, response]()
                {
                    if (!target.isNull()) target->write(response);
                });
            }
        });
    }
};

QByteArray httpResponse(int status, const QByteArray &reason, const QByteArray &headers,
                        const QByteArray &body)
{
    return "HTTP/1.1 " + QByteArray::number(status) + ' ' + reason + "\r\n" + headers +
           "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
}

struct HttpResult
{
    int code = 0;
    QByteArray body;
    qint64 elapsed_ms = 0;
};

class PendingGet
{
public:
    PendingGet(QNetworkAccessManager &network, const QUrl &url)
    {
        this->elapsed.start();
        QNetworkRequest request(url);
        this->reply = network.get(request);
    }
    bool finished() const { return this->reply->isFinished(); }
    HttpResult take()
    {
        HttpResult result;
        if (this->reply->isFinished())
        {
            result.code = this->reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            result.body = this->reply->readAll();
        }
        else
            this->reply->abort();
        result.elapsed_ms = this->finished_ms > 0 ? this->finished_ms : this->elapsed.elapsed();
        this->reply->deleteLater();
        return result;
    }
    void markIfFinished()
    {
        if (this->finished_ms == 0 && this->reply->isFinished())
            this->finished_ms = this->elapsed.elapsed();
    }

private:
    QNetworkReply *reply = nullptr;
    QElapsedTimer elapsed;
    qint64 finished_ms = 0;
};

HttpResult get(QCoreApplication &application, QNetworkAccessManager &network, const QUrl &url,
               int timeout_ms = 15000)
{
    PendingGet pending(network, url);
    QElapsedTimer elapsed;
    elapsed.start();
    while (!pending.finished() && elapsed.elapsed() < timeout_ms)
    {
        application.processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    pending.markIfFinished();
    return pending.take();
}

QUrl baseUrl(const Server &server)
{
    return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(MapCascadeMeshTestAccess::port(server)));
}

Aowis::Map::MapCascadeUpstream upstreamTo(const QString &name, const QUrl &url)
{
    Aowis::Map::MapCascadeUpstream result;
    result.name = name;
    result.url = url;
    return result;
}

Server::Config serverConfig(const QString &directory)
{
    Server::Config result;
    result.port = 0;
    result.cache_directory = directory;
    result.terrain_cache_directory = directory + QStringLiteral("/terrain");
    result.map_remote_fetch_enabled = true;
    result.terrain_remote_fetch_enabled = true;
    result.cascade.allow_direct_fetch = false;
    result.cascade.poor_internet_mode = false; // Exact bytes end to end.
    result.maximum_terrain_remote_workers = 2;
    return result;
}

// The activity entry of one upstream, section "map_tiles" or "terrain".
QJsonObject upstreamHealth(QCoreApplication &application, QNetworkAccessManager &network,
                           const Server &server, const QString &name, const QString &section)
{
    const HttpResult activity = get(application, network,
        QUrl(baseUrl(server).toString() + QStringLiteral("/upstream/v1/activity")));
    const QJsonArray upstreams = QJsonDocument::fromJson(activity.body).object()
        .value(QStringLiteral("upstreams")).toArray();
    for (const QJsonValue &value : upstreams)
        if (value.toObject().value(QStringLiteral("name")).toString() == name)
            return value.toObject().value(section).toObject();
    return {};
}

QString rasterRoute(const QString &provider, int zoom, int x, int y)
{
    return QStringLiteral("/%1/%2/%3/%4.png").arg(provider).arg(zoom).arg(x).arg(y);
}

QString rasterCachePath(const Server::Config &config, const QString &provider,
                        int zoom, int x, int y)
{
    return QDir(config.cache_directory).filePath(
        QStringLiteral("maptiles/%1/%2/%3/%4.png").arg(provider).arg(zoom).arg(x).arg(y));
}

const QByteArray stub_instance = QByteArrayLiteral("00000000-0000-4000-8000-0000000000e1");

// 1. Two hubs, each the other's upstream, receive requests for the same
// uncached raster tile at the same moment. Without loop-safe joins each
// download waits for the other until the 10 s transfer timeout.
bool mutualHubs(QCoreApplication &application, const QString &root)
{
    StubHttp origin;
    if (!require(origin.start(), "mutual: origin stub did not start")) return false;
    const QByteArray cold_png = samplePng(0x1234u);
    origin.delay_ms = 1500; // Keeps the first download in flight.
    origin.respond = [&cold_png](const QByteArray &) -> QByteArray
    {
        return httpResponse(200, "OK", "Content-Type: image/png\r\n", cold_png);
    };

    Server::Config a_config = serverConfig(root + QStringLiteral("/a"));
    Server::Config b_config = serverConfig(root + QStringLiteral("/b"));
    a_config.cascade.allow_direct_fetch = true;
    b_config.cascade.allow_direct_fetch = true;
    a_config.map_providers[0].url_template = origin.origin().toString() + QStringLiteral("/{z}/{x}/{y}.png");
    b_config.map_providers[0].url_template = a_config.map_providers[0].url_template;
    const QString provider = a_config.map_providers[0].id;

    // Ports are known only after start(): start each hub once to learn
    // its port, then restart both with the other as upstream.
    std::unique_ptr<Server> a = std::make_unique<Server>(a_config);
    std::unique_ptr<Server> b = std::make_unique<Server>(b_config);
    if (!require(a->start() && b->start(), "mutual: hubs did not start")) return false;
    const quint16 a_port = MapCascadeMeshTestAccess::port(*a);
    const quint16 b_port = MapCascadeMeshTestAccess::port(*b);
    a.reset();
    b.reset();
    a_config.port = a_port;
    b_config.port = b_port;
    a_config.cascade.upstreams = {upstreamTo(QStringLiteral("b"),
        QUrl(QStringLiteral("http://127.0.0.1:%1").arg(b_port)))};
    b_config.cascade.upstreams = {upstreamTo(QStringLiteral("a"),
        QUrl(QStringLiteral("http://127.0.0.1:%1").arg(a_port)))};
    a = std::make_unique<Server>(a_config);
    b = std::make_unique<Server>(b_config);
    if (!require(a->start() && b->start(), "mutual: hubs did not restart on their ports"))
        return false;

    // Each hub learns the other's instance ID from one ordinary transfer.
    const QByteArray warm_png = samplePng(0x5678u);
    if (!require(saveFile(rasterCachePath(b_config, provider, 10, 500, 300), warm_png) &&
                 saveFile(rasterCachePath(a_config, provider, 10, 501, 300), warm_png),
                 "mutual: could not seed warm-up tiles")) return false;
    QNetworkAccessManager network;
    const HttpResult warm_a = get(application, network,
        QUrl(baseUrl(*a).toString() + rasterRoute(provider, 10, 500, 300)));
    const HttpResult warm_b = get(application, network,
        QUrl(baseUrl(*b).toString() + rasterRoute(provider, 10, 501, 300)));
    if (!require(warm_a.code == 200 && warm_a.body == warm_png &&
                 warm_b.code == 200 && warm_b.body == warm_png && origin.requests.isEmpty(),
                 "mutual: warm-up transfers between the hubs failed")) return false;

    const QString cold = rasterRoute(provider, 10, 502, 300);
    PendingGet at_a(network, QUrl(baseUrl(*a).toString() + cold));
    PendingGet at_b(network, QUrl(baseUrl(*b).toString() + cold));
    QElapsedTimer elapsed;
    elapsed.start();
    while ((!at_a.finished() || !at_b.finished()) && elapsed.elapsed() < 20000)
    {
        application.processEvents(QEventLoop::AllEvents, 20);
        at_a.markIfFinished();
        at_b.markIfFinished();
        QThread::msleep(1);
    }
    at_a.markIfFinished();
    at_b.markIfFinished();
    const HttpResult result_a = at_a.take();
    const HttpResult result_b = at_b.take();
    std::fprintf(stderr, "mutual: A %d in %lld ms, B %d in %lld ms, origin requests %d\n",
                 result_a.code, static_cast<long long>(result_a.elapsed_ms),
                 result_b.code, static_cast<long long>(result_b.elapsed_ms),
                 int(origin.requests.size()));
    if (!require(result_a.code == 200 && result_a.body == cold_png &&
                 result_b.code == 200 && result_b.body == cold_png,
                 "mutual: a hub did not deliver the cold tile")) return false;
    if (!require(result_a.elapsed_ms < 8000 && result_b.elapsed_ms < 8000,
                 "mutual: the hubs waited for each other (join deadlock)")) return false;
    if (!require(origin.requests.size() >= 1 && origin.requests.size() <= 2,
                 "mutual: unexpected number of origin downloads")) return false;
    const QJsonObject b_seen_by_a = upstreamHealth(application, network, *a,
                                                   QStringLiteral("b"), QStringLiteral("map_tiles"));
    const QJsonObject a_seen_by_b = upstreamHealth(application, network, *b,
                                                   QStringLiteral("a"), QStringLiteral("map_tiles"));
    return require(b_seen_by_a.value(QStringLiteral("consecutive_failures")).toInt(-1) == 0 &&
                   a_seen_by_b.value(QStringLiteral("consecutive_failures")).toInt(-1) == 0 &&
                   b_seen_by_a.value(QStringLiteral("instance")).toString().toLatin1() ==
                       MapCascadeMeshTestAccess::identity(*b) &&
                   a_seen_by_b.value(QStringLiteral("instance")).toString().toLatin1() ==
                       MapCascadeMeshTestAccess::identity(*a),
                   "mutual: a refused join put the other hub on cooldown or no ID was learned");
}

// 2. Edge -> hub -> root, where the root needs 12 s for a terrain tile the
// hub does not have (longer than the 10 s request timeout of the old
// blocking cascade). Hub and edge poll with cache-first delivery.
bool slowHubTerrain(QCoreApplication &application, const QString &root)
{
    const Aowis::Map::TerrainTileAddress address{8, 131, 101};
    const QByteArray tile = terrainTile(address, 812.0f);
    if (!require(!tile.isEmpty(), "slow hub: could not encode terrain tile")) return false;
    StubHttp slow_root;
    if (!require(slow_root.start(), "slow hub: root stub did not start")) return false;
    QElapsedTimer since_first;
    slow_root.respond = [&since_first, &tile](const QByteArray &request) -> QByteArray
    {
        if (!request.contains("/terrain/v1/"))
            return httpResponse(404, "Not Found", "X-AOWIS-Instance: " + stub_instance + "\r\n", {});
        if (!since_first.isValid()) since_first.start();
        if (since_first.elapsed() < 12000)
            return httpResponse(202, "Accepted", "X-AOWIS-Instance: " + stub_instance +
                                "\r\nRetry-After: 1\r\n", "acquiring");
        return httpResponse(200, "OK",
            "Content-Type: application/vnd.aowis.terrain\r\nX-AOWIS-Terrain-Revision: " +
            QByteArray::number(Aowis::Map::TerrainNormalizationRevision) +
            "\r\nX-AOWIS-Instance: " + stub_instance + "\r\n", tile);
    };

    Server::Config hub_config = serverConfig(root + QStringLiteral("/hub"));
    hub_config.cascade.upstreams = {upstreamTo(QStringLiteral("root"), slow_root.origin())};
    std::unique_ptr<Server> hub = std::make_unique<Server>(hub_config);
    if (!require(hub->start(), "slow hub: hub did not start")) return false;
    Server::Config edge_config = serverConfig(root + QStringLiteral("/edge"));
    edge_config.cascade.upstreams = {upstreamTo(QStringLiteral("hub"), baseUrl(*hub))};
    std::unique_ptr<Server> edge = std::make_unique<Server>(edge_config);
    if (!require(edge->start(), "slow hub: edge did not start")) return false;

    QNetworkAccessManager network;
    const HttpResult result = get(application, network,
        QUrl(baseUrl(*edge).toString() +
             QStringLiteral("/terrain/v1/copernicus-glo30/8/131/101.aowterrain")), 40000);
    std::fprintf(stderr, "slow hub: edge %d in %lld ms, root polls %d%s%s\n", result.code,
                 static_cast<long long>(result.elapsed_ms),
                 slow_root.count("x-aowis-tile-delivery: cache-first"),
                 result.code == 200 ? "" : ", edge says: ",
                 result.code == 200 ? "" : result.body.left(300).constData());
    if (!require(result.code == 200 && result.body == tile,
                 "slow hub: edge did not deliver the slowly acquired terrain tile")) return false;
    if (!require(result.elapsed_ms >= 11000,
                 "slow hub: the slow root was not actually exercised")) return false;
    if (!require(slow_root.count("accept-encoding: identity") >= 3,
                 "slow hub: a non-poor-link downstream left Accept-Encoding to Qt"))
        return false;
    if (!require(slow_root.count("x-aowis-tile-delivery: cache-first") >= 3,
                 "slow hub: the hub did not poll its upstream with cache-first delivery"))
        return false;
    QFile hub_copy(MapCascadeMeshTestAccess::terrainPath(*hub, address));
    QFile edge_copy(MapCascadeMeshTestAccess::terrainPath(*edge, address));
    return require(hub_copy.open(QIODevice::ReadOnly) && hub_copy.readAll() == tile &&
                   edge_copy.open(QIODevice::ReadOnly) && edge_copy.readAll() == tile,
                   "slow hub: hub or edge did not cache the canonical tile");
}

// 3. A busy upstream (answered 503, "busy") must not trigger a direct
// Copernicus download, while an unreachable one (503 without MAP identity,
// e.g. from a reverse proxy) does fall back when direct fetching is allowed.
bool busyUpstream(QCoreApplication &application, const QString &root)
{
    StubHttp copernicus;
    if (!require(copernicus.start(), "busy: source stub did not start")) return false;
    copernicus.respond = [](const QByteArray &) -> QByteArray
    {
        return httpResponse(404, "Not Found", {}, {});
    };
    StubHttp busy;
    StubHttp proxy_error;
    if (!require(busy.start() && proxy_error.start(), "busy: upstream stubs did not start"))
        return false;
    busy.respond = [](const QByteArray &) -> QByteArray
    {
        return httpResponse(503, "Service Unavailable", "X-AOWIS-Instance: " + stub_instance +
                            "\r\nX-AOWIS-Upstream-Status: busy\r\nRetry-After: 1\r\n", "busy");
    };
    proxy_error.respond = [](const QByteArray &) -> QByteArray
    {
        return httpResponse(503, "Service Unavailable", {}, "proxy cannot reach MAP");
    };

    const QString terrain_route = QStringLiteral("/terrain/v1/copernicus-glo30/8/101/100.aowterrain");
    QNetworkAccessManager network;
    {
        Server::Config config = serverConfig(root + QStringLiteral("/busy"));
        config.cascade.allow_direct_fetch = true;
        config.terrain_integration_source_url = QUrl(copernicus.origin().toString() +
                                                     QStringLiteral("/source.tif"));
        config.cascade.upstreams = {upstreamTo(QStringLiteral("busy"), busy.origin())};
        Server server(config);
        if (!require(server.start(), "busy: downstream did not start")) return false;
        const HttpResult result = get(application, network,
            QUrl(baseUrl(server).toString() + terrain_route), 20000);
        // Each downstream also asks its upstream for the provider catalog;
        // only terrain requests matter here.
        if (!require(result.code != 200 && busy.count("/terrain/v1/") == 1 &&
                     copernicus.requests.isEmpty(),
                     "busy: a busy upstream led to a direct source download")) return false;
        const QJsonObject health = upstreamHealth(application, network, server,
                                                  QStringLiteral("busy"), QStringLiteral("terrain"));
        if (!require(health.value(QStringLiteral("consecutive_failures")).toInt(-1) == 0 &&
                     health.value(QStringLiteral("cooldown_remaining_ms")).toDouble(-1) == 0.0 &&
                     health.value(QStringLiteral("instance")).toString().toLatin1() == stub_instance,
                     "busy: a busy upstream was treated as unreachable")) return false;
    }
    {
        Server::Config config = serverConfig(root + QStringLiteral("/proxy-error"));
        config.cascade.allow_direct_fetch = true;
        config.terrain_integration_source_url = QUrl(copernicus.origin().toString() +
                                                     QStringLiteral("/source.tif"));
        config.cascade.upstreams = {upstreamTo(QStringLiteral("proxied"), proxy_error.origin())};
        Server server(config);
        if (!require(server.start(), "busy: control downstream did not start")) return false;
        get(application, network, QUrl(baseUrl(server).toString() + terrain_route), 30000);
        if (!require(proxy_error.count("/terrain/v1/") == 1 && !copernicus.requests.isEmpty(),
                     "busy: control did not fall back to the direct source")) return false;
        const QJsonObject health = upstreamHealth(application, network, server,
                                                  QStringLiteral("proxied"), QStringLiteral("terrain"));
        return require(health.value(QStringLiteral("consecutive_failures")).toInt(-1) == 1 &&
                       health.value(QStringLiteral("last_failure_unix_ms")).toDouble(0) > 0.0 &&
                       health.value(QStringLiteral("instance")).toString().isEmpty(),
                       "busy: an unreachable upstream was not counted as an outage");
    }
}

// 4. A dead primary (accepts TCP, never answers) and a live backup: the
// first tile pays the timeout once, the next tiles skip the primary at once,
// and the cooldown doubles on the next outage.
bool deadPrimary(QCoreApplication &application, const QString &root)
{
    StubHttp hole; // respond unset: never answers.
    if (!require(hole.start(), "dead: black hole did not start")) return false;
    Server::Config live_config = serverConfig(root + QStringLiteral("/live"));
    live_config.map_remote_fetch_enabled = false;
    std::unique_ptr<Server> live = std::make_unique<Server>(live_config);
    if (!require(live->start(), "dead: live upstream did not start")) return false;
    const QString provider = live_config.map_providers[0].id;
    const QByteArray png = samplePng(0x9abcu);
    for (int x = 600; x < 603; ++x)
        if (!require(saveFile(rasterCachePath(live_config, provider, 10, x, 300), png),
                     "dead: could not seed live upstream")) return false;

    Server::Config config = serverConfig(root + QStringLiteral("/site"));
    config.cascade.upstreams = {upstreamTo(QStringLiteral("dead"), hole.origin()),
                                upstreamTo(QStringLiteral("live"), baseUrl(*live))};
    Server site(config);
    if (!require(site.start(), "dead: site did not start")) return false;
    QNetworkAccessManager network;

    const HttpResult first = get(application, network,
        QUrl(baseUrl(site).toString() + rasterRoute(provider, 10, 600, 300)), 30000);
    const HttpResult second = get(application, network,
        QUrl(baseUrl(site).toString() + rasterRoute(provider, 10, 601, 300)), 30000);
    std::fprintf(stderr, "dead: first %d in %lld ms, second %d in %lld ms\n",
                 first.code, static_cast<long long>(first.elapsed_ms),
                 second.code, static_cast<long long>(second.elapsed_ms));
    if (!require(first.code == 200 && first.body == png && second.code == 200 &&
                 second.body == png, "dead: failover to the live upstream failed")) return false;
    // The site also asks the black hole for its provider catalog: count tiles only.
    if (!require(first.elapsed_ms >= 8000 && second.elapsed_ms < 3000 &&
                 hole.count(".png http/1.1") == 1,
                 "dead: the dead primary was asked again during its cooldown")) return false;
    QJsonObject dead = upstreamHealth(application, network, site,
                                      QStringLiteral("dead"), QStringLiteral("map_tiles"));
    const QJsonObject alive = upstreamHealth(application, network, site,
                                             QStringLiteral("live"), QStringLiteral("map_tiles"));
    if (!require(dead.value(QStringLiteral("consecutive_failures")).toInt(-1) == 1 &&
                 dead.value(QStringLiteral("cooldown_remaining_ms")).toDouble(0) > 0.0 &&
                 !dead.value(QStringLiteral("last_error")).toString().isEmpty() &&
                 alive.value(QStringLiteral("consecutive_failures")).toInt(-1) == 0 &&
                 alive.value(QStringLiteral("instance")).toString().toLatin1() ==
                     MapCascadeMeshTestAccess::identity(*live),
                 "dead: reported upstream state is wrong")) return false;

    // After the 5 s cooldown the primary is tried once more; a second
    // outage doubles the cooldown to 10 s.
    pump(application, int(dead.value(QStringLiteral("cooldown_remaining_ms")).toDouble()) + 200);
    const HttpResult third = get(application, network,
        QUrl(baseUrl(site).toString() + rasterRoute(provider, 10, 602, 300)), 30000);
    dead = upstreamHealth(application, network, site,
                          QStringLiteral("dead"), QStringLiteral("map_tiles"));
    return require(third.code == 200 && third.body == png && hole.count(".png http/1.1") == 2 &&
                   dead.value(QStringLiteral("consecutive_failures")).toInt(-1) == 2 &&
                   dead.value(QStringLiteral("cooldown_remaining_ms")).toDouble(0) > 5000.0,
                   "dead: the cooldown did not double on the second outage");
}
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QTemporaryDir directory;
    if (!require(directory.isValid(), "Unable to create temporary cache")) return 1;
    if (!mutualHubs(application, directory.path() + QStringLiteral("/mutual"))) return 1;
    if (!slowHubTerrain(application, directory.path() + QStringLiteral("/slow"))) return 1;
    if (!busyUpstream(application, directory.path() + QStringLiteral("/busy"))) return 1;
    if (!deadPrimary(application, directory.path() + QStringLiteral("/dead"))) return 1;
    return 0;
}
