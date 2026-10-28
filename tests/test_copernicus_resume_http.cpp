#include "../src/lib/copernicus_resume_integration_test.h"
#include "../src/lib/copernicus_resume_state.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>

#include <future>
#include <iostream>
#include <thread>

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
    return 0;
}
