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
#include <QTemporaryDir>
#include <QThread>

#include <png.h>

#include <cstdio>
#include <memory>
#include <vector>

class MapCascadeChainTestAccess
{
public:
    static quint16 port(const Server &server) { return server.tcp->serverPort(); }
    static QByteArray identity(const Server &server)
    {
        return server.maptiles->cascadeInstanceId();
    }
    static quint64 jpegEncodingMicroseconds(const Server &server)
    {
        return server.raster_transfer.statistics().encoding_microseconds;
    }
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
    if (!condition) std::fprintf(stderr, "%s\n", message);
    return condition;
}

QByteArray samplePng()
{
    std::vector<unsigned char> pixels(256 * 256 * 3);
    quint32 state = 0x52a70123u;
    for (int y = 0; y < 256; ++y)
    {
        for (int x = 0; x < 256; ++x)
        {
            state = state * 1664525u + 1013904223u;
            const int noise = int(state >> 26) & 31;
            const size_t i = (size_t(y) * 256 + size_t(x)) * 3;
            pixels[i] = static_cast<unsigned char>((2 * x + noise) % 256);
            pixels[i + 1] = static_cast<unsigned char>((2 * y + noise) % 256);
            pixels[i + 2] = static_cast<unsigned char>((x + y + noise) % 256);
        }
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
    if (!png_image_write_to_memory(&image, encoded.data(), &length, 0,
                                   pixels.data(), 0, nullptr)) return {};
    encoded.resize(int(length));
    return encoded;
}

bool saveFile(const QString &path, const QByteArray &content)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly) &&
           file.write(content) == content.size() && file.flush();
}

QByteArray loadFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

struct HttpResult
{
    int code = 0;
    QByteArray content_type;
    QByteArray body;
};

HttpResult get(QCoreApplication &application, QNetworkAccessManager &network,
               const QUrl &url, const QByteArray &trace = {},
               const QByteArray &transfer = {}, const QByteArray &api_key = {})
{
    QNetworkRequest request(url);
    if (!trace.isEmpty()) request.setRawHeader("X-AOWIS-Cascade-Visited", trace);
    if (!transfer.isEmpty()) request.setRawHeader("X-AOWIS-Raster-Transfer", transfer);
    if (!api_key.isEmpty()) request.setRawHeader("X-API-Key", api_key);
    QNetworkReply *reply = network.get(request);
    QElapsedTimer elapsed;
    elapsed.start();
    while (!reply->isFinished() && elapsed.elapsed() < 12000)
    {
        application.processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    HttpResult response;
    if (reply->isFinished())
    {
        response.code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        response.content_type = reply->rawHeader("Content-Type");
        response.body = reply->readAll();
    }
    else
    {
        reply->abort();
    }
    reply->deleteLater();
    return response;
}

QUrl baseUrl(const Server &server)
{
    return QUrl(QStringLiteral("http://127.0.0.1:%1")
                .arg(MapCascadeChainTestAccess::port(server)));
}

Aowis::Map::MapCascadeUpstream upstream(const QString &name, const Server &server,
                                        const QByteArray &key)
{
    Aowis::Map::MapCascadeUpstream result;
    result.name = name;
    result.url = baseUrl(server);
    result.api_key = key;
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
    result.cascade.poor_internet_mode = true;
    result.cascade.jpeg_quality = 75;
    result.maximum_terrain_remote_workers = 2;
    return result;
}
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QTemporaryDir directory;
    if (!require(directory.isValid(), "Unable to create temporary cache")) return 1;

    Server::Config root_config = serverConfig(directory.path() + QStringLiteral("/root"));
    root_config.map_remote_fetch_enabled = false;
    root_config.terrain_remote_fetch_enabled = false;
    root_config.api_key = QByteArrayLiteral("root-read-key");
    std::unique_ptr<Server> root = std::make_unique<Server>(root_config);
    if (!require(root->start(), "Unable to start root MAP server")) return 1;

    const QString provider = root_config.map_providers.first().id;
    const int zoom = 12;
    const int x = 2100;
    const int y = 1390;
    const QString raster_relative = QStringLiteral("maptiles/%1/%2/%3/%4.png")
        .arg(provider).arg(zoom).arg(x).arg(y);
    const QByteArray png = samplePng();
    if (!require(png.size() > 12 * 1024 && saveFile(
        QDir(root_config.cache_directory).filePath(raster_relative), png),
        "Unable to seed root PNG cache")) return 1;

    const Aowis::Map::TerrainTileAddress address{8, 130, 101};
    Aowis::Map::TerrainTile tile;
    tile.address = address;
    tile.dataset = QStringLiteral("copernicus-glo30");
    tile.vertical_datum = Aowis::Map::TerrainVerticalDatum::Egm2008;
    tile.nominal_resolution_m = 30;
    tile.elevations_m.fill(420.0f, Aowis::Map::TerrainTileSampleCount);
    const QByteArray terrain = Aowis::Map::encodeTerrainTile(tile);
    if (!require(!terrain.isEmpty() && saveFile(
        MapCascadeChainTestAccess::terrainPath(*root, address), terrain),
        "Unable to seed root terrain cache")) return 1;

    Server::Config middle_config = serverConfig(directory.path() + QStringLiteral("/middle"));
    middle_config.api_key = QByteArrayLiteral("middle-read-key");
    middle_config.cascade.upstreams.append(upstream(QStringLiteral("root"), *root,
                                                    root_config.api_key));
    std::unique_ptr<Server> middle = std::make_unique<Server>(middle_config);
    if (!require(middle->start(), "Unable to start middle MAP server")) return 1;

    Server::Config edge_config = serverConfig(directory.path() + QStringLiteral("/edge"));
    edge_config.cascade.upstreams.append(upstream(QStringLiteral("middle"), *middle,
                                                  middle_config.api_key));
    std::unique_ptr<Server> edge = std::make_unique<Server>(edge_config);
    if (!require(edge->start(), "Unable to start edge MAP server")) return 1;

    QNetworkAccessManager network;
    const QString raster_route = QStringLiteral("/%1/%2/%3/%4.png")
        .arg(provider).arg(zoom).arg(x).arg(y);
    const QString terrain_route = QStringLiteral(
        "/terrain/v1/copernicus-glo30/8/130/101.aowterrain");
    const QByteArray reference_trace = QByteArrayLiteral("00000000-0000-4000-8000-000000000001");
    const HttpResult jpeg_reference = get(application, network,
        QUrl(baseUrl(*root).toString() + raster_route), reference_trace,
        QByteArrayLiteral("adaptive-jpeg;quality=75"), root_config.api_key);
    if (!require(jpeg_reference.code == 200 &&
                 jpeg_reference.content_type.startsWith("image/jpeg") &&
                 jpeg_reference.body.size() < png.size(),
                 "Root did not produce a smaller JPEG reference")) return 1;
    const quint64 jpeg_encode_us =
        MapCascadeChainTestAccess::jpegEncodingMicroseconds(*root);

    const HttpResult raster_result = get(application, network,
        QUrl(baseUrl(*edge).toString() + raster_route));
    if (!require(raster_result.code == 200 &&
                 raster_result.content_type.startsWith("image/jpeg") &&
                 raster_result.body == jpeg_reference.body,
                 "Three-server raster cascade re-encoded JPEG or returned wrong bytes")) return 1;
    if (!require(loadFile(QDir(middle_config.cache_directory).filePath(raster_relative)) ==
                     jpeg_reference.body &&
                 loadFile(QDir(edge_config.cache_directory).filePath(raster_relative)) ==
                     jpeg_reference.body,
                 "Middle or edge failed to persist the exact transferred JPEG")) return 1;

    const HttpResult terrain_result = get(application, network,
        QUrl(baseUrl(*edge).toString() + terrain_route));
    if (!require(terrain_result.code == 200 && terrain_result.body == terrain &&
                 loadFile(MapCascadeChainTestAccess::terrainPath(*middle, address)) == terrain &&
                 loadFile(MapCascadeChainTestAccess::terrainPath(*edge, address)) == terrain,
                 "Three-server terrain cascade did not preserve canonical bytes")) return 1;

    // A cyclic trace must be rejected even if the root has the requested
    // tile in its cache. If the loop guard fails, it would return HTTP 200.
    const QString second_raster_relative = QStringLiteral("maptiles/%1/12/2100/1391.png")
        .arg(provider);
    Aowis::Map::TerrainTile second_tile = tile;
    second_tile.address = Aowis::Map::TerrainTileAddress{8, 130, 102};
    const QByteArray second_terrain = Aowis::Map::encodeTerrainTile(second_tile);
    if (!require(saveFile(QDir(root_config.cache_directory).filePath(second_raster_relative), png) &&
                 saveFile(MapCascadeChainTestAccess::terrainPath(*root, second_tile.address),
                          second_terrain),
                 "Unable to seed cache for cycle protection test")) return 1;
    const QByteArray cyclic_trace = MapCascadeChainTestAccess::identity(*root);
    const QString second_raster_route = QStringLiteral("/%1/12/2100/1391.png").arg(provider);
    const QString second_terrain_route = QStringLiteral(
        "/terrain/v1/copernicus-glo30/8/130/102.aowterrain");
    const HttpResult loop_raster = get(application, network,
        QUrl(baseUrl(*edge).toString() + second_raster_route), cyclic_trace);
    const HttpResult loop_terrain = get(application, network,
        QUrl(baseUrl(*edge).toString() + second_terrain_route), cyclic_trace);
    if (!require(loop_raster.code != 200 && loop_terrain.code != 200 &&
                 !QFileInfo::exists(QDir(edge_config.cache_directory).filePath(
                     second_raster_relative)) &&
                 !QFileInfo::exists(MapCascadeChainTestAccess::terrainPath(
                     *edge, second_tile.address)),
                 "Cyclic MAP request bypassed the visited-server guard or poisoned cache"))
        return 1;

    // After a rejected trace, valid requests for those very same cached root
    // tiles must still succeed. This catches incorrect permanent cooldowns.
    QElapsedTimer cooldown;
    cooldown.start();
    while (cooldown.elapsed() < 5200)
    {
        application.processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    const HttpResult recovered_raster = get(application, network,
        QUrl(baseUrl(*edge).toString() + second_raster_route));
    const HttpResult recovered_terrain = get(application, network,
        QUrl(baseUrl(*edge).toString() + second_terrain_route));
    if (!require(recovered_raster.code == 200 && recovered_raster.body == jpeg_reference.body &&
                 recovered_terrain.code == 200 && recovered_terrain.body == second_terrain,
                 "MAP cascade failed to recover after a cyclic request")) return 1;

    // Shut down both upstream MAP servers, then restart the edge with its
    // original acquisition configuration. Warm tiles must work without WAN.
    edge.reset();
    middle.reset();
    root.reset();
    Server restarted(edge_config);
    if (!require(restarted.start(), "Unable to restart edge in offline mode")) return 1;
    const HttpResult offline_raster = get(application, network,
        QUrl(baseUrl(restarted).toString() + raster_route));
    const HttpResult offline_terrain = get(application, network,
        QUrl(baseUrl(restarted).toString() + terrain_route));
    if (!require(offline_raster.code == 200 && offline_raster.body == jpeg_reference.body &&
                 offline_terrain.code == 200 && offline_terrain.body == terrain,
                 "Offline edge failed to serve its persistent raster and terrain caches")) return 1;
    // Misses must fail, not contact the original provider, when both upstream
    // servers are down and allow_direct_fetch=false.
    const HttpResult offline_miss = get(application, network,
        QUrl(baseUrl(restarted).toString() +
             QStringLiteral("/%1/12/2100/1392.png").arg(provider)));
    if (!require(offline_miss.code != 200 && offline_miss.code != 0,
                 "Offline edge returned an unexpected tile or stalled on a cache miss")) return 1;

    QElapsedTimer zstd_timer;
    zstd_timer.start();
    const std::optional<QByteArray> zstd = Aowis::Map::compressTerrainTileForTransfer(terrain);
    const qint64 zstd_encode_us = zstd_timer.nsecsElapsed() / 1000;
    if (!require(zstd.has_value() && zstd->size() < terrain.size(),
                 "Synthetic terrain did not compress")) return 1;
    std::printf("Synthetic WAN sizes (not representative of real-world tiles): "
                "PNG=%lld JPEG=%lld terrain=%lld zstd=%lld bytes; "
                "JPEG encode=%llu us zstd encode=%lld us\n",
                static_cast<long long>(png.size()),
                static_cast<long long>(jpeg_reference.body.size()),
                static_cast<long long>(terrain.size()),
                static_cast<long long>(zstd->size()),
                static_cast<unsigned long long>(jpeg_encode_us),
                static_cast<long long>(zstd_encode_us));
    return 0;
}
