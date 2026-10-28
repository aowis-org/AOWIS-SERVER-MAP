#include "../src/lib/copernicus_resume_integration_test.h"
#include "../src/lib/copernicus_resume_state.h"

#include <QCoreApplication>
#include <QtConcurrent>
#include <QThreadPool>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>

#include <future>
#include <iostream>
#include <thread>
#include <chrono>

namespace
{
bool require(bool success, const char *message)
{
    if (!success)
        std::cerr << message << '\n';
    return success;
}

struct MockUpstream
{
    quint16 port = 0;
    std::future<bool> outcome;
    std::thread thread;
};

MockUpstream launchUpstream(int step, const QByteArray &payload)
{
    std::promise<quint16> ready;
    std::promise<bool> completed;
    MockUpstream mock;
    mock.outcome = completed.get_future();
    std::future<quint16> port = ready.get_future();
    mock.thread = std::thread([step, payload, ready = std::move(ready),
                               completed = std::move(completed)]() mutable
    {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0))
        {
            ready.set_value(0);
            completed.set_value(false);
            return;
        }
        ready.set_value(server.serverPort());
        if (!server.waitForNewConnection(7000))
        {
            completed.set_value(false);
            return;
        }
        QTcpSocket *socket = server.nextPendingConnection();
        QByteArray request;
        while (!request.contains("\r\n\r\n") && request.size() < 16384)
        {
            if (!socket->waitForReadyRead(7000))
                break;
            request += socket->readAll();
        }
        const qint64 offset = step == 0 ? 0 : (step == 1 ? 300 : 700);
        const QByteArray expected_range = "Range: bytes=" + QByteArray::number(offset) + "-";
        const QByteArray normalized_request = request.toLower();
        const bool range_ok = step == 0 ? !normalized_request.contains("range:") :
            normalized_request.contains(expected_range.toLower()) &&
            normalized_request.contains("if-range: \"v1\"");
        const qint64 bytes_to_send = step == 0 ? 300 : (step == 1 ? 400 : 300);
        QByteArray header = step == 0 ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 206 Partial Content\r\n";
        header += "Content-Length: " + QByteArray::number(payload.size() - offset) + "\r\n";
        header += "ETag: \"v1\"\r\n";
        if (step != 0)
            header += "Content-Range: bytes " + QByteArray::number(offset) + "-999/1000\r\n";
        header += "Connection: close\r\n\r\n";
        socket->write(header + payload.mid(offset, bytes_to_send));
        socket->flush();
        socket->waitForBytesWritten(5000);
        socket->disconnectFromHost();
        socket->waitForDisconnected(2000);
        completed.set_value(range_ok);
    });
    mock.port = port.get();
    return mock;
}


// Exercise a real Range request against servers that send a full replacement
// or a mismatched partial response. A nonmatching 206 must never be published.
MockUpstream launchRecoveryUpstream(int scenario, const QByteArray &payload)
{
    std::promise<quint16> ready;
    std::promise<bool> completed;
    MockUpstream mock;
    mock.outcome = completed.get_future();
    std::future<quint16> port = ready.get_future();
    mock.thread = std::thread([scenario, payload, ready = std::move(ready),
                               completed = std::move(completed)]() mutable
    {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0))
        {
            ready.set_value(0);
            completed.set_value(false);
            return;
        }
        ready.set_value(server.serverPort());
        if (!server.waitForNewConnection(7000))
        {
            completed.set_value(false);
            return;
        }
        QTcpSocket *socket = server.nextPendingConnection();
        QByteArray request;
        while (!request.contains("\r\n\r\n") && request.size() < 16384)
        {
            if (!socket->waitForReadyRead(7000))
                break;
            request += socket->readAll();
        }
        const QByteArray normalized = request.toLower();
        const bool headers_ok = normalized.contains("range: bytes=300-") &&
                                normalized.contains("if-range: \"v1\"");
        QByteArray response;
        if (scenario == 0 || scenario == 1)
        {
            // 200 with a different validator models a changed upstream entity;
            // 200 with the original validator models a server ignoring Range.
            response = "HTTP/1.1 200 OK\r\nContent-Length: 1000\r\nETag: ";
            response += scenario == 0 ? "\"v2\"\r\n" : "\"v1\"\r\n";
            response += "Connection: close\r\n\r\n";
            response += payload;
        }
        else
        {
            // Wrong entity validator in 206: data may be internally complete,
            // but must not be appended to a prefix from a different entity.
            response = "HTTP/1.1 206 Partial Content\r\n"
                       "Content-Length: 700\r\n"
                       "Content-Range: bytes 300-999/1000\r\n"
                       "ETag: \"v2\"\r\nConnection: close\r\n\r\n";
            response += payload.mid(300);
        }
        socket->write(response);
        socket->flush();
        socket->waitForBytesWritten(5000);
        socket->disconnectFromHost();
        socket->waitForDisconnected(2000);
        completed.set_value(headers_ok);
    });
    mock.port = port.get();
    return mock;
}

// Prove real bytes arriving steadily can exceed the old two-minute-style
// absolute cutoff without being misclassified as an idle connection. Also
// prove that a peer which stops making progress is eventually aborted.
bool verifySlowNetwork(const QString &root, int scenario)
{
    const bool stall = scenario == 1;
    const bool absolute_deadline = scenario == 2;
    const QString directory = QDir(root).filePath(
        stall ? "stalled-transfer" : (absolute_deadline ? "deadline-transfer" : "slow-transfer"));
    std::promise<quint16> ready;
    std::promise<bool> completed;
    std::future<quint16> port = ready.get_future();
    std::future<bool> outcome = completed.get_future();
    std::thread server_thread([stall, ready = std::move(ready),
                               completed = std::move(completed)]() mutable
    {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0))
        {
            ready.set_value(0);
            completed.set_value(false);
            return;
        }
        ready.set_value(server.serverPort());
        if (!server.waitForNewConnection(5000))
        {
            completed.set_value(false);
            return;
        }
        QTcpSocket *socket = server.nextPendingConnection();
        QByteArray request;
        while (!request.contains("\r\n\r\n") && request.size() < 16384)
        {
            if (!socket->waitForReadyRead(5000))
                break;
            request += socket->readAll();
        }
        const bool request_ok = request.startsWith("GET ") &&
                                !request.toLower().contains("range:");
        socket->write("HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n"
                      "ETag: \"slow-v1\"\r\nConnection: close\r\n\r\n");
        socket->flush();
        if (stall)
        {
            socket->write(QByteArray(100, 'S'));
            socket->flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(1400));
        }
        else
        {
            for (int chunk = 0; chunk < 20; ++chunk)
            {
                if (socket->state() != QAbstractSocket::ConnectedState)
                    break;
                socket->write(QByteArray(50, 'S'));
                socket->flush();
                std::this_thread::sleep_for(std::chrono::milliseconds(80));
            }
        }
        socket->disconnectFromHost();
        socket->waitForDisconnected(1000);
        completed.set_value(request_ok);
    });
    const quint16 upstream_port = port.get();
    if (upstream_port == 0)
    {
        server_thread.join();
        return require(false, "Slow-network mock server failed to bind");
    }
    QString error;
    const bool fetched = Aowis::Map::Internal::fetchCopernicusSourceForIntegrationTest(
        directory, QUrl(QStringLiteral("http://127.0.0.1:%1/source.tif").arg(upstream_port)),
        &error, 500, stall ? 300 : 500, absolute_deadline ? 650 : 4000);
    const bool request_ok = outcome.get();
    server_thread.join();
    const QString source = QDir(directory).filePath(
        "copernicus/glo30/Copernicus_DSM_COG_10_N50_00_E008_00_DEM.tif");
    if (!require(request_ok, "Slow-network upstream did not receive a full GET") ||
        !require(fetched == (scenario == 0), "Unexpected slow-network transfer result"))
        return false;
    if (scenario != 0)
        return require(!QFile::exists(source),
                       "Timed-out transfer published incomplete source");
    QFile published(source);
    return require(published.open(QIODevice::ReadOnly) &&
                   published.readAll() == QByteArray(1000, 'S'),
                   "Slow but progressing transfer did not publish complete source");
}

// Cancellation must abort an active upstream reply promptly, without waiting
// for the inactivity/absolute deadline or publishing an incomplete GeoTIFF.
bool verifyActiveCancellation(const QString &root)
{
    const QString directory = QDir(root).filePath("active-cancellation");
    std::promise<quint16> ready;
    std::promise<bool> completed;
    std::future<quint16> port = ready.get_future();
    std::future<bool> outcome = completed.get_future();
    std::thread upstream([ready = std::move(ready),
                          completed = std::move(completed)]() mutable
    {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0))
        {
            ready.set_value(0);
            completed.set_value(false);
            return;
        }
        ready.set_value(server.serverPort());
        if (!server.waitForNewConnection(5000))
        {
            completed.set_value(false);
            return;
        }
        QTcpSocket *socket = server.nextPendingConnection();
        QByteArray request;
        while (!request.contains("\r\n\r\n") && request.size() < 16384)
        {
            if (!socket->waitForReadyRead(5000))
                break;
            request += socket->readAll();
        }
        socket->write("HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n"
                      "ETag: \"cancel-v1\"\r\nConnection: close\r\n\r\n");
        socket->write(QByteArray(100, 'C'));
        socket->flush();
        // The client should close the connection upon cancellation, long before
        // its otherwise generous timeout. A bounded wait prevents test hangs.
        const bool disconnected = socket->waitForDisconnected(2000);
        completed.set_value(request.startsWith("GET ") && disconnected);
    });
    const quint16 upstream_port = port.get();
    if (upstream_port == 0)
    {
        upstream.join();
        return require(false, "Cancellation mock could not bind");
    }
    QString error;
    const bool fetched = Aowis::Map::Internal::fetchCopernicusSourceForIntegrationTest(
        directory, QUrl(QStringLiteral("http://127.0.0.1:%1/source.tif").arg(upstream_port)),
        &error, 3000, 3000, 5000, 150);
    const bool disconnected = outcome.get();
    upstream.join();
    const QString source = QDir(directory).filePath(
        "copernicus/glo30/Copernicus_DSM_COG_10_N50_00_E008_00_DEM.tif");
    if (!require(disconnected, "Active source cancellation did not close upstream connection") ||
        !require(!fetched && error.contains("cancel", Qt::CaseInsensitive),
                 "Active source cancellation did not report cancellation") ||
        !require(!QFile::exists(source), "Canceled download published incomplete source"))
        return false;

    // A canceled transfer must not poison later acquisition. Respond with a
    // complete replacement even if the next request includes Range/If-Range.
    std::promise<quint16> recovery_ready;
    std::promise<bool> recovery_completed;
    std::future<quint16> recovery_port = recovery_ready.get_future();
    std::future<bool> recovery_outcome = recovery_completed.get_future();
    const QByteArray complete_payload(1000, 'R');
    std::thread recovery([ready = std::move(recovery_ready),
                          completed = std::move(recovery_completed),
                          complete_payload]() mutable
    {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0))
        {
            ready.set_value(0);
            completed.set_value(false);
            return;
        }
        ready.set_value(server.serverPort());
        if (!server.waitForNewConnection(5000))
        {
            completed.set_value(false);
            return;
        }
        QTcpSocket *socket = server.nextPendingConnection();
        QByteArray request;
        while (!request.contains("\r\n\r\n") && request.size() < 16384)
        {
            if (!socket->waitForReadyRead(5000))
                break;
            request += socket->readAll();
        }
        const QByteArray response =
            "HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n"
            "ETag: \"recovered-v2\"\r\nConnection: close\r\n\r\n" + complete_payload;
        const qint64 accepted = socket->write(response);
        socket->flush();
        while (socket->bytesToWrite() > 0 &&
               socket->state() == QAbstractSocket::ConnectedState)
            socket->waitForBytesWritten(5000);
        const bool sent = accepted == response.size() && socket->bytesToWrite() == 0;
        socket->disconnectFromHost();
        if (socket->state() != QAbstractSocket::UnconnectedState)
            socket->waitForDisconnected(2000);
        completed.set_value(request.startsWith("GET ") && sent);
    });
    const quint16 second_port = recovery_port.get();
    if (!require(second_port != 0, "Recovery mock could not bind"))
    {
        recovery.join();
        return false;
    }
    error.clear();
    const bool recovered = Aowis::Map::Internal::fetchCopernicusSourceForIntegrationTest(
        directory, QUrl(QStringLiteral("http://127.0.0.1:%1/source.tif").arg(second_port)),
        &error);
    const bool recovery_sent = recovery_outcome.get();
    recovery.join();
    QFile published(source);
    return require(recovery_sent && recovered,
                   QStringLiteral("Download did not recover after cancellation: upstream_sent=%1, error=%2")
                       .arg(recovery_sent).arg(error).toUtf8().constData()) &&
           require(published.open(QIODevice::ReadOnly) &&
                   published.readAll() == complete_payload,
                   "Recovery did not atomically publish the complete replacement");
}

// Exercise independent real HTTP transfers concurrently: one peer holds its
// response open until cancellation while another publishes a complete source.
// This tests the downloader transport, not the Server's CPU scheduling.
bool verifyConcurrentStallAndCompletion(const QString &root)
{
    const QString stalled_directory = QDir(root).filePath("parallel-stalled");
    const QString healthy_directory = QDir(root).filePath("parallel-healthy");
    const QString relative_source =
        QStringLiteral("copernicus/glo30/Copernicus_DSM_COG_10_N50_00_E008_00_DEM.tif");
    const QByteArray payload(1000, 'H');

    std::promise<quint16> stalled_ready;
    std::promise<bool> stalled_done;
    std::future<quint16> stalled_port = stalled_ready.get_future();
    std::future<bool> stalled_result = stalled_done.get_future();
    std::thread stalled_upstream([ready = std::move(stalled_ready),
                                  completed = std::move(stalled_done)]() mutable
    {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0))
        {
            ready.set_value(0);
            completed.set_value(false);
            return;
        }
        ready.set_value(server.serverPort());
        if (!server.waitForNewConnection(5000))
        {
            completed.set_value(false);
            return;
        }
        QTcpSocket *socket = server.nextPendingConnection();
        QByteArray request;
        while (!request.contains("\r\n\r\n") && request.size() < 16384)
        {
            if (!socket->waitForReadyRead(5000))
                break;
            request += socket->readAll();
        }
        socket->write("HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n"
                      "ETag: \"stalled-v1\"\r\nConnection: close\r\n\r\n");
        socket->write(QByteArray(100, 'S'));
        socket->flush();
        const bool disconnected = socket->waitForDisconnected(3000);
        completed.set_value(request.startsWith("GET ") && disconnected);
    });

    std::promise<quint16> healthy_ready;
    std::promise<bool> healthy_done;
    std::future<quint16> healthy_port = healthy_ready.get_future();
    std::future<bool> healthy_result = healthy_done.get_future();
    std::thread healthy_upstream([ready = std::move(healthy_ready),
                                  completed = std::move(healthy_done), payload]() mutable
    {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0))
        {
            ready.set_value(0);
            completed.set_value(false);
            return;
        }
        ready.set_value(server.serverPort());
        if (!server.waitForNewConnection(5000))
        {
            completed.set_value(false);
            return;
        }
        QTcpSocket *socket = server.nextPendingConnection();
        QByteArray request;
        while (!request.contains("\r\n\r\n") && request.size() < 16384)
        {
            if (!socket->waitForReadyRead(5000))
                break;
            request += socket->readAll();
        }
        const QByteArray response =
            "HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n"
            "ETag: \"healthy-v1\"\r\nConnection: close\r\n\r\n" + payload;
        const bool accepted = socket->write(response) == response.size();
        socket->flush();
        while (socket->bytesToWrite() > 0 && socket->state() == QAbstractSocket::ConnectedState)
        {
            if (!socket->waitForBytesWritten(3000))
                break;
        }
        const bool sent = accepted && socket->bytesToWrite() == 0;
        socket->disconnectFromHost();
        if (socket->state() != QAbstractSocket::UnconnectedState)
            socket->waitForDisconnected(2000);
        completed.set_value(request.startsWith("GET ") && sent);
    });

    const quint16 stalled_endpoint = stalled_port.get();
    const quint16 healthy_endpoint = healthy_port.get();
    if (stalled_endpoint == 0 || healthy_endpoint == 0)
    {
        stalled_upstream.join();
        healthy_upstream.join();
        return require(false, "Concurrent HTTP mock could not bind");
    }

    std::promise<void> start;
    std::shared_future<void> gate = start.get_future().share();
    QString stalled_error;
    QString healthy_error;
    std::promise<void> healthy_download_finished;
    std::future<void> healthy_completion = healthy_download_finished.get_future();
    QThreadPool download_pool;
    download_pool.setMaxThreadCount(2);
    QFuture<bool> stalled_fetch = QtConcurrent::run(&download_pool, [&]()
    {
        gate.wait();
        return Aowis::Map::Internal::fetchCopernicusSourceForIntegrationTest(
            stalled_directory,
            QUrl(QStringLiteral("http://127.0.0.1:%1/source.tif").arg(stalled_endpoint)),
            &stalled_error, 3000, 3000, 5000, 1200);
    });
    QFuture<bool> healthy_fetch = QtConcurrent::run(&download_pool, [&]()
    {
        gate.wait();
        const bool result = Aowis::Map::Internal::fetchCopernicusSourceForIntegrationTest(
            healthy_directory,
            QUrl(QStringLiteral("http://127.0.0.1:%1/source.tif").arg(healthy_endpoint)),
            &healthy_error, 3000, 3000, 5000);
        healthy_download_finished.set_value();
        return result;
    });
    start.set_value();

    const bool healthy_finished_before_stall =
        healthy_completion.wait_for(std::chrono::milliseconds(800)) == std::future_status::ready;
    healthy_fetch.waitForFinished();
    stalled_fetch.waitForFinished();
    const bool healthy_ok = healthy_fetch.result();
    const bool stalled_ok = stalled_fetch.result();
    const bool healthy_sent = healthy_result.get();
    const bool stalled_disconnected = stalled_result.get();
    healthy_upstream.join();
    stalled_upstream.join();

    QFile published(QDir(healthy_directory).filePath(relative_source));
    return require(healthy_finished_before_stall,
                   "Healthy transfer did not finish while another connection was stalled") &&
           require(healthy_sent && healthy_ok,
                   QStringLiteral("Healthy parallel transfer failed: %1")
                       .arg(healthy_error).toUtf8().constData()) &&
           require(published.open(QIODevice::ReadOnly) && published.readAll() == payload,
                   "Healthy parallel transfer published incorrect bytes") &&
           require(stalled_disconnected && !stalled_ok &&
                   stalled_error.contains("cancel", Qt::CaseInsensitive),
                   QStringLiteral("Stalled parallel transfer did not cancel: %1")
                       .arg(stalled_error).toUtf8().constData()) &&
           require(!QFile::exists(QDir(stalled_directory).filePath(relative_source)),
                   "Stalled parallel transfer published an incomplete source");
}

bool verifyRecoveryScenario(const QString &root, int scenario,
                            const QByteArray &old_payload,
                            const QByteArray &new_payload)
{
    const QString directory = QDir(root).filePath(QStringLiteral("recovery-%1").arg(scenario));
    const QString source = QDir(directory).filePath(
        "copernicus/glo30/Copernicus_DSM_COG_10_N50_00_E008_00_DEM.tif");
    const QString partial = source + ".partial";
    const QString metadata = source + ".partial.meta";
    if (!QDir().mkpath(QFileInfo(source).absolutePath()))
        return require(false, "Unable to create source directory");
    QFile prefix(partial);
    if (!require(prefix.open(QIODevice::WriteOnly) &&
                 prefix.write(old_payload.left(300)) == 300,
                 "Unable to seed partial source"))
        return false;
    prefix.close();
    const Aowis::Map::Internal::CopernicusPartialState state{300, 1000, "\"v1\""};
    if (!require(Aowis::Map::Internal::saveCopernicusPartialState(
                     metadata, state, 256LL * 1024LL * 1024LL),
                 "Unable to seed resume metadata"))
        return false;

    MockUpstream mock = launchRecoveryUpstream(scenario, new_payload);
    if (!require(mock.port != 0, "Recovery mock upstream could not bind"))
    {
        mock.thread.join();
        return false;
    }
    QString error;
    const bool success = Aowis::Map::Internal::fetchCopernicusSourceForIntegrationTest(
        directory, QUrl(QStringLiteral("http://127.0.0.1:%1/source.tif").arg(mock.port)),
        &error);
    const bool headers_ok = mock.outcome.get();
    mock.thread.join();
    if (!require(headers_ok, "Recovery request omitted Range or If-Range") ||
        !require(success == (scenario != 2), "Incorrect recovery response result"))
        return false;
    if (scenario == 2)
        return require(!QFile::exists(source), "Mismatched range was published") &&
               require(!QFile::exists(partial) && !QFile::exists(metadata),
                       "Invalid partial state was retained");
    QFile published(source);
    return require(published.open(QIODevice::ReadOnly) &&
                   published.readAll() == new_payload,
                   "Full replacement did not publish the new source") &&
           require(!QFile::exists(partial) && !QFile::exists(metadata),
                   "Partial state survived full replacement");
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir temp;
    if (!require(temp.isValid(), "Temporary directory unavailable"))
        return 1;
    QByteArray payload(1000, '\0');
    for (qsizetype i = 0; i < payload.size(); ++i)
        payload[i] = char('A' + i % 26);
    const QString source = QDir(temp.path()).filePath(
        "copernicus/glo30/Copernicus_DSM_COG_10_N50_00_E008_00_DEM.tif");
    const QString partial = source + ".partial";
    const QString metadata = source + ".partial.meta";
    for (int step = 0; step < 3; ++step)
    {
        MockUpstream mock = launchUpstream(step, payload);
        if (!require(mock.port != 0, "Mock upstream could not bind"))
        {
            mock.thread.join();
            return 1;
        }
        QString error;
        const bool success = Aowis::Map::Internal::fetchCopernicusSourceForIntegrationTest(
            temp.path(), QUrl(QStringLiteral("http://127.0.0.1:%1/source.tif").arg(mock.port)), &error);
        const bool valid_request = mock.outcome.get();
        mock.thread.join();
        if (!require(valid_request, "Incorrect HTTP Range or If-Range request") ||
            !require(success == (step == 2), "Unexpected download result"))
            return 1;
        if (step < 2)
        {
            const qint64 expected = step == 0 ? 300 : 700;
            const auto state = Aowis::Map::Internal::loadCopernicusPartialState(
                partial, metadata, 256LL * 1024LL * 1024LL);
            if (!require(state.has_value() && state->downloaded_bytes == expected,
                         "Interrupted bytes not preserved") ||
                !require(!QFile::exists(source), "Incomplete source became visible"))
                return 1;
        }
    }
    QFile published(source);
    if (!require(published.open(QIODevice::ReadOnly) && published.readAll() == payload,
                 "Resumed source bytes differ from original") ||
        !require(!QFile::exists(partial) && !QFile::exists(metadata),
                 "Partial state remained after successful publication"))
        return 1;
    QByteArray changed_payload(1000, '\0');
    for (qsizetype i = 0; i < changed_payload.size(); ++i)
        changed_payload[i] = char('a' + i % 26);
    if (!verifyConcurrentStallAndCompletion(temp.path()) ||
        !verifyActiveCancellation(temp.path()) ||
        !verifySlowNetwork(temp.path(), 0) ||
        !verifySlowNetwork(temp.path(), 1) ||
        !verifySlowNetwork(temp.path(), 2) ||
        !verifyRecoveryScenario(temp.path(), 0, payload, changed_payload) ||
        !verifyRecoveryScenario(temp.path(), 1, payload, payload) ||
        !verifyRecoveryScenario(temp.path(), 2, payload, changed_payload))
        return 1;
    return 0;
}
