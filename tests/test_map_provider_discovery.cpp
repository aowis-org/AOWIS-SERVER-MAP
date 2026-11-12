#include "../src/server/server.h"

#include <aowis/map/settings_repository.h>
#include <aowis/map/provider_catalog.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>

class ProviderDiscoveryTestAccess
{
public:
    static quint16 port(const Server &server) { return server.tcp->serverPort(); }
};

namespace
{
const QByteArray ValidPng = QByteArray::fromBase64(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+j"
    "mZkAAAAASUVORK5CYII=");

bool check(bool value, const char *message)
{
    if (!value) std::fprintf(stderr, "FAILED: %s\n", message);
    return value;
}

class Stub final : public QTcpServer
{
public:
    QByteArray catalog;
    QByteArray fingerprint;
    QByteArray api_key;
    QByteArray tile = ValidPng;
    int catalog_count = 0;
    int tile_count = 0;
    bool legacy_catalog_only = false;

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
        QObject::connect(socket, &QTcpSocket::readyRead, socket,
            [this, socket, request_bytes = QByteArray()]() mutable
        {
            request_bytes.append(socket->readAll());
            if (!request_bytes.contains("\r\n\r\n")) return;
            const QByteArray request = request_bytes.left(request_bytes.indexOf("\r\n\r\n"));
            const QByteArray path = request.split(' ').value(1);
            const bool authorized = this->api_key.isEmpty() ||
                request.toLower().contains("x-api-key: " + this->api_key.toLower());
            const bool new_catalog = path == "/map/v1/providers/available";
            const bool legacy_catalog = path == "/map/v1/providers";
            const bool is_catalog = this->legacy_catalog_only ? legacy_catalog : new_catalog;
            if (is_catalog) ++this->catalog_count;
            else if (path.endsWith(".png")) ++this->tile_count;
            const int status = !authorized ? 401 : (is_catalog || path.endsWith(".png") ? 200 : 404);
            const QByteArray bytes = status != 200 ? QByteArrayLiteral("missing") :
                (is_catalog ? this->catalog : this->tile);
            QByteArray response = "HTTP/1.1 " + QByteArray::number(status) +
                (status == 200 ? " OK\r\n" : " Error\r\n");
            response += "Content-Type: " + QByteArray(is_catalog ? "application/json" : "image/png") +
                "\r\nContent-Length: " + QByteArray::number(bytes.size()) + "\r\n";
            if (!is_catalog && !this->fingerprint.isEmpty())
                response += "X-AOWIS-Raster-Source: " + this->fingerprint + "\r\n";
            response += "Connection: close\r\n\r\n";
            socket->write(response + bytes);
            socket->disconnectFromHost();
        });
    }
};

struct ReplyData
{
    int status = 0;
    QByteArray body;
};

ReplyData request(QCoreApplication &application, QNetworkAccessManager *manager,
                  const QUrl &url, const QByteArray &api_key = {})
{
    QNetworkRequest query(url);
    if (!api_key.isEmpty()) query.setRawHeader("X-API-Key", api_key);
    QNetworkReply *reply = manager->get(query);
    QElapsedTimer timer;
    timer.start();
    while (!reply->isFinished() && timer.elapsed() < 9000)
    {
        application.processEvents(QEventLoop::AllEvents, 15);
        QThread::msleep(1);
    }
    if (!reply->isFinished()) reply->abort();
    ReplyData data;
    data.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    data.body = reply->readAll();
    delete reply;
    return data;
}
ReplyData updateProvider(QCoreApplication &application, QNetworkAccessManager *manager,
                         const QUrl &url, const QByteArray &key, const QString &id, bool enabled)
{
    QNetworkRequest query(url);
    query.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (!key.isEmpty()) query.setRawHeader("X-API-Key", key);
    const QByteArray body = QJsonDocument(QJsonObject{
        {QStringLiteral("id"), id}, {QStringLiteral("enabled"), enabled}
    }).toJson(QJsonDocument::Compact);
    QNetworkReply *reply = manager->put(query, body);
    QElapsedTimer timer;
    timer.start();
    while (!reply->isFinished() && timer.elapsed() < 9000)
    {
        application.processEvents(QEventLoop::AllEvents, 15);
        QThread::msleep(1);
    }
    if (!reply->isFinished()) reply->abort();
    ReplyData response;
    response.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    response.body = reply->readAll();
    delete reply;
    return response;
}
ReplyData updateMode(QCoreApplication &application, QNetworkAccessManager *manager,
                     const QUrl &url, const QByteArray &key, const QString &id,
                     const QString &mode)
{
    QNetworkRequest query(url);
    query.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (!key.isEmpty()) query.setRawHeader("X-API-Key", key);
    const QByteArray body = QJsonDocument(QJsonObject{
        {QStringLiteral("id"), id}, {QStringLiteral("mode"), mode}
    }).toJson(QJsonDocument::Compact);
    QNetworkReply *reply = manager->put(query, body);
    QElapsedTimer timer;
    timer.start();
    while (!reply->isFinished() && timer.elapsed() < 9000)
    {
        application.processEvents(QEventLoop::AllEvents, 15);
        QThread::msleep(1);
    }
    if (!reply->isFinished()) reply->abort();
    ReplyData response;
    response.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    response.body = reply->readAll();
    delete reply;
    return response;
}
ReplyData updateRoute(QCoreApplication &application, QNetworkAccessManager *manager,
                      const QUrl &url, const QByteArray &key, const QString &id,
                      const QStringList &upstreams)
{
    QNetworkRequest query(url);
    query.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (!key.isEmpty()) query.setRawHeader("X-API-Key", key);
    QJsonArray selected;
    for (const QString &name : upstreams) selected.append(name);
    const QByteArray body = QJsonDocument(QJsonObject{
        {QStringLiteral("id"), id}, {QStringLiteral("upstreams"), selected}
    }).toJson(QJsonDocument::Compact);
    QNetworkReply *reply = manager->put(query, body);
    QElapsedTimer timer;
    timer.start();
    while (!reply->isFinished() && timer.elapsed() < 9000)
    {
        application.processEvents(QEventLoop::AllEvents, 15);
        QThread::msleep(1);
    }
    if (!reply->isFinished()) reply->abort();
    ReplyData response;
    response.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    response.body = reply->readAll();
    delete reply;
    return response;
}
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QTemporaryDir storage;
    if (!check(storage.isValid(), "Create temporary cache")) return 1;
    Stub root;
    Stub backup;
    Stub origin;
    if (!check(root.listen(QHostAddress::LocalHost, 0) &&
               backup.listen(QHostAddress::LocalHost, 0) &&
               origin.listen(QHostAddress::LocalHost, 0), "Start loopback servers")) return 1;

    Aowis::Map::MapProvider inherited;
    inherited.id = QStringLiteral("15c2d930-1e4a-4b76-9675-31ea802989fa");
    inherited.name = QStringLiteral("Test upstream imagery");
    inherited.url_template = origin.origin().toString() + QStringLiteral("/{z}/{x}/{y}.png");
    inherited.minimum_zoom = 0;
    inherited.maximum_zoom = 19;
    root.catalog = Aowis::Map::encodeProviderCatalog({inherited});
    root.api_key = QByteArrayLiteral("upstream-key");
    root.fingerprint = QCryptographicHash::hash(inherited.url_template.toUtf8(),
                                               QCryptographicHash::Sha256).toHex();
    backup.catalog = root.catalog;
    backup.fingerprint = root.fingerprint;
    backup.api_key = QByteArrayLiteral("backup-key");
    backup.legacy_catalog_only = true; // Compatibility with pre-discovery MAP.

    Server::Config config;
    config.listen_address = QHostAddress::LocalHost;
    config.port = 0;
    config.terrain_enabled = false;
    config.cache_directory = QDir(storage.path()).filePath(QStringLiteral("cache"));
    config.map_providers.clear();
    config.api_key = QByteArrayLiteral("client-key");
    config.cascade.allow_direct_fetch = true; // Must NEVER apply to inherited sources.
    config.cascade.upstreams.append({QStringLiteral("root"), root.origin(), root.api_key});
    config.cascade.upstreams.append({QStringLiteral("backup"), backup.origin(), backup.api_key});
    config.delete_api_key = QByteArrayLiteral("admin-key");
    config.managed_settings_root = QDir(storage.path()).filePath(QStringLiteral("map-config"));

    QNetworkAccessManager network;
    const QString path = QStringLiteral("/%1/6/30/22.png").arg(inherited.id);
    {
        Server downstream(config);
        if (!check(downstream.start(), "Start downstream MAP")) return 1;
        const QString base = QStringLiteral("http://127.0.0.1:%1").arg(
            ProviderDiscoveryTestAccess::port(downstream));
        const QUrl available_url(base + QStringLiteral("/map/v1/providers/available"));
        const QUrl local_url(base + QStringLiteral("/map/v1/providers"));
        if (!check(request(application, &network, available_url).status == 401,
                   "Discovered catalog requires read authentication")) return 1;
        bool found = false;
        for (int index = 0; index < 150 && !found; ++index)
        {
            const ReplyData response = request(application, &network, available_url,
                                               config.api_key);
            const QJsonArray array = QJsonDocument::fromJson(response.body)
                .object().value(QStringLiteral("providers")).toArray();
            if (array.size() == 1)
            {
                const QJsonObject provider = array.first().toObject();
                found = provider.value(QStringLiteral("id")).toString() == inherited.id &&
                    provider.value(QStringLiteral("origin")).toString() ==
                        QStringLiteral("inherited") &&
                    provider.value(QStringLiteral("upstreams")).toArray().size() == 2 &&
                    provider.value(QStringLiteral("upstreams")).toArray().at(0).toString() ==
                        QStringLiteral("root") &&
                    provider.value(QStringLiteral("upstreams")).toArray().at(1).toString() ==
                        QStringLiteral("backup");
            }
            if (!found) QThread::msleep(5);
        }
        if (!check(found && root.catalog_count > 0 && backup.catalog_count > 0,
                   "Discover and merge ordered upstream providers")) return 1;
        // Built-in provider UUIDs commonly exist on both servers. A local
        // definition keeps ownership but must show its matching upstreams.
        Aowis::Map::ProviderCatalog overlapping(
            QDir(storage.path()).filePath(QStringLiteral("overlap-cache")));
        overlapping.configure({inherited}, config.cascade);
        overlapping.refresh();
        bool overlap_found = false;
        for (int index = 0; index < 150 && !overlap_found; ++index)
        {
            application.processEvents(QEventLoop::AllEvents, 15);
            const QVector<Aowis::Map::AvailableMapProvider> entries = overlapping.available();
            overlap_found = entries.size() == 1 && !entries.first().inherited &&
                entries.first().upstream_names ==
                    QStringList{QStringLiteral("root"), QStringLiteral("backup")};
            if (!overlap_found) QThread::msleep(5);
        }
        if (!check(overlap_found && overlapping.inheritedUpstreams().isEmpty(),
                   "Advertise matching upstreams without duplicating local provider")) return 1;
        Aowis::Map::MapProvider different = inherited;
        different.url_template = QStringLiteral("https://unrelated.example/{z}/{x}/{y}.png");
        Aowis::Map::ProviderCatalog conflicting(
            QDir(storage.path()).filePath(QStringLiteral("conflicting-cache")));
        conflicting.configure({different}, config.cascade);
        const int root_catalog_before_conflict = root.catalog_count;
        const int backup_catalog_before_conflict = backup.catalog_count;
        conflicting.refresh();
        for (int index = 0; index < 150 &&
             (root.catalog_count == root_catalog_before_conflict ||
              backup.catalog_count == backup_catalog_before_conflict); ++index)
        {
            application.processEvents(QEventLoop::AllEvents, 15);
            QThread::msleep(5);
        }
        const QVector<Aowis::Map::AvailableMapProvider> different_entries =
            conflicting.available();
        if (!check(root.catalog_count > root_catalog_before_conflict &&
                   backup.catalog_count > backup_catalog_before_conflict &&
                   different_entries.size() == 1 &&
                   different_entries.first().upstream_names.isEmpty(),
                   "Do not advertise conflicting upstream source under local UUID")) return 1;
        const ReplyData local = request(application, &network, local_url, config.api_key);
        if (!check(local.status == 200 && QJsonDocument::fromJson(local.body)
                   .object().value(QStringLiteral("providers")).toArray().isEmpty(),
                   "Keep editable local catalog separate")) return 1;
        const QUrl override_url(base + QStringLiteral("/map/v1/providers/inherited"));
        if (!check(updateProvider(application, &network, override_url, {}, inherited.id,
                                  false).status == 401,
                   "Inherited provider edits require administrator authentication")) return 1;
        if (!check(updateProvider(application, &network, override_url,
                                  config.delete_api_key, inherited.id, false).status == 200,
                   "Disable inherited provider locally")) return 1;
        const ReplyData disabled_catalog = request(application, &network, available_url,
                                                   config.api_key);
        const QJsonArray disabled_entries = QJsonDocument::fromJson(disabled_catalog.body)
            .object().value(QStringLiteral("providers")).toArray();
        if (!check(disabled_entries.size() == 1 &&
                   !disabled_entries.first().toObject().value(QStringLiteral("enabled")).toBool() &&
                   disabled_entries.first().toObject().value(QStringLiteral("origin")).toString() ==
                       QStringLiteral("inherited"),
                   "Disabled inherited provider remains discoverable")) return 1;
        std::unique_ptr<Aowis::Map::SettingsRepository> override_repository =
            Aowis::Map::SettingsRepository::openAtOwnerRoot(
                QStringLiteral("server-map"), config.managed_settings_root);
        QSet<QString> stored_disabled;
        if (!check(override_repository != nullptr &&
                   override_repository->readDisabledInheritedProviders(&stored_disabled) &&
                   stored_disabled.contains(inherited.id),
                   "Inherited disable state saved through MAP CONFIG")) return 1;
        const int root_tiles_before_disabled = root.tile_count;
        const ReplyData disabled_tile = request(application, &network, QUrl(base + path),
                                                config.api_key);
        if (!check(disabled_tile.status != 200 && root.tile_count == root_tiles_before_disabled,
                   "Disabled provider cannot retrieve tiles from upstream")) return 1;
        if (!check(updateProvider(application, &network, override_url,
                                  config.delete_api_key, inherited.id, true).status == 200,
                   "Re-enable inherited provider without editing upstream catalog")) return 1;
        std::unique_ptr<Aowis::Map::SettingsRepository> persisted =
            Aowis::Map::SettingsRepository::openAtOwnerRoot(
                QStringLiteral("server-map"), config.managed_settings_root);
        QSet<QString> disabled_after;
        if (!check(persisted != nullptr &&
                   persisted->readDisabledInheritedProviders(&disabled_after) &&
                   disabled_after.isEmpty(), "Inherited override persists in MAP CONFIG")) return 1;
        const ReplyData tile = request(application, &network, QUrl(base + path),
                                       config.api_key);
        if (!check(tile.status == 200 && tile.body == ValidPng &&
                   root.tile_count > 0 && origin.tile_count == 0,
                   "Fetch inherited provider via MAP only")) return 1;
        // Routing overrides are local-only, select an ordered subset of the
        // advertising peers, and never contact a provider's origin directly.
        const QUrl routes_url(base + QStringLiteral("/map/v1/providers/upstreams"));
        if (!check(updateRoute(application, &network, routes_url, {}, inherited.id,
                               {QStringLiteral("backup")}).status == 401,
                   "Provider route updates require administrator authentication")) return 1;
        if (!check(updateRoute(application, &network, routes_url, config.delete_api_key,
                               inherited.id, {QStringLiteral("unknown")}).status == 400,
                   "Unknown advertising peers cannot be selected")) return 1;
        if (!check(updateRoute(application, &network, routes_url, config.delete_api_key,
                               inherited.id, {QStringLiteral("backup"), QStringLiteral("root")})
                       .status == 200,
                   "Save preferred MAP upstream order")) return 1;
        const QJsonArray ordered_entries = QJsonDocument::fromJson(
            request(application, &network, available_url, config.api_key).body)
            .object().value(QStringLiteral("providers")).toArray();
        const QJsonArray selected_upstreams = ordered_entries.first().toObject()
            .value(QStringLiteral("selected_upstreams")).toArray();
        if (!check(selected_upstreams.size() == 2 &&
                   selected_upstreams.at(0).toString() == QStringLiteral("backup") &&
                   selected_upstreams.at(1).toString() == QStringLiteral("root"),
                   "Advertise the effective provider-specific route order")) return 1;
        const QUrl mode_url(base + QStringLiteral("/map/v1/providers/acquisition"));
        if (!check(updateMode(application, &network, mode_url, {}, inherited.id,
                              QStringLiteral("automatic")).status == 401,
                   "Acquisition policy edits require administrator authentication")) return 1;
        if (!check(updateMode(application, &network, mode_url, config.delete_api_key,
                              inherited.id, QStringLiteral("direct")).status == 403,
                   "Inherited providers must never select Direct")) return 1;
        if (!check(updateMode(application, &network, mode_url, config.delete_api_key,
                              inherited.id, QStringLiteral("nonsense")).status == 400,
                   "Unknown acquisition policies are rejected")) return 1;
        if (!check(updateMode(application, &network, mode_url, config.delete_api_key,
                              inherited.id, QStringLiteral("automatic")).status == 200,
                   "Inherited provider may select Automatic (MAP upstream only)")) return 1;
        const QJsonArray automatic_catalog = QJsonDocument::fromJson(
            request(application, &network, available_url, config.api_key).body)
            .object().value(QStringLiteral("providers")).toArray();
        if (!check(automatic_catalog.first().toObject().value(
                       QStringLiteral("acquisition_mode")).toString() ==
                       QStringLiteral("automatic") &&
                   !automatic_catalog.first().toObject().value(
                       QStringLiteral("direct_fetch_allowed")).toBool(),
                   "Catalog advertises effective inherited policy")) return 1;
        if (!check(updateMode(application, &network, mode_url, config.delete_api_key,
                              inherited.id, QStringLiteral("cascade")).status == 200,
                   "Reset inherited provider to safe cascade default")) return 1;
        const int root_before_routing = root.tile_count;
        const int backup_before_routing = backup.tile_count;
        const ReplyData preferred = request(application, &network,
            QUrl(base + QStringLiteral("/%1/6/30/25.png").arg(inherited.id)), config.api_key);
        if (!check(preferred.status == 200 && preferred.body == ValidPng &&
                   backup.tile_count > backup_before_routing &&
                   root.tile_count == root_before_routing && origin.tile_count == 0,
                   "Preferred upstream wins without contacting origin or global primary")) return 1;
        std::unique_ptr<Aowis::Map::SettingsRepository> route_repository =
            Aowis::Map::SettingsRepository::openAtOwnerRoot(
                QStringLiteral("server-map"), config.managed_settings_root);
        QHash<QString, QStringList> saved_routes;
        if (!check(route_repository != nullptr &&
                   route_repository->readProviderUpstreamRoutes(&saved_routes) &&
                   saved_routes.value(inherited.id) ==
                       QStringList({QStringLiteral("backup"), QStringLiteral("root")}),
                   "Selected MAP upstreams persist in MAP-owned CONFIG")) return 1;
        if (!check(updateRoute(application, &network, routes_url, config.delete_api_key,
                               inherited.id, {}).status == 200,
                   "Reset per-provider order to global priority")) return 1;
        const QJsonArray reset_entries = QJsonDocument::fromJson(
            request(application, &network, available_url, config.api_key).body)
            .object().value(QStringLiteral("providers")).toArray();
        if (!check(reset_entries.first().toObject().value(QStringLiteral("selected_upstreams"))
                       .toArray().first().toString() == QStringLiteral("root"),
                   "Default global upstream priority restored")) return 1;
        // Cached tiles must also obey local enablement, not just new downloads.
        if (!check(updateProvider(application, &network, override_url,
                                  config.delete_api_key, inherited.id, false).status == 200,
                   "Disable inherited provider with cached tiles")) return 1;
        if (!check(request(application, &network, QUrl(base + path), config.api_key).status != 200,
                   "Disabling hides even locally cached inherited tiles")) return 1;
        if (!check(updateProvider(application, &network, override_url,
                                  config.delete_api_key, inherited.id, true).status == 200 &&
                   request(application, &network, QUrl(base + path), config.api_key).status == 200,
                   "Re-enabling restores cached inherited tiles")) return 1;
        // The primary is now unavailable, but a second advertising MAP server
        // may serve another tile of the same source without direct fetching.
        // A local definition may select Direct, Cascade or Automatic. Check
        // actual HTTP destinations, not just the persisted mode string.
        Server::Config local_config = config;
        local_config.map_providers = {inherited};
        local_config.cache_directory = QDir(storage.path()).filePath(QStringLiteral("local-cache"));
        local_config.managed_settings_root = QDir(storage.path()).filePath(
            QStringLiteral("local-config"));
        {
            Server local(local_config);
            if (!check(local.start(), "Start MAP with a locally defined provider")) return 1;
            const QString local_base = QStringLiteral("http://127.0.0.1:%1").arg(
                ProviderDiscoveryTestAccess::port(local));
            const QUrl local_catalog(local_base + QStringLiteral("/map/v1/providers/available"));
            bool advertised = false;
            for (int retry = 0; retry < 80 && !advertised; ++retry)
            {
                const QJsonArray available = QJsonDocument::fromJson(
                    request(application, &network, local_catalog, config.api_key).body)
                    .object().value(QStringLiteral("providers")).toArray();
                if (!available.isEmpty())
                    advertised = available.first().toObject().value(
                        QStringLiteral("upstreams")).toArray().size() == 2;
                application.processEvents(QEventLoop::AllEvents, 10);
            }
            if (!check(advertised, "Local provider advertised by MAP upstreams")) return 1;
            const QUrl local_mode(local_base + QStringLiteral("/map/v1/providers/acquisition"));
            const int direct_origin_before = origin.tile_count;
            const int direct_root_before = root.tile_count;
            if (!check(updateMode(application, &network, local_mode, config.delete_api_key,
                                  inherited.id, QStringLiteral("direct")).status == 200,
                       "Local provider selects Direct")) return 1;
            const ReplyData direct = request(application, &network,
                QUrl(local_base + QStringLiteral("/%1/6/20/10.png").arg(inherited.id)),
                config.api_key);
            if (!check(direct.status == 200 && origin.tile_count > direct_origin_before &&
                       root.tile_count == direct_root_before,
                       "Direct mode bypasses MAP upstreams")) return 1;
            if (!check(updateMode(application, &network, local_mode, config.delete_api_key,
                                  inherited.id, QStringLiteral("cascade")).status == 200,
                       "Local provider selects Cascade")) return 1;
            const int cascade_origin_before = origin.tile_count;
            const int cascade_root_before = root.tile_count;
            const ReplyData cascaded = request(application, &network,
                QUrl(local_base + QStringLiteral("/%1/6/20/11.png").arg(inherited.id)),
                config.api_key);
            if (!check(cascaded.status == 200 && root.tile_count > cascade_root_before &&
                       origin.tile_count == cascade_origin_before,
                       "Cascade mode never contacts origin")) return 1;
            if (!check(updateMode(application, &network, local_mode, config.delete_api_key,
                                  inherited.id, QStringLiteral("automatic")).status == 200,
                       "Reset local provider to Automatic mode")) return 1;
            const int automatic_root_before = root.tile_count;
            const int automatic_origin_before = origin.tile_count;
            const ReplyData automatic = request(application, &network,
                QUrl(local_base + QStringLiteral("/%1/6/20/12.png").arg(inherited.id)),
                config.api_key);
            if (!check(automatic.status == 200 &&
                       root.tile_count > automatic_root_before &&
                       origin.tile_count == automatic_origin_before,
                       "Automatic mode uses a healthy MAP upstream before origin")) return 1;
            if (!check(updateMode(application, &network, local_mode, config.delete_api_key,
                                  inherited.id, QStringLiteral("cascade")).status == 200,
                       "Return local provider to Cascade policy")) return 1;
            std::unique_ptr<Aowis::Map::SettingsRepository> mode_repository =
                Aowis::Map::SettingsRepository::openAtOwnerRoot(
                    QStringLiteral("server-map"), local_config.managed_settings_root);
            QHash<QString, Aowis::Map::MapProviderAcquisitionMode> stored_modes;
            if (!check(mode_repository != nullptr &&
                       mode_repository->readProviderAcquisitionModes(&stored_modes) &&
                       stored_modes.value(inherited.id) ==
                           Aowis::Map::MapProviderAcquisitionMode::Cascade,
                       "Provider acquisition mode persists in MAP CONFIG")) return 1;
        }
        // Administrator policy remains authoritative regardless of an older
        // per-provider Direct setting saved by a previous session.
        Server::Config restricted_config = local_config;
        restricted_config.cascade.allow_direct_fetch = false;
        restricted_config.cache_directory = QDir(storage.path()).filePath(
            QStringLiteral("restricted-cache"));
        restricted_config.managed_settings_root = QDir(storage.path()).filePath(
            QStringLiteral("restricted-config"));
        {
            Server restricted(restricted_config);
            if (!check(restricted.start(), "Start MAP with direct origin forbidden")) return 1;
            const QString restricted_base = QStringLiteral("http://127.0.0.1:%1").arg(
                ProviderDiscoveryTestAccess::port(restricted));
            if (!check(updateMode(application, &network,
                    QUrl(restricted_base + QStringLiteral("/map/v1/providers/acquisition")),
                    config.delete_api_key, inherited.id, QStringLiteral("direct")).status == 403,
                    "Admin ban on direct fetching overrides provider-level preference")) return 1;
        }
        root.close();
        const ReplyData failover = request(application, &network,
            QUrl(base + QStringLiteral("/%1/6/30/23.png").arg(inherited.id)), config.api_key);
        if (!check(failover.status == 200 && failover.body == ValidPng &&
                   backup.tile_count > 0 && origin.tile_count == 0,
                   "Fallback uses second advertising upstream")) return 1;
    }
    backup.close();
    {
        Server offline(config);
        if (!check(offline.start(), "Restart downstream with offline upstream")) return 1;
        const QString base = QStringLiteral("http://127.0.0.1:%1").arg(
            ProviderDiscoveryTestAccess::port(offline));
        const ReplyData catalog = request(application, &network,
            QUrl(base + QStringLiteral("/map/v1/providers/available")), config.api_key);
        if (!check(catalog.status == 200 && QJsonDocument::fromJson(catalog.body)
                   .object().value(QStringLiteral("providers")).toArray().size() == 1,
                   "Restore validated offline provider catalog")) return 1;
        const ReplyData cached = request(application, &network, QUrl(base + path),
                                         config.api_key);
        if (!check(cached.status == 200 && cached.body == ValidPng,
                   "Serve cached inherited tile offline")) return 1;
        // New tile must not call the origin directly, even with direct fallback on.
        const ReplyData missing = request(application, &network,
            QUrl(base + QStringLiteral("/%1/6/30/24.png").arg(inherited.id)), config.api_key);
        if (!check(missing.status != 200 && origin.tile_count == 0,
                   "Inherited provider cannot bypass MAP upstream")) return 1;
    }
    // A local disabled definition is an explicit mask, not permission to
    // resurrect the same UUID from remote discovery.
    Aowis::Map::MapProvider disabled = inherited;
    disabled.enabled = false;
    Aowis::Map::ProviderCatalog masked(config.cache_directory);
    masked.configure({disabled}, config.cascade);
    const QVector<Aowis::Map::AvailableMapProvider> available = masked.available();
    if (!check(available.size() == 1 && !available.first().inherited &&
               !available.first().provider.enabled &&
               masked.inheritedUpstreams().isEmpty(),
               "Local disabled provider masks upstream advertisements")) return 1;
    return 0;
}
