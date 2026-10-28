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

    static QFuture<QHttpServerResponse> submitTileDirect(
        Server &server, const QString &dataset, int zoom, int x, int y)
    {
        return server.submitTerrainTile(dataset, zoom, x, y);
    }

    static QFuture<QHttpServerResponse> submitElevationDirect(Server &server)
    {
        return server.submitTerrainElevation(
            QStringLiteral("copernicus-glo30"), 48.0, 11.0, std::nullopt);
    }

    static QFuture<QHttpServerResponse> submitGridDirect(Server &server)
    {
        Aowis::Map::TerrainAnalysisGridRequest request;
        request.dataset = QStringLiteral("copernicus-glo30");
        request.bounds.west_deg = 10.0;
        request.bounds.south_deg = 48.0;
        request.bounds.east_deg = 10.1;
        request.bounds.north_deg = 48.1;
        request.columns = 2;
        request.rows = 2;
        return server.submitTerrainGrid(request);
    }

    static bool reserveRemoteJob(Server &server)
    {
        if (!server.terrain_remote_admission.tryAcquire())
            return false;
        if (!server.terrain_admission.tryAcquire())
        {
            server.terrain_remote_admission.release();
            return false;
        }
        return true;
    }

    static void releaseRemoteJob(Server &server)
    {
        server.terrain_admission.release();
        server.terrain_remote_admission.release();
    }

    static int remoteWorkerLimit(const Server &server)
    {
        return server.terrain_remote_pool.maxThreadCount();
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

    static int remoteAdmittedJobs(const Server &server)
    {
        return server.terrain_remote_admission.count();
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

    if (TerrainSingleFlightTestAccess::remoteWorkerLimit(server) != 1)
    {
        std::cerr << "Remote worker limit must respect the shared job admission cap\n";
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
        isolation_config.maximum_terrain_remote_workers = 2;
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

        // Fill only the remote admission allowance. The cached tile must
        // still enter through the shared admission budget and finish on the
        // local pool, while a new remote miss must get HTTP 503 immediately.
        const bool first_remote_slot =
            TerrainSingleFlightTestAccess::reserveRemoteJob(isolated_server);
        const bool second_remote_slot = first_remote_slot &&
            TerrainSingleFlightTestAccess::reserveRemoteJob(isolated_server);
        if (!second_remote_slot)
        {
            if (first_remote_slot)
                TerrainSingleFlightTestAccess::releaseRemoteJob(isolated_server);
            remote_release.release(2);
            std::cerr << "Failed to reserve remote admission slots\n";
            return 1;
        }

        QNetworkAccessManager remote_probe_network;
        const QUrl remote_url(QStringLiteral(
            "http://127.0.0.1:%1/terrain/v1/copernicus-glo30/5/11/13.aowterrain")
            .arg(TerrainSingleFlightTestAccess::port(isolated_server)));
        QNetworkReply *remote_reply =
            remote_probe_network.get(QNetworkRequest(remote_url));
        timer.restart();
        while (!remote_reply->isFinished() && timer.elapsed() < 3000)
        {
            app.processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(1);
        }
        const bool remote_rejected = remote_reply->isFinished() &&
            remote_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 503;
        remote_reply->deleteLater();

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
        TerrainSingleFlightTestAccess::releaseRemoteJob(isolated_server);
        TerrainSingleFlightTestAccess::releaseRemoteJob(isolated_server);
        cached_reply->deleteLater();
        if (!isolation_passed || !remote_rejected ||
            TerrainSingleFlightTestAccess::admittedJobs(isolated_server) != 0)
        {
            std::cerr << "Remote overload did not preserve cached terrain admission: "
                      << "cached=" << isolation_passed
                      << " rejected=" << remote_rejected << '\n';
            return 1;
        }
    }
    // A completed acquisition must release its remote slot before a blocked
    // CPU continuation runs, while retaining global admission until completion.
    // Disable providers to make acquisition deterministic and fully offline.
    {
        QTemporaryDir handoff_cache;
        if (!handoff_cache.isValid())
        {
            std::cerr << "Failed to create handoff test cache\n";
            return 1;
        }
        Server::Config handoff_config;
        handoff_config.port = 0;
        handoff_config.cache_directory = handoff_cache.path();
        handoff_config.terrain_cache_directory = handoff_cache.path() + "/terrain";
        handoff_config.map_remote_fetch_enabled = false;
        handoff_config.terrain_remote_fetch_enabled = true;
        handoff_config.copernicus_enabled = false;
        handoff_config.maximum_terrain_workers = 1;
        handoff_config.maximum_terrain_remote_workers = 1;
        handoff_config.maximum_terrain_jobs = 2;
        Server handoff_server(handoff_config);
        if (!handoff_server.start())
        {
            std::cerr << "Failed to start handoff test server\n";
            return 1;
        }
        QSemaphore cpu_entered;
        QSemaphore cpu_release;
        TerrainSingleFlightTestAccess::blockWorker(handoff_server, cpu_entered, cpu_release);
        if (!cpu_entered.tryAcquire(1, 3000))
        {
            cpu_release.release();
            std::cerr << "Failed to block CPU worker for handoff test\n";
            return 1;
        }
        QNetworkAccessManager handoff_network;
        const QUrl handoff_url(QStringLiteral(
            "http://127.0.0.1:%1/terrain/v1/copernicus-glo30/5/11/12.aowterrain")
            .arg(TerrainSingleFlightTestAccess::port(handoff_server)));
        QNetworkReply *handoff_reply = handoff_network.get(QNetworkRequest(handoff_url));
        timer.restart();
        bool acquired_and_queued = false;
        while (timer.elapsed() < 3000)
        {
            app.processEvents(QEventLoop::AllEvents, 20);
            acquired_and_queued = TerrainSingleFlightTestAccess::waiterCount(handoff_server) == 1 &&
                TerrainSingleFlightTestAccess::remoteAdmittedJobs(handoff_server) == 0 &&
                TerrainSingleFlightTestAccess::admittedJobs(handoff_server) == 1 &&
                !handoff_reply->isFinished();
            if (acquired_and_queued)
                break;
            QThread::msleep(1);
        }
        cpu_release.release();
        timer.restart();
        while (!handoff_reply->isFinished() && timer.elapsed() < 3000)
        {
            app.processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(1);
        }
        const bool finished = handoff_reply->isFinished();
        const int response_code = finished
            ? handoff_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() : 0;
        handoff_reply->deleteLater();
        app.processEvents(QEventLoop::AllEvents, 20);
        if (!acquired_and_queued || !finished || response_code == 200 ||
            TerrainSingleFlightTestAccess::admittedJobs(handoff_server) != 0 ||
            TerrainSingleFlightTestAccess::remoteAdmittedJobs(handoff_server) != 0)
        {
            std::cerr << "Terrain acquisition-to-CPU handoff did not release budgets correctly: "
                      << "queued=" << acquired_and_queued
                      << " finished=" << finished
                      << " status=" << response_code << '\n';
            return 1;
        }
    }
    // A grid acquisition must release remote admission before its CPU-only
    // sampling stage runs; the global job and byte reservations survive the
    // handoff. No providers are enabled, so this remains an offline test.
    {
        QTemporaryDir grid_handoff_cache;
        if (!grid_handoff_cache.isValid())
            return 1;
        Server::Config grid_handoff_config;
        grid_handoff_config.port = 0;
        grid_handoff_config.cache_directory = grid_handoff_cache.path();
        grid_handoff_config.terrain_cache_directory = grid_handoff_cache.path() + "/terrain";
        grid_handoff_config.map_remote_fetch_enabled = false;
        grid_handoff_config.terrain_remote_fetch_enabled = true;
        grid_handoff_config.copernicus_enabled = false;
        grid_handoff_config.maximum_terrain_workers = 1;
        grid_handoff_config.maximum_terrain_remote_workers = 1;
        grid_handoff_config.maximum_terrain_jobs = 2;
        Server grid_handoff_server(grid_handoff_config);
        if (!grid_handoff_server.start())
            return 1;
        QSemaphore grid_cpu_entered;
        QSemaphore grid_cpu_release;
        TerrainSingleFlightTestAccess::blockWorker(
            grid_handoff_server, grid_cpu_entered, grid_cpu_release);
        if (!grid_cpu_entered.tryAcquire(1, 3000))
        {
            grid_cpu_release.release();
            return 1;
        }
        QNetworkAccessManager grid_handoff_network;
        const QUrl grid_handoff_url(QStringLiteral(
            "http://127.0.0.1:%1/terrain/v1/analysis/grid?west=8&south=49&east=9&north=50&columns=2&rows=2")
            .arg(TerrainSingleFlightTestAccess::port(grid_handoff_server)));
        QNetworkReply *grid_handoff_reply = grid_handoff_network.get(
            QNetworkRequest(grid_handoff_url));
        timer.restart();
        bool grid_queued = false;
        while (timer.elapsed() < 3000)
        {
            app.processEvents(QEventLoop::AllEvents, 20);
            grid_queued = TerrainSingleFlightTestAccess::gridJobs(grid_handoff_server) == 1 &&
                TerrainSingleFlightTestAccess::remoteAdmittedJobs(grid_handoff_server) == 0 &&
                TerrainSingleFlightTestAccess::admittedJobs(grid_handoff_server) == 1 &&
                !grid_handoff_reply->isFinished();
            if (grid_queued)
                break;
            QThread::msleep(1);
        }
        grid_cpu_release.release();
        timer.restart();
        while (!grid_handoff_reply->isFinished() && timer.elapsed() < 3000)
        {
            app.processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(1);
        }
        const bool grid_finished = grid_handoff_reply->isFinished();
        const int grid_status = grid_finished
            ? grid_handoff_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()
            : 0;
        grid_handoff_reply->deleteLater();
        app.processEvents(QEventLoop::AllEvents, 20);
        if (!grid_queued || !grid_finished || grid_status == 200 ||
            TerrainSingleFlightTestAccess::admittedJobs(grid_handoff_server) != 0 ||
            TerrainSingleFlightTestAccess::remoteAdmittedJobs(grid_handoff_server) != 0)
        {
            std::cerr << "Grid acquisition-to-CPU handoff regression failed: queued="
                      << grid_queued << " finished=" << grid_finished
                      << " status=" << grid_status << '\n';
            return 1;
        }
    }
    // Elevation acquisition releases remote admission before sampling starts.
    // With no providers enabled, both stages are deterministic and offline.
    {
        QTemporaryDir elevation_handoff_cache;
        if (!elevation_handoff_cache.isValid())
            return 1;
        Server::Config elevation_config;
        elevation_config.port = 0;
        elevation_config.cache_directory = elevation_handoff_cache.path();
        elevation_config.terrain_cache_directory = elevation_handoff_cache.path() + "/terrain";
        elevation_config.map_remote_fetch_enabled = false;
        elevation_config.terrain_remote_fetch_enabled = true;
        elevation_config.copernicus_enabled = false;
        elevation_config.maximum_terrain_workers = 1;
        elevation_config.maximum_terrain_remote_workers = 1;
        elevation_config.maximum_terrain_jobs = 2;
        Server elevation_server(elevation_config);
        if (!elevation_server.start())
            return 1;
        QSemaphore elevation_cpu_entered;
        QSemaphore elevation_cpu_release;
        TerrainSingleFlightTestAccess::blockWorker(
            elevation_server, elevation_cpu_entered, elevation_cpu_release);
        if (!elevation_cpu_entered.tryAcquire(1, 3000))
        {
            elevation_cpu_release.release();
            return 1;
        }
        QNetworkAccessManager elevation_network;
        const QUrl elevation_url(QStringLiteral(
            "http://127.0.0.1:%1/terrain/v1/elevation?dataset=copernicus-glo30&latitude=49&longitude=8")
            .arg(TerrainSingleFlightTestAccess::port(elevation_server)));
        QNetworkReply *elevation_reply = elevation_network.get(QNetworkRequest(elevation_url));
        timer.restart();
        bool elevation_queued = false;
        while (timer.elapsed() < 3000)
        {
            app.processEvents(QEventLoop::AllEvents, 20);
            elevation_queued = TerrainSingleFlightTestAccess::elevationJobs(elevation_server) == 1 &&
                TerrainSingleFlightTestAccess::remoteAdmittedJobs(elevation_server) == 0 &&
                TerrainSingleFlightTestAccess::admittedJobs(elevation_server) == 1 &&
                !elevation_reply->isFinished();
            if (elevation_queued)
                break;
            QThread::msleep(1);
        }
        elevation_cpu_release.release();
        timer.restart();
        while (!elevation_reply->isFinished() && timer.elapsed() < 3000)
        {
            app.processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(1);
        }
        const bool elevation_finished = elevation_reply->isFinished();
        const int elevation_status = elevation_finished
            ? elevation_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() : 0;
        elevation_reply->deleteLater();
        app.processEvents(QEventLoop::AllEvents, 20);
        if (!elevation_queued || !elevation_finished || elevation_status == 200 ||
            TerrainSingleFlightTestAccess::admittedJobs(elevation_server) != 0 ||
            TerrainSingleFlightTestAccess::remoteAdmittedJobs(elevation_server) != 0)
        {
            std::cerr << "Elevation acquisition-to-CPU handoff regression failed: queued="
                      << elevation_queued << " finished=" << elevation_finished
                      << " status=" << elevation_status << '\n';
            return 1;
        }
    }
    // Destroying the server while a post-acquisition CPU continuation is
    // queued must finish its future even when the queued completion callback
    // has not been dispatched by the event loop.
    {
        QTemporaryDir shutdown_cache;
        if (!shutdown_cache.isValid())
            return 1;
        Server::Config shutdown_config;
        shutdown_config.port = 0;
        shutdown_config.cache_directory = shutdown_cache.path();
        shutdown_config.terrain_cache_directory = shutdown_cache.path() + "/terrain";
        shutdown_config.map_remote_fetch_enabled = false;
        shutdown_config.terrain_remote_fetch_enabled = true;
        shutdown_config.copernicus_enabled = false;
        shutdown_config.maximum_terrain_workers = 1;
        shutdown_config.maximum_terrain_remote_workers = 1;
        shutdown_config.maximum_terrain_jobs = 2;
        Server *shutdown_server = new Server(shutdown_config);
        QSemaphore shutdown_cpu_entered;
        QSemaphore shutdown_cpu_release;
        TerrainSingleFlightTestAccess::blockWorker(
            *shutdown_server, shutdown_cpu_entered, shutdown_cpu_release);
        if (!shutdown_cpu_entered.tryAcquire(1, 3000))
        {
            shutdown_cpu_release.release();
            delete shutdown_server;
            std::cerr << "Failed to occupy CPU worker in shutdown regression\n";
            return 1;
        }
        QFuture<QHttpServerResponse> shutdown_future =
            TerrainSingleFlightTestAccess::submitTileDirect(
                *shutdown_server, QStringLiteral("copernicus-glo30"), 5, 11, 12);
        timer.restart();
        bool shutdown_queued = false;
        while (timer.elapsed() < 3000)
        {
            app.processEvents(QEventLoop::AllEvents, 20);
            shutdown_queued =
                TerrainSingleFlightTestAccess::remoteAdmittedJobs(*shutdown_server) == 0 &&
                TerrainSingleFlightTestAccess::admittedJobs(*shutdown_server) == 1 &&
                !shutdown_future.isFinished();
            if (shutdown_queued)
                break;
            QThread::msleep(1);
        }
        // No event-loop pumping after this release: the CPU task can finish,
        // but its queued callback must be drained safely by server destruction.
        shutdown_cpu_release.release();
        delete shutdown_server;
        if (!shutdown_queued || !shutdown_future.isFinished() ||
            shutdown_future.resultCount() != 1)
        {
            std::cerr << "Shutdown did not drain queued terrain handoff future: queued="
                      << shutdown_queued << " finished=" << shutdown_future.isFinished()
                      << " results=" << shutdown_future.resultCount() << '\n';
            return 1;
        }
    }
    // Elevation and grid continuations have the same shutdown contract as
    // tiles: remote acquisition may finish while CPU work is still queued.
    for (int kind = 0; kind < 2; ++kind)
    {
        QTemporaryDir cache;
        if (!cache.isValid())
            return 1;
        Server::Config config;
        config.port = 0;
        config.cache_directory = cache.path();
        config.terrain_cache_directory = cache.path() + "/terrain";
        config.map_remote_fetch_enabled = false;
        config.terrain_remote_fetch_enabled = true;
        config.copernicus_enabled = false;
        config.maximum_terrain_workers = 1;
        config.maximum_terrain_remote_workers = 1;
        config.maximum_terrain_jobs = 2;
        Server *server = new Server(config);
        QSemaphore entered;
        QSemaphore release;
        TerrainSingleFlightTestAccess::blockWorker(*server, entered, release);
        if (!entered.tryAcquire(1, 3000))
        {
            release.release();
            delete server;
            std::cerr << "Failed to occupy CPU worker for handoff shutdown test\n";
            return 1;
        }
        QFuture<QHttpServerResponse> future = kind == 0
            ? TerrainSingleFlightTestAccess::submitElevationDirect(*server)
            : TerrainSingleFlightTestAccess::submitGridDirect(*server);
        timer.restart();
        bool queued = false;
        while (timer.elapsed() < 3000)
        {
            app.processEvents(QEventLoop::AllEvents, 20);
            queued = TerrainSingleFlightTestAccess::remoteAdmittedJobs(*server) == 0 &&
                TerrainSingleFlightTestAccess::admittedJobs(*server) == 1 &&
                (kind == 0 ? TerrainSingleFlightTestAccess::elevationJobs(*server) == 1
                           : TerrainSingleFlightTestAccess::gridJobs(*server) == 1) &&
                !future.isFinished();
            if (queued)
                break;
            QThread::msleep(1);
        }
        release.release();
        delete server;
        if (!queued || !future.isFinished() || future.resultCount() != 1)
        {
            std::cerr << "Shutdown did not drain "
                      << (kind == 0 ? "elevation" : "grid")
                      << " handoff: queued=" << queued
                      << " finished=" << future.isFinished()
                      << " results=" << future.resultCount() << '\n';
            return 1;
        }
    }
    return 0;
}
