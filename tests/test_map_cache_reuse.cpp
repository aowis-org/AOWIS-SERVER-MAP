#include <aowis/map/map_provider.h>
#include <aowis/map/maptiles.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdlib>
#include <iostream>

namespace
{
void require(bool condition, const char *description)
{
    if (!condition)
    {
        std::cerr << description << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void writeFile(const QString &path, const QByteArray &data)
{
    require(QDir().mkpath(QFileInfo(path).absolutePath()), "Cannot create cache fixture directories");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "Cannot open cache fixture");
    require(file.write(data) == data.size(), "Cannot write cache fixture");
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir cache;
    require(cache.isValid(), "Cannot create cache test directory");

    QVector<Aowis::Map::MapProvider> providers = Aowis::Map::defaultMapProviders();
    require(!providers.isEmpty(), "Missing provider defaults");
    const QString provider_id = providers.first().id;
    const QString old_tile_path = QDir(cache.path()).filePath(
        QStringLiteral("maptiles/%1/6/31/20.png").arg(provider_id));
    const QByteArray old_bytes("previously-cached-map-tile");
    writeFile(old_tile_path, old_bytes);
    writeFile(old_tile_path + QStringLiteral(".http.json"),
              R"({"etag":"old-tag","last_modified":"Wed, 21 Oct 2015 07:28:00 GMT","fresh_until":1,"must_revalidate":true})");
    QFile old_tile(old_tile_path);
    require(old_tile.open(QIODevice::ReadWrite), "Cannot open old cache fixture");
    require(old_tile.setFileTime(QDateTime::currentDateTimeUtc().addYears(-5),
                                 QFileDevice::FileModificationTime),
            "Cannot age cached tile");
    old_tile.close();

    // Use a loopback-only upstream even if the test fails; tests must never
    // contact public tile servers.
    providers.first().url_template = QStringLiteral("http://127.0.0.1:1/{z}/{x}/{y}.png");

    // HTTP cold-cache path must complete asynchronously, coalescing concurrent
    // disk reads and preserving each request's response key.
    {
        MapTiles async_reader(cache.path(), 2, 8);
        async_reader.configureProviders(providers, {}, {});
        QByteArray first_result;
        QByteArray second_result;
        QEventLoop read_loop;
        QTimer read_timeout;
        read_timeout.setSingleShot(true);
        QObject::connect(&read_timeout, &QTimer::timeout, &read_loop, &QEventLoop::quit);
        QObject::connect(&async_reader, &MapTiles::tileReady, &read_loop,
            [&read_loop, &first_result, &second_result](const QString &key, const QByteArray &data)
        {
            if (key == QStringLiteral("async-one")) first_result = data;
            if (key == QStringLiteral("async-two")) second_result = data;
            if (!first_result.isEmpty() && !second_result.isEmpty()) read_loop.quit();
        });
        const MapTiles::TileRequestResult first_async =
            async_reader.getTileForHttp(provider_id, 6, 31, 20, QStringLiteral("async-one"));
        const MapTiles::TileRequestResult second_async =
            async_reader.getTileForHttp(provider_id, 6, 31, 20, QStringLiteral("async-two"));
        require(first_async.status == MapTiles::TileRequestStatus::Pending &&
                second_async.status == MapTiles::TileRequestStatus::Pending,
                "HTTP cold disk reads should be pending without blocking the caller");
        read_timeout.start(5000);
        read_loop.exec();
        require(first_result == old_bytes && second_result == old_bytes,
                "Asynchronous cold disk read did not complete both requests");
        const MapTiles::TileRequestResult warm =
            async_reader.getTileForHttp(provider_id, 6, 31, 20, QStringLiteral("async-warm"));
        require(warm.status == MapTiles::TileRequestStatus::Ready && warm.data == old_bytes,
                "Asynchronous disk completion did not warm the RAM cache");
        require(async_reader.upstreamActivity().active == 0 &&
                async_reader.upstreamActivity().queued == 0,
                "Asynchronous disk hit unexpectedly contacted upstream");
    }

    // A cold disk hit with expired metadata must not even schedule a download.
    MapTiles existing(cache.path(), 2, 8);
    existing.configureProviders(providers, {}, {});
    const MapTiles::TileRequestResult disk_hit =
        existing.getTile(provider_id, 6, 31, 20, QStringLiteral("old-disk"));
    require(disk_hit.status == MapTiles::TileRequestStatus::Ready && disk_hit.data == old_bytes,
            "An expired disk tile should be returned without revalidation");
    require(existing.upstreamActivity().active == 0 && existing.upstreamActivity().queued == 0,
            "Cold cache hit unexpectedly scheduled an upstream download");
    const MapTiles::TileRequestResult memory_hit =
        existing.getTile(provider_id, 6, 31, 20, QStringLiteral("old-memory"));
    require(memory_hit.status == MapTiles::TileRequestStatus::Ready && memory_hit.data == old_bytes,
            "An expired memory tile should be returned without revalidation");
    require(existing.upstreamActivity().active == 0 && existing.upstreamActivity().queued == 0,
            "Memory cache hit unexpectedly scheduled an upstream download");
    // A warm RAM hit must not depend on the disk file remaining present.
    // This also exercises the optimized lookup path before providerCachePath().
    require(QFile::remove(old_tile_path), "Cannot remove warm-cache fixture");
    const MapTiles::TileRequestResult memory_after_disk_removed =
        existing.getTile(provider_id, 6, 31, 20, QStringLiteral("warm-without-disk"));
    require(memory_after_disk_removed.status == MapTiles::TileRequestStatus::Ready &&
            memory_after_disk_removed.data == old_bytes,
            "Warm memory cache should serve a tile without disk access");

    const QString no_metadata_path = QDir(cache.path()).filePath(
        QStringLiteral("maptiles/%1/6/31/21.png").arg(provider_id));
    writeFile(no_metadata_path, old_bytes);
    QFile no_metadata_tile(no_metadata_path);
    require(no_metadata_tile.open(QIODevice::ReadWrite), "Cannot open metadata-free tile");
    require(no_metadata_tile.setFileTime(QDateTime::currentDateTimeUtc().addYears(-5),
                                         QFileDevice::FileModificationTime),
            "Cannot age metadata-free tile");
    no_metadata_tile.close();
    const MapTiles::TileRequestResult no_metadata_hit =
        existing.getTile(provider_id, 6, 31, 21, QStringLiteral("old-no-metadata"));
    require(no_metadata_hit.status == MapTiles::TileRequestStatus::Ready &&
            no_metadata_hit.data == old_bytes,
            "Old cache tile without metadata must not be refreshed");
    require(existing.upstreamActivity().active == 0 && existing.upstreamActivity().queued == 0,
            "Old cache tile without metadata triggered a download");

    // Download a missing tile from a local HTTP server. Its zero max-age must
    // not cause a second network request, even after restarting MapTiles.
    QTcpServer upstream;
    require(upstream.listen(QHostAddress::LocalHost), "Cannot listen on local test port");
    int request_count = 0;
    const QByteArray image = QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVQIHWP4z8DwHwAFgAI/ScL/nwAAAABJRU5ErkJggg==");
    QObject::connect(&upstream, &QTcpServer::newConnection, &upstream, [&upstream, &request_count, &image]()
    {
        while (upstream.hasPendingConnections())
        {
            QTcpSocket *socket = upstream.nextPendingConnection();
            const auto respond = [socket, &request_count, &image]()
            {
                if (!socket->bytesAvailable() || socket->property("responded").toBool())
                    return;
                socket->setProperty("responded", true);
                socket->readAll();
                ++request_count;
                const QByteArray headers =
                    QByteArray("HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nCache-Control: max-age=0\r\nETag: \"test\"\r\nContent-Length: ") +
                    QByteArray::number(image.size()) + QByteArray("\r\nConnection: close\r\n\r\n");
                socket->write(headers + image);
                socket->disconnectFromHost();
            };
            QObject::connect(socket, &QTcpSocket::readyRead, socket, respond);
            QTimer::singleShot(0, socket, respond);
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });

    providers.first().url_template = QStringLiteral("http://127.0.0.1:%1/{z}/{x}/{y}.png")
        .arg(upstream.serverPort());
    // Use a fresh cache root for the loopback provider. The earlier fixtures
    // established a fingerprint for a different upstream URL, which correctly
    // routes this provider into a variant-specific directory.
    QTemporaryDir online_cache;
    require(online_cache.isValid(), "Cannot create online cache test directory");
    const QString requested_key = QStringLiteral("missing-first-time");
    {
        MapTiles online(online_cache.path(), 2, 8);
        online.configureProviders(providers, {}, {});
        QByteArray received;
        bool failed = false;
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(&online, &MapTiles::tileReady, &loop,
            [&loop, &received, &requested_key](const QString &key, const QByteArray &data)
        {
            if (key == requested_key)
            {
                received = data;
                loop.quit();
            }
        });
        QObject::connect(&online, &MapTiles::tileFailed, &loop,
            [&loop, &failed, &requested_key](const QString &key, MapTiles::TileFailureReason)
        {
            if (key == requested_key)
            {
                failed = true;
                loop.quit();
            }
        });
        const MapTiles::TileRequestResult first =
            online.getTile(provider_id, 6, 30, 20, requested_key);
        require(first.status == MapTiles::TileRequestStatus::Pending,
                "Missing tile should schedule first upstream download");
        timeout.start(5000);
        loop.exec();
        require(!failed && received == image, "New tile did not download successfully");
        // tileReady is emitted only after the asynchronous cache-write task
        // completes. This also catches regressions that publish a response
        // before an admitted write has committed its file.
        const QString downloaded_path = QDir(online_cache.path()).filePath(
            QStringLiteral("maptiles/%1/6/30/20.png").arg(provider_id));
        QFile persisted_tile(downloaded_path);
        require(persisted_tile.open(QIODevice::ReadOnly),
                "tileReady fired before asynchronous disk persistence completed");
        require(persisted_tile.readAll() == image,
                "Asynchronously persisted tile differs from downloaded bytes");
        require(request_count == 1, "New tile must be downloaded exactly once");
        const MapTiles::TileRequestResult second =
            online.getTile(provider_id, 6, 30, 20, QStringLiteral("in-memory"));
        require(second.status == MapTiles::TileRequestStatus::Ready && second.data == image,
                "Newly downloaded tile not reused from memory");
        require(online.upstreamActivity().active == 0 && online.upstreamActivity().queued == 0,
                "Cached tile scheduled a refresh after max-age=0");
    }

    MapTiles after_restart(online_cache.path(), 2, 8);
    after_restart.configureProviders(providers, {}, {});
    const MapTiles::TileRequestResult after_restart_hit =
        after_restart.getTile(provider_id, 6, 30, 20, QStringLiteral("after-restart"));
    require(after_restart_hit.status == MapTiles::TileRequestStatus::Ready &&
            after_restart_hit.data == image, "Downloaded tile not reused after restart");
    require(after_restart.upstreamActivity().active == 0 && after_restart.upstreamActivity().queued == 0,
            "Disk cache unexpectedly scheduled a refresh after restart");
    require(request_count == 1, "Repeat access unexpectedly downloaded another tile");

    std::cout << "Permanent raster cache reuse tests passed\n";
    return EXIT_SUCCESS;
}
