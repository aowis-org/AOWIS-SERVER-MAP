#include <aowis/map/map_provider.h>
#include <aowis/map/maptiles.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QSemaphore>
#include <QThread>
#include <QtConcurrentRun>
#include <atomic>
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

// Test-only friend: exercise the same write-state synchronization used by the
// production worker without exposing test controls in the public map API.
class MapTilesCacheWriteTestAccess
{
public:
    static QSharedPointer<MapTiles::CacheWriteState> injectPendingWrite(
        MapTiles &tiles, const QString &key)
    {
        const QSharedPointer<MapTiles::CacheWriteState> state =
            QSharedPointer<MapTiles::CacheWriteState>::create();
        tiles.cache_write_states.insert(key, state);
        return state;
    }

    static bool persist(const QByteArray &bytes, const QString &path)
    {
        return MapTiles::saveMapTile(bytes, path);
    }
};

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
        // Invalid administrative input must neither block behind unrelated
        // reads nor cancel valid pending HTTP responses.
        require(async_reader.deleteTiles(provider_id, -1, 31, 31, 20, 20) == -1,
                "Malformed cache deletion must fail");
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

    // Deletion invalidates a pending disk read even when its worker has already
    // read the bytes but the queued completion has not run on the Qt thread.
    // It must not repopulate RAM or trigger another upstream download.
    {
        QTemporaryDir deletion_cache;
        require(deletion_cache.isValid(), "Cannot create deletion-race cache");
        const QString tile_path = QDir(deletion_cache.path()).filePath(
            QStringLiteral("maptiles/%1/6/31/20.png").arg(provider_id));
        writeFile(tile_path, QByteArray("deleted-tile-data"));
        MapTiles reader(deletion_cache.path(), 2, 8);
        reader.configureProviders(providers, {}, {});
        int ready_count = 0;
        int canceled_count = 0;
        QObject::connect(&reader, &MapTiles::tileReady, &reader,
            [&ready_count](const QString &key, const QByteArray &)
        {
            if (key == QStringLiteral("delete-race")) ++ready_count;
        });
        QObject::connect(&reader, &MapTiles::tileFailed, &reader,
            [&canceled_count](const QString &key, MapTiles::TileFailureReason reason)
        {
            if (key == QStringLiteral("delete-race") &&
                reason == MapTiles::TileFailureReason::Cancelled) ++canceled_count;
        });
        const MapTiles::TileRequestResult pending =
            reader.getTileForHttp(provider_id, 6, 31, 20, QStringLiteral("delete-race"));
        require(pending.status == MapTiles::TileRequestStatus::Pending,
                "Deletion race fixture should schedule a disk read");
        require(reader.deleteTiles(provider_id, 6, 31, 31, 20, 20) == 1,
                "Cannot delete tile during pending disk read");
        QEventLoop callback_loop;
        QTimer callback_timeout;
        callback_timeout.setSingleShot(true);
        QObject::connect(&callback_timeout, &QTimer::timeout, &callback_loop, &QEventLoop::quit);
        QObject::connect(&reader, &MapTiles::tileFailed, &callback_loop,
            [&callback_loop](const QString &key, MapTiles::TileFailureReason)
        {
            if (key == QStringLiteral("delete-race")) callback_loop.quit();
        });
        callback_timeout.start(5000);
        callback_loop.exec();
        require(canceled_count == 1 && ready_count == 0,
                "Invalidated disk read must complete once as canceled");
        require(!QFile::exists(tile_path), "Deleted tile unexpectedly reappeared");
        require(reader.upstreamActivity().active == 0 && reader.upstreamActivity().queued == 0,
                "Invalidated cache read restarted an upstream download");
    }

    // Force an overlapping write to be active while administrative deletion
    // begins. The background worker holds the exact same per-write mutex as
    // production persistence. It releases only after deletion invalidates it.
    // This is deterministic: no file sizes or arbitrary delay select the race.
    {
        QTemporaryDir race_cache;
        require(race_cache.isValid(), "Cannot create active-write race cache");
        const QString tile_path = QDir(race_cache.path()).filePath(
            QStringLiteral("maptiles/%1/6/31/20.png").arg(provider_id));
        writeFile(tile_path, QByteArray("pre-existing-data"));
        MapTiles writer(race_cache.path(), 2, 8);
        writer.configureProviders(providers, {}, {});
        const QString canonical_key = QStringLiteral("%1/6/31/20").arg(provider_id);
        const auto state = MapTilesCacheWriteTestAccess::injectPendingWrite(writer, canonical_key);
        QSemaphore worker_locked;
        std::atomic_bool release_observed{false};
        QFuture<void> active_write = QtConcurrent::run(
            [state, &worker_locked, &release_observed, tile_path]()
        {
            QMutexLocker locker(&state->mutex);
            worker_locked.release();
            // The deleting thread marks invalidated before acquiring mutex.
            // The worker checks the marker before committing, as production does.
            QElapsedTimer deadline;
            deadline.start();
            while (!state->invalidated.load(std::memory_order_acquire) &&
                   deadline.elapsed() < 5000)
                QThread::yieldCurrentThread();
            release_observed.store(
                state->invalidated.load(std::memory_order_acquire),
                std::memory_order_release);
            if (!state->invalidated.load(std::memory_order_acquire))
                MapTilesCacheWriteTestAccess::persist(QByteArray("late-data"), tile_path);
        });
        require(worker_locked.tryAcquire(1, 5000),
                "Active-write test worker did not acquire its mutex");
        require(writer.deleteTiles(provider_id, 6, 31, 31, 20, 20) == 1,
                "Deletion failed while overlapping write was active");
        active_write.waitForFinished();
        require(release_observed.load(std::memory_order_acquire),
                "Deletion did not invalidate active write before waiting");
        require(!QFile::exists(tile_path),
                "Active invalidated writer resurrected a deleted tile");
    }

    // A sparse oversized cache file must be rejected before any disk worker
    // tries to allocate its content. This verifies the read byte budget.
    {
        QTemporaryDir oversized_cache;
        require(oversized_cache.isValid(), "Cannot create oversized tile cache");
        const QString oversized_path = QDir(oversized_cache.path()).filePath(
            QStringLiteral("maptiles/%1/6/31/22.png").arg(provider_id));
        require(QDir().mkpath(QFileInfo(oversized_path).absolutePath()),
                "Cannot create oversized cache fixture directories");
        QFile oversized_file(oversized_path);
        require(oversized_file.open(QIODevice::WriteOnly), "Cannot create oversized cache file");
        require(oversized_file.resize(65LL * 1024 * 1024), "Cannot resize oversized cache file");
        oversized_file.close();
        MapTiles limited_reader(oversized_cache.path(), 2, 8);
        limited_reader.configureProviders(providers, {}, {});
        const MapTiles::TileRequestResult rejected = limited_reader.getTileForHttp(
            provider_id, 6, 31, 22, QStringLiteral("oversized-read"));
        require(rejected.status == MapTiles::TileRequestStatus::ServerBusy,
                "Oversized cold cache tile should be rejected by memory admission");
        require(limited_reader.upstreamActivity().active == 0,
                "Oversized cached tile must not schedule an upstream download");
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

    // Regression: selective cache deletion after asynchronous persistence must
    // not resurrect the tile through delayed worker completion, and must
    // preserve a neighboring cached tile.
    {
        MapTiles deletion_checker(online_cache.path(), 2, 8);
        deletion_checker.configureProviders(providers, {}, {});
        const QString downloaded_path = QDir(online_cache.path()).filePath(
            QStringLiteral("maptiles/%1/6/30/20.png").arg(provider_id));
        const QString neighbor_path = QDir(online_cache.path()).filePath(
            QStringLiteral("maptiles/%1/6/30/21.png").arg(provider_id));
        const QByteArray neighbor_data("neighbor-must-survive");
        writeFile(neighbor_path, neighbor_data);
        require(deletion_checker.deleteTiles(provider_id, 6, 30, 30, 20, 20) == 1,
                "Deleting asynchronously persisted tile failed");
        require(!QFile::exists(downloaded_path),
                "Deleted tile still exists immediately after deletion");
        QEventLoop deletion_drain_loop;
        QTimer::singleShot(30, &deletion_drain_loop, &QEventLoop::quit);
        deletion_drain_loop.exec();
        require(!QFile::exists(downloaded_path),
                "Late cache write recreated a deleted tile");
        QFile neighbor_file(neighbor_path);
        require(neighbor_file.open(QIODevice::ReadOnly) &&
                neighbor_file.readAll() == neighbor_data,
                "Selective deletion affected an unrelated tile");
    }

    std::cout << "Permanent raster cache reuse tests passed\n";
    return EXIT_SUCCESS;
}
