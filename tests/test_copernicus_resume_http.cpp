#include "../src/lib/copernicus_resume_integration_test.h"
#include "../src/lib/copernicus_resume_state.h"

#include <QCoreApplication>
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
bool verifySlowNetwork(const QString &root, bool stall)
{
    const QString directory = QDir(root).filePath(stall ? "stalled-transfer" : "slow-transfer");
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
        &error, 500, 500, stall ? 900 : 4000);
    const bool request_ok = outcome.get();
    server_thread.join();
    const QString source = QDir(directory).filePath(
        "copernicus/glo30/Copernicus_DSM_COG_10_N50_00_E008_00_DEM.tif");
    if (!require(request_ok, "Slow-network upstream did not receive a full GET") ||
        !require(fetched == !stall, "Unexpected slow-network transfer result"))
        return false;
    if (stall)
        return require(!QFile::exists(source), "Stalled transfer published incomplete source");
    QFile published(source);
    return require(published.open(QIODevice::ReadOnly) &&
                   published.readAll() == QByteArray(1000, 'S'),
                   "Slow but progressing transfer did not publish complete source");
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
    if (!verifySlowNetwork(temp.path(), false) ||
        !verifySlowNetwork(temp.path(), true) ||
        !verifyRecoveryScenario(temp.path(), 0, payload, changed_payload) ||
        !verifyRecoveryScenario(temp.path(), 1, payload, payload) ||
        !verifyRecoveryScenario(temp.path(), 2, payload, changed_payload))
        return 1;
    return 0;
}
