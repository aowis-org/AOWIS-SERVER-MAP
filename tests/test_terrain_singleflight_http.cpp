#include "server.h"
#include <aowis/map/terrain_data.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>

#include <iostream>
#include <utility>

// Inspect only the single-flight registry and server-owned terrain worker pool.
// Production builds do not expose any additional HTTP or configuration controls.
class TerrainSingleFlightTestAccess
{
public:
    static void blockWorker(Server &server, QSemaphore &entered, QSemaphore &release)
    {
        server.terrain_worker_pool.start([&entered, &release]()
        {
            entered.release();
            release.acquire();
        });
    }

    static void blockRemoteWorker(Server &server, QSemaphore &entered, QSemaphore &release)
    {
        server.terrain_remote_pool.start([&entered, &release]()
        {
            entered.release();
            release.acquire();
        });
    }

    static QString normalizedTilePath(const Server &server, const QString &dataset,
                                      const Aowis::Map::TerrainTileAddress &address)
    {
        return server.terrain_data->normalizedTilePath(dataset, address);
    }

    static quint16 port(const Server &server)
    {
        return server.tcp->serverPort();
    }

    static int waiterCount(const Server &server)
    {
        return server.pending_terrain_tile_count;
    }

    static int uniqueJobCount(const Server &server)
    {
        return server.pending_terrain_tiles.size();
    }

    static int gridJobs(const Server &server)
    {
        return server.pending_terrain_grids.size();
    }

    static int elevationJobs(const Server &server)
    {
        return server.pending_terrain_elevations.size();
    }

    static int admittedJobs(const Server &server)
    {
        return server.terrain_admission.count();
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary_directory;
    if (!temporary_directory.isValid())
    {
        std::cerr << "Failed to create temporary cache directory\n";
        return 1;
    }

    Server::Config config;
    config.port = 0;
    config.cache_directory = temporary_directory.path();
    config.terrain_cache_directory = temporary_directory.path() + "/terrain";
    config.map_remote_fetch_enabled = false;
    config.terrain_remote_fetch_enabled = false;
    config.maximum_terrain_workers = 1;
    config.maximum_terrain_jobs = 1;
    config.maximum_pending_requests = 6;
    Server server(config);
    if (!server.start())
    {
        std::cerr << "Failed to start test server\n";
        return 1;
    }

    QSemaphore worker_entered;
    QSemaphore release_worker;
    TerrainSingleFlightTestAccess::blockWorker(server, worker_entered, release_worker);
    if (!worker_entered.tryAcquire(1, 3000))
    {
        release_worker.release();
        std::cerr << "Failed to occupy terrain worker\n";
        return 1;
    }

    constexpr int request_count = 6;
    QNetworkAccessManager network;
    QList<QNetworkReply *> replies;
    const QUrl url(QStringLiteral("http://127.0.0.1:%1/terrain/v1/copernicus-glo30/5/11/12.aowterrain")
                       .arg(TerrainSingleFlightTestAccess::port(server)));
    for (int index = 0; index < request_count; ++index)
        replies.append(network.get(QNetworkRequest(url)));

    QElapsedTimer timer;
    timer.start();
    while (TerrainSingleFlightTestAccess::waiterCount(server) != request_count &&
           timer.elapsed() < 5000)
    {
        app.processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }

    const bool deduplicated =
        TerrainSingleFlightTestAccess::waiterCount(server) == request_count &&
        TerrainSingleFlightTestAccess::uniqueJobCount(server) == 1 &&
        TerrainSingleFlightTestAccess::admittedJobs(server) == 1;

    // The shared job has six HTTP waiters, but only one admission slot.
    // A seventh identical request and a new unique tile must both be
    // rejected immediately while the worker remains occupied.
    // Use a separate connection pool for overload probes. The first manager
    // already has six outstanding responses and may queue further requests
    // locally instead of sending them to the server.
    QNetworkAccessManager overload_network;
    QNetworkReply *excess_waiter = overload_network.get(QNetworkRequest(url));
    const QUrl other_url(QStringLiteral(
        "http://127.0.0.1:%1/terrain/v1/copernicus-glo30/5/11/13.aowterrain")
        .arg(TerrainSingleFlightTestAccess::port(server)));
    QNetworkReply *excess_job = overload_network.get(QNetworkRequest(other_url));
    timer.restart();
    while ((!excess_waiter->isFinished() || !excess_job->isFinished()) &&
           timer.elapsed() < 5000)
    {
        app.processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    const bool overload_rejected =
        excess_waiter->isFinished() && excess_job->isFinished() &&
        excess_waiter->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 503 &&
        excess_job->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 503 &&
        TerrainSingleFlightTestAccess::waiterCount(server) == request_count &&
        TerrainSingleFlightTestAccess::uniqueJobCount(server) == 1 &&
        TerrainSingleFlightTestAccess::admittedJobs(server) == 1;
    excess_waiter->deleteLater();
    excess_job->deleteLater();

    // Always release the blocked worker before destroying the server.
    release_worker.release();
    timer.restart();
    bool all_finished = false;
    while (timer.elapsed() < 5000)
    {
        app.processEvents(QEventLoop::AllEvents, 20);
        all_finished = true;
        for (QNetworkReply *reply : std::as_const(replies))
            all_finished = all_finished && reply->isFinished();
        if (all_finished)
            break;
        QThread::msleep(1);
    }

    bool response_ok = all_finished;
    for (QNetworkReply *reply : std::as_const(replies))
    {
        // Offline tiles are absent; all waiters must receive the same 404 result.
        response_ok = response_ok &&
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 404;
        reply->deleteLater();
    }

    // Once the first flight has completed, capacity must be reusable. A
    // rejected request must not leave a stuck registry entry or admission
    // reservation that prevents later unique tiles from being processed.
    const bool first_flight_cleared =
        TerrainSingleFlightTestAccess::waiterCount(server) == 0 &&
        TerrainSingleFlightTestAccess::uniqueJobCount(server) == 0 &&
        TerrainSingleFlightTestAccess::admittedJobs(server) == 0;

    QNetworkAccessManager recovery_network;
    QNetworkReply *recovery_reply = recovery_network.get(QNetworkRequest(other_url));
    timer.restart();
    while (!recovery_reply->isFinished() && timer.elapsed() < 5000)
    {
        app.processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    const bool recovered = recovery_reply->isFinished() &&
        recovery_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 404 &&
        TerrainSingleFlightTestAccess::waiterCount(server) == 0 &&
        TerrainSingleFlightTestAccess::uniqueJobCount(server) == 0 &&
        TerrainSingleFlightTestAccess::admittedJobs(server) == 0;
    recovery_reply->deleteLater();

    if (!deduplicated || !overload_rejected || !response_ok ||
        !first_flight_cleared || !recovered)
    {
        std::cerr << "Terrain tile single-flight HTTP regression failed: "
                  << "deduplicated=" << deduplicated
                  << " overload_rejected=" << overload_rejected
                  << " all_finished=" << all_finished
                  << " valid_responses=" << response_ok
                  << " first_flight_cleared=" << first_flight_cleared
                  << " recovered=" << recovered << '\n';
        return 1;
    }
    // A second wave verifies that elevation requests share the same bounded
    // admission and response budgets as tile requests.
    QSemaphore elevation_entered;
    QSemaphore elevation_release;
    TerrainSingleFlightTestAccess::blockWorker(server, elevation_entered, elevation_release);
    if (!elevation_entered.tryAcquire(1, 3000))
    {
        elevation_release.release();
        std::cerr << "Failed to occupy elevation worker\n";
        return 1;
    }

    QNetworkAccessManager elevation_network;
    QList<QNetworkReply *> elevation_replies;
    const QUrl elevation_url(QStringLiteral(
        "http://127.0.0.1:%1/terrain/v1/elevation?latitude=50&longitude=9")
        .arg(TerrainSingleFlightTestAccess::port(server)));
    for (int index = 0; index < request_count; ++index)
        elevation_replies.append(elevation_network.get(QNetworkRequest(elevation_url)));
    timer.restart();
    while (TerrainSingleFlightTestAccess::waiterCount(server) != request_count &&
           timer.elapsed() < 5000)
    {
        app.processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    const bool elevation_deduplicated =
        TerrainSingleFlightTestAccess::waiterCount(server) == request_count &&
        TerrainSingleFlightTestAccess::elevationJobs(server) == 1 &&
        TerrainSingleFlightTestAccess::admittedJobs(server) == 1;

    QNetworkAccessManager elevation_overload_network;
    QNetworkReply *elevation_excess =
        elevation_overload_network.get(QNetworkRequest(elevation_url));
    timer.restart();
    while (!elevation_excess->isFinished() && timer.elapsed() < 5000)
    {
        app.processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    const bool elevation_overload = elevation_excess->isFinished() &&
        elevation_excess->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 503;
    elevation_excess->deleteLater();
    elevation_release.release();

    timer.restart();
    bool elevation_finished = false;
    while (timer.elapsed() < 5000)
    {
        app.processEvents(QEventLoop::AllEvents, 20);
        elevation_finished = true;
        for (QNetworkReply *reply : std::as_const(elevation_replies))
            elevation_finished = elevation_finished && reply->isFinished();
        if (elevation_finished)
            break;
        QThread::msleep(1);
    }
    bool elevation_responses = elevation_finished;
    for (QNetworkReply *reply : std::as_const(elevation_replies))
    {
        elevation_responses = elevation_responses &&
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 404;
        reply->deleteLater();
    }
    const bool elevation_cleared =
        TerrainSingleFlightTestAccess::waiterCount(server) == 0 &&
        TerrainSingleFlightTestAccess::elevationJobs(server) == 0 &&
        TerrainSingleFlightTestAccess::admittedJobs(server) == 0;
    if (!elevation_deduplicated || !elevation_overload ||
        !elevation_responses || !elevation_cleared)
    {
        std::cerr << "Terrain elevation single-flight regression failed: "
                  << "deduplicated=" << elevation_deduplicated
                  << " overload=" << elevation_overload
                  << " responses=" << elevation_responses
                  << " cleared=" << elevation_cleared << '\n';
        return 1;
    }
    // Third wave: grid requests also use single-flight, admission, and the
    // same global pending-response budget. Equivalent numeric query strings
    // must map to one computation.
    QSemaphore grid_entered;
    QSemaphore grid_release;
    TerrainSingleFlightTestAccess::blockWorker(server, grid_entered, grid_release);
    if (!grid_entered.tryAcquire(1, 3000))
    {
        grid_release.release();
        std::cerr << "Failed to occupy grid worker\n";
        return 1;
    }

    QNetworkAccessManager grid_network;
    QList<QNetworkReply *> grid_replies;
    const QString grid_path = QStringLiteral(
        "/terrain/v1/analysis/grid?west=8&south=49&east=10&north=51&columns=8&rows=8");
    const QUrl grid_url(QStringLiteral("http://127.0.0.1:%1%2")
        .arg(TerrainSingleFlightTestAccess::port(server)).arg(grid_path));
    const QUrl equivalent_url(QStringLiteral(
        "http://127.0.0.1:%1/terrain/v1/analysis/grid?west=8.0&south=49.0&east=10.0&north=51.0&columns=08&rows=08")
        .arg(TerrainSingleFlightTestAccess::port(server)));
    for (int index = 0; index < request_count; ++index)
        grid_replies.append(grid_network.get(QNetworkRequest(
            index == request_count - 1 ? equivalent_url : grid_url)));
    timer.restart();
    while (TerrainSingleFlightTestAccess::waiterCount(server) != request_count &&
           timer.elapsed() < 5000)
    {
        app.processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    const bool grid_deduplicated =
        TerrainSingleFlightTestAccess::waiterCount(server) == request_count &&
        TerrainSingleFlightTestAccess::gridJobs(server) == 1 &&
        TerrainSingleFlightTestAccess::admittedJobs(server) == 1;

    QNetworkAccessManager grid_overload_network;
    QNetworkReply *grid_excess = grid_overload_network.get(QNetworkRequest(grid_url));
    timer.restart();
    while (!grid_excess->isFinished() && timer.elapsed() < 5000)
    {
        app.processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    const bool grid_overload = grid_excess->isFinished() &&
        grid_excess->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 503;
    grid_excess->deleteLater();
    grid_release.release();

    timer.restart();
    bool grid_finished = false;
    while (timer.elapsed() < 5000)
    {
        app.processEvents(QEventLoop::AllEvents, 20);
        grid_finished = true;
        for (QNetworkReply *reply : std::as_const(grid_replies))
            grid_finished = grid_finished && reply->isFinished();
        if (grid_finished)
            break;
        QThread::msleep(1);
    }
    bool grid_responses = grid_finished;
    for (QNetworkReply *reply : std::as_const(grid_replies))
    {
        // Offline-only test data produces not-found, shared by all waiters.
        grid_responses = grid_responses &&
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 404;
        reply->deleteLater();
    }
    const bool grid_cleared =
        TerrainSingleFlightTestAccess::waiterCount(server) == 0 &&
        TerrainSingleFlightTestAccess::gridJobs(server) == 0 &&
        TerrainSingleFlightTestAccess::admittedJobs(server) == 0;
    if (!grid_deduplicated || !grid_overload || !grid_responses || !grid_cleared)
    {
        std::cerr << "Terrain grid single-flight regression failed: "
                  << "deduplicated=" << grid_deduplicated
                  << " overload=" << grid_overload
                  << " responses=" << grid_responses
                  << " cleared=" << grid_cleared << '\n';
        return 1;
    }
    // Remote worker starvation must not delay an already-normalized tile.
    // Exercise the real HTTP route with remote fetching enabled and both
    // remote-capable threads occupied; no upstream server is involved.
    {
        QTemporaryDir isolated_cache;
        if (!isolated_cache.isValid())
        {
            std::cerr << "Failed to create isolation test cache\n";
            return 1;
        }

        Server::Config isolation_config;
        isolation_config.port = 0;
        isolation_config.cache_directory = isolated_cache.path();
        isolation_config.terrain_cache_directory = isolated_cache.path() + "/terrain";
        isolation_config.map_remote_fetch_enabled = false;
        isolation_config.terrain_remote_fetch_enabled = true;
        isolation_config.maximum_terrain_workers = 1;
        isolation_config.maximum_terrain_jobs = 4;
        isolation_config.maximum_pending_requests = 6;
        Server isolated_server(isolation_config);
        if (!isolated_server.start())
        {
            std::cerr << "Failed to start isolation test server\n";
            return 1;
        }

        Aowis::Map::TerrainTile cached_tile;
        cached_tile.dataset = QStringLiteral("copernicus-glo30");
        cached_tile.address.zoom = 5;
        cached_tile.address.x = 11;
        cached_tile.address.y = 12;
        cached_tile.nominal_resolution_m = 30.0;
        cached_tile.vertical_datum = Aowis::Map::TerrainVerticalDatum::Egm96;
        cached_tile.elevations_m.fill(42.0f, Aowis::Map::TerrainTileSampleCount);
        QString encoding_error;
        const QByteArray encoded = Aowis::Map::encodeTerrainTile(cached_tile, &encoding_error);
        const QString path = TerrainSingleFlightTestAccess::normalizedTilePath(isolated_server, 
            cached_tile.dataset, cached_tile.address);
        if (encoded.isEmpty() || !QDir().mkpath(QFileInfo(path).absolutePath()))
        {
            std::cerr << "Failed to prepare cached test tile\n";
            return 1;
        }
        QSaveFile output(path);
        if (!output.open(QIODevice::WriteOnly) ||
            output.write(encoded) != encoded.size() || !output.commit())
        {
            std::cerr << "Failed to save cached test tile\n";
            return 1;
        }

        QSemaphore remote_entered;
        QSemaphore remote_release;
        TerrainSingleFlightTestAccess::blockRemoteWorker(
            isolated_server, remote_entered, remote_release);
        TerrainSingleFlightTestAccess::blockRemoteWorker(
            isolated_server, remote_entered, remote_release);
        if (!remote_entered.tryAcquire(2, 3000))
        {
            remote_release.release(2);
            std::cerr << "Failed to occupy both remote workers\n";
            return 1;
        }

        QNetworkAccessManager isolation_network;
        const QUrl cached_url(QStringLiteral(
            "http://127.0.0.1:%1/terrain/v1/copernicus-glo30/5/11/12.aowterrain")
            .arg(TerrainSingleFlightTestAccess::port(isolated_server)));
        QNetworkReply *cached_reply = isolation_network.get(QNetworkRequest(cached_url));
        timer.restart();
        while (!cached_reply->isFinished() && timer.elapsed() < 3000)
        {
            app.processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(1);
        }
        const bool isolation_passed = cached_reply->isFinished() &&
            cached_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200 &&
            cached_reply->readAll() == encoded;
        // Always unblock before server destruction, even if the HTTP probe failed.
        remote_release.release(2);
        cached_reply->deleteLater();
        if (!isolation_passed)
        {
            std::cerr << "Cached terrain request was starved by remote workers\n";
            return 1;
        }
    }
    return 0;
}
