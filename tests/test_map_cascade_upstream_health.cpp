#include <aowis/map/cascade_upstream_health.h>
#include <aowis/map/provider_catalog.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

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

const QByteArray first_id = QByteArrayLiteral("00000000-0000-4000-8000-000000000001");
const QByteArray second_id = QByteArrayLiteral("00000000-0000-4000-8000-000000000002");
const QByteArray own_id = QByteArrayLiteral("00000000-0000-4000-8000-0000000000aa");

// Serves one fixed provider catalog to every request.
class CatalogServer final : public QTcpServer
{
public:
    QByteArray body;
    QList<QByteArray> requests;

    bool start() { return listen(QHostAddress::LocalHost, 0); }
    QUrl origin() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(serverPort()));
    }

protected:
    void incomingConnection(qintptr descriptor) override
    {
        QTcpSocket *socket = new QTcpSocket(this);
        if (!socket->setSocketDescriptor(descriptor))
        {
            socket->deleteLater();
            return;
        }
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket]()
        {
            const QByteArray request = socket->readAll();
            if (!request.contains("\r\n\r\n")) return;
            this->requests.append(request);
            QByteArray response = QByteArrayLiteral("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                                    "Connection: close\r\nContent-Length: ");
            response += QByteArray::number(this->body.size()) + "\r\n\r\n" + this->body;
            socket->write(response);
            socket->disconnectFromHost();
        });
    }
};

QByteArray catalogWith(const QString &id, const QJsonArray &via)
{
    QJsonObject provider;
    provider.insert(QStringLiteral("id"), id);
    provider.insert(QStringLiteral("name"), QStringLiteral("Upstream imagery"));
    provider.insert(QStringLiteral("url"), QStringLiteral("https://tiles.example.org/{z}/{x}/{y}.png"));
    provider.insert(QStringLiteral("minimum_zoom"), 0);
    provider.insert(QStringLiteral("maximum_zoom"), 19);
    provider.insert(QStringLiteral("enabled"), true);
    provider.insert(QStringLiteral("remote_fetch_enabled"), true);
    provider.insert(QStringLiteral("via"), via);
    return QJsonDocument(QJsonObject{{QStringLiteral("providers"), QJsonArray{provider}}})
        .toJson(QJsonDocument::Compact);
}

QVector<Aowis::Map::AvailableMapProvider> discover(const QString &directory,
                                                   CatalogServer &server)
{
    Aowis::Map::MapCascadeConfig cascade;
    Aowis::Map::MapCascadeUpstream upstream;
    upstream.name = QStringLiteral("peer");
    upstream.url = server.origin();
    cascade.upstreams.append(upstream);
    Aowis::Map::ProviderCatalog catalog(directory);
    catalog.setInstanceId(own_id);
    catalog.configure({}, cascade);
    const qsizetype before = server.requests.size();
    catalog.refresh();
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&loop, &server, before]()
    {
        if (server.requests.size() > before) loop.quit();
    });
    poll.start(20);
    timeout.start(5000);
    loop.exec();
    // Let the reply be processed after the request was seen.
    QTimer::singleShot(200, &loop, &QEventLoop::quit);
    loop.exec();
    return catalog.available();
}
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    using namespace Aowis::Map;

    require(isCascadeInstanceId(first_id), "Valid instance ID rejected");
    require(!isCascadeInstanceId(QByteArrayLiteral("00000000-0000-4000-8000-00000000000G")),
            "Uppercase/invalid instance ID accepted");
    require(!isCascadeInstanceId(QByteArrayLiteral("short")), "Short instance ID accepted");
    const QList<QByteArray> ids = cascadeTraceIds(first_id + ",junk," + second_id);
    require(ids.size() == 2 && ids.at(0) == first_id && ids.at(1) == second_id,
            "Trace IDs not extracted");
    require(cascadeTraceIds(QByteArray()).isEmpty(), "Empty trace yields IDs");

    require(CascadeUpstreamHealth::cooldownForFailures(0) == 0 &&
            CascadeUpstreamHealth::cooldownForFailures(1) == 5000 &&
            CascadeUpstreamHealth::cooldownForFailures(2) == 10000 &&
            CascadeUpstreamHealth::cooldownForFailures(4) == 40000 &&
            CascadeUpstreamHealth::cooldownForFailures(5) == 60000 &&
            CascadeUpstreamHealth::cooldownForFailures(50) == 60000,
            "Cooldown sequence is not 5, 10, 20, 40, 60 s");

    CascadeUpstreamHealth health;
    const QString primary = QStringLiteral("primary");
    qint64 now = 1000000;
    require(!health.coolingDown(primary, now), "Unknown upstream cooling down");
    health.recordUnreachable(primary, QStringLiteral("refused"), now);
    require(health.coolingDown(primary, now + 4999) && !health.coolingDown(primary, now + 5000),
            "First failure must cool down for 5 s");
    // Failures of requests already in flight during the cooldown are the
    // same outage and must not escalate it.
    health.recordUnreachable(primary, QStringLiteral("refused"), now + 100);
    require(health.snapshot().value(primary).consecutive_failures == 1,
            "Parallel failures escalated one outage");
    now += 5000;
    health.recordUnreachable(primary, QStringLiteral("refused"), now);
    require(health.coolingDown(primary, now + 9999) && !health.coolingDown(primary, now + 10000),
            "Second failure must cool down for 10 s");
    health.recordAnswered(primary, first_id, now + 1);
    require(!health.coolingDown(primary, now + 2), "An answer did not end the cooldown");
    require(health.snapshot().value(primary).consecutive_failures == 0 &&
            health.snapshot().value(primary).instance_id == first_id,
            "Answer did not reset failures or learn the instance");

    require(health.instanceInTrace(primary, {second_id, first_id}),
            "Upstream in visited trace not detected");
    require(!health.instanceInTrace(primary, {second_id}), "Foreign trace matched");
    require(!health.instanceInTrace(QStringLiteral("unknown"), {first_id}),
            "Upstream without learned instance matched");
    // Tie-break between two servers asking each other: only the higher ID refuses.
    require(health.refusesJoin(primary, {first_id}, own_id), "Higher ID did not refuse");
    require(!health.refusesJoin(primary, {first_id}, QByteArrayLiteral("00000000-0000-4000-8000-000000000000")),
            "Lower ID refused as well");
    require(!health.refusesJoin(primary, {second_id}, own_id), "Refused without a loop");
    health.recordAnswered(primary, QByteArrayLiteral("not-an-id"), now + 3);
    require(health.snapshot().value(primary).instance_id == first_id,
            "Invalid instance header replaced the learned ID");
    health.reset();
    require(health.snapshot().isEmpty(), "Reset kept state");

    // Provider provenance: an entry that already passed through this server
    // is never inherited back; others are, with their provenance kept.
    QTemporaryDir directory;
    require(directory.isValid(), "Temporary directory failed");
    CatalogServer server;
    require(server.start(), "Catalog server failed");
    const QString provider_id = QStringLiteral("2f5b0f0e-6d3c-4b8e-9a51-0c7d9e1f2a3b");

    server.body = catalogWith(provider_id, QJsonArray{QString::fromLatin1(second_id),
                                                      QString::fromLatin1(own_id)});
    const QVector<AvailableMapProvider> circular =
        discover(QDir(directory.path()).filePath(QStringLiteral("circular")), server);
    require(!server.requests.isEmpty() &&
            server.requests.last().toLower().contains(
                "x-aowis-cascade-visited: " + own_id),
            "Catalog discovery did not send its own instance as trace");
    for (const AvailableMapProvider &entry : circular)
        require(entry.provider.id != provider_id, "Own advertisement was inherited back");

    server.body = catalogWith(provider_id, QJsonArray{QString::fromLatin1(second_id)});
    const QVector<AvailableMapProvider> inherited =
        discover(QDir(directory.path()).filePath(QStringLiteral("inherited")), server);
    bool found = false;
    for (const AvailableMapProvider &entry : inherited)
        if (entry.provider.id == provider_id)
            found = entry.inherited && entry.via == QStringList{QString::fromLatin1(second_id)};
    require(found, "Foreign provider was not inherited with its provenance");
    return EXIT_SUCCESS;
}
