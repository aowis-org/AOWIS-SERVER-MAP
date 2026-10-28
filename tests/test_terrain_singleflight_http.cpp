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
    config.maximum_pending_requests = 16;
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

    if (!deduplicated || !response_ok ||
        TerrainSingleFlightTestAccess::waiterCount(server) != 0 ||
        TerrainSingleFlightTestAccess::uniqueJobCount(server) != 0)
    {
        std::cerr << "Terrain tile single-flight HTTP regression failed: "
                  << "deduplicated=" << deduplicated
                  << " all_finished=" << all_finished
                  << " valid_responses=" << response_ok << '\n';
        return 1;
    }
    return 0;
}
