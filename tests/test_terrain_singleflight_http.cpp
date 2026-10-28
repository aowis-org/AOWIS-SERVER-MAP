#include "server.h"

#include <QCoreApplication>
#include <QElapsedTimer>
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
    return 0;
}
