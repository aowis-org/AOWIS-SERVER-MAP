#include "../src/server/configuration.h"

#include <aowis/map/settings_repository.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>
#include <QTemporaryDir>
#include <QUrl>

#include <cstdio>
#include <memory>

class MapManagedSettingsHttpTestAccess
{
public:
    static quint16 port(const Server &server) { return server.tcp->serverPort(); }
};

namespace
{
bool await(QCoreApplication &application, QNetworkReply *reply)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!reply->isFinished() && elapsed.elapsed() < 5000)
    {
        application.processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    return reply->isFinished();
}

bool check(bool ok, const char *what, const QString &detail = QString())
{
    if (!ok)
        std::fprintf(stderr, "FAILED: %s %s\n", what, qPrintable(detail));
    return ok;
}

QString httpFailureDetails(QNetworkReply *reply)
{
    return QStringLiteral("HTTP %1; network: %2; response: %3")
        .arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt())
        .arg(reply->errorString(), QString::fromUtf8(reply->peek(2048)));
}

bool writeFile(const QString &path, const QByteArray &contents)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(contents) == contents.size();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool loadServerConfig(const QString &path, Server::Config *config, bool migrate,
                      QString *error)
{
    config->provider_state_file = MapServerConfiguration::defaultProviderStateFilePath();
    return MapServerConfiguration::loadConfigFile(path, config, error) &&
           MapServerConfiguration::loadManagedSettings(config, migrate, error);
}
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QTemporaryDir temp;
    if (!check(temp.isValid(), "Create temporary directory")) return 1;

    const QString admin_path = QDir(temp.path()).filePath(QStringLiteral("admin.ini"));
    const QString without_upstream = QDir(temp.path()).filePath(QStringLiteral("no-upstream.ini"));
    const QString legacy_path = QDir(temp.path()).filePath(QStringLiteral("old/providers.ini"));
    const QByteArray admin_ini = QByteArrayLiteral(
        "# Administrator file: never change this.\n"
        "[storage]\nprovider_state_file=old/providers.ini\n"
        "managed_settings_directory=old/config/server-map\n"
        "[downloads]\nremote_fetch_enabled=false\n"
        "[terrain]\nremote_fetch_enabled=false\n"
        "[upstream]\nallow_direct_fetch=false\n"
        "[authentication]\napi_key=admin-secret\n");
    const QByteArray no_upstream_ini = QByteArrayLiteral(
        "[storage]\nprovider_state_file=old/providers.ini\n"
        "managed_settings_directory=old/config/server-map\n"
        "[downloads]\nremote_fetch_enabled=false\n");
    if (!check(writeFile(admin_path, admin_ini) &&
               writeFile(without_upstream, no_upstream_ini), "Write administrator INI"))
        return 1;

    QVector<Aowis::Map::MapProvider> legacy = Aowis::Map::defaultMapProviders();
    legacy[0].name = QStringLiteral("Imported provider");
    legacy[0].allow_lossy_transfer = false;
    QString error;
    if (!check(MapServerConfiguration::saveMapProvidersToState(legacy_path, legacy, &error),
               "Write old provider state", error)) return 1;
    const QByteArray legacy_bytes = readFile(legacy_path);

    Server::Config config;
    if (!check(loadServerConfig(admin_path, &config, true, &error),
               "Import legacy providers", error)) return 1;
    if (!check(config.map_providers[0].name == QStringLiteral("Imported provider") &&
               !config.map_providers[0].allow_lossy_transfer &&
               !config.provider_revision.isEmpty() &&
               !config.cascade.allow_direct_fetch &&
               config.cascade_defined_in_admin_ini &&
               !config.map_remote_fetch_enabled && !config.terrain_remote_fetch_enabled &&
               config.api_key == QByteArrayLiteral("admin-secret") &&
               readFile(admin_path) == admin_ini && readFile(legacy_path) == legacy_bytes,
               "Preserve administrator restrictions, secrets and old files")) return 1;

    std::unique_ptr<Aowis::Map::SettingsRepository> store =
        Aowis::Map::SettingsRepository::openAtOwnerRoot(
            QStringLiteral("server-map"), config.managed_settings_root, &error);
    if (!check(store != nullptr, "Open MAP-owned CONFIG", error)) return 1;
    QVector<Aowis::Map::MapProvider> saved;
    QString first_revision;
    if (!check(store->readProviders(&saved, &first_revision, &error) &&
               first_revision == config.provider_revision && saved[0].name == legacy[0].name,
               "Legacy catalog committed to CONFIG", error)) return 1;
    saved[0].name = QStringLiteral("Edited in CONFIG");
    QString new_revision;
    if (!check(store->saveProviders(saved, first_revision, &new_revision, &error),
               "Edit catalog with revision check", error)) return 1;
    if (!check(!store->saveProviders(legacy, first_revision, nullptr, &error) &&
               error.contains(QStringLiteral("revision conflict")),
               "Reject stale provider edit", error)) return 1;

    Server::Config after_restart;
    if (!check(loadServerConfig(admin_path, &after_restart, true, &error) &&
               after_restart.map_providers[0].name == QStringLiteral("Edited in CONFIG") &&
               after_restart.provider_revision == new_revision &&
               readFile(legacy_path) == legacy_bytes,
               "Restart uses CONFIG rather than legacy INI", error)) return 1;

    Aowis::Map::MapCascadeConfig cascade;
    cascade.allow_direct_fetch = true;
    cascade.poor_internet_mode = true;
    cascade.upstreams.append({QStringLiteral("central"),
                              QUrl(QStringLiteral("https://central.example")),
                              QByteArrayLiteral("central-key")});
    if (!check(store->saveCascade(cascade, QString(), nullptr, &error),
               "Write managed cascade", error)) return 1;
    Server::Config admin_authoritative;
    if (!check(loadServerConfig(admin_path, &admin_authoritative, true, &error) &&
               !admin_authoritative.cascade.allow_direct_fetch &&
               admin_authoritative.cascade.upstreams.isEmpty(),
               "Administrator INI forbids managed override", error)) return 1;
    Server::Config managed_authoritative;
    if (!check(loadServerConfig(without_upstream, &managed_authoritative, true, &error) &&
               managed_authoritative.cascade.allow_direct_fetch &&
               managed_authoritative.cascade.poor_internet_mode &&
               managed_authoritative.cascade.upstreams.size() == 1 &&
               managed_authoritative.cascade.upstreams[0].api_key == QByteArrayLiteral("central-key"),
               "CONFIG cascade used when administrator has no cascade directives", error))
        return 1;

    // A damaged legacy state cannot be imported. A validation-only run must
    // not write a CONFIG record even when it reads valid legacy state.
    const QString other_ini = QDir(temp.path()).filePath(QStringLiteral("other.ini"));
    const QString other_legacy = QDir(temp.path()).filePath(QStringLiteral("other/providers.ini"));
    const QByteArray other_admin = QByteArrayLiteral(
        "[storage]\nprovider_state_file=other/providers.ini\n"
        "managed_settings_directory=other/config/server-map\n");
    if (!check(writeFile(other_ini, other_admin) &&
               MapServerConfiguration::saveMapProvidersToState(other_legacy, legacy, &error),
               "Setup validation-only migration", error)) return 1;
    Server::Config check_only;
    if (!check(loadServerConfig(other_ini, &check_only, false, &error) &&
               check_only.provider_revision.isEmpty() &&
               check_only.map_providers[0].name == legacy[0].name,
               "Check configuration without migration write", error)) return 1;
    std::unique_ptr<Aowis::Map::SettingsRepository> other_store =
        Aowis::Map::SettingsRepository::openAtOwnerRoot(
            QStringLiteral("server-map"), check_only.managed_settings_root, &error);
    QVector<Aowis::Map::MapProvider> absent;
    QString absent_revision;
    if (!check(other_store != nullptr &&
               other_store->readProviders(&absent, &absent_revision, &error) &&
               absent_revision.isEmpty(), "No CONFIG state created during validation", error))
        return 1;
    if (!check(writeFile(other_legacy, QByteArrayLiteral("[unexpected]\nkey=value\n")),
               "Damage old provider state")) return 1;
    Server::Config damaged;
    if (!check(!loadServerConfig(other_ini, &damaged, true, &error) &&
               other_store->readProviders(&absent, &absent_revision, nullptr) &&
               absent_revision.isEmpty(),
               "Corrupt legacy state fails without storing defaults", error)) return 1;

    // A CONFIG catalog cannot introduce a coverage profile that the server's
    // administrator INI hasn't enabled.
    saved[0].coverage = QStringLiteral("nonexistent-coverage");
    QString invalid_revision;
    if (!check(store->saveProviders(saved, new_revision, &invalid_revision, &error),
               "Persist provider with server-unknown coverage", error)) return 1;
    Server::Config invalid;
    if (!check(!loadServerConfig(admin_path, &invalid, true, &error) &&
               error.contains(QStringLiteral("coverage")),
               "Reject invalid managed provider coverage", error)) return 1;

    // A live MAP server must persist REST edits to CONFIG (not providers.ini),
    // apply them in memory, and reject stale writes from another process.
    saved[0].coverage.clear();
    QString clean_revision;
    if (!check(store->saveProviders(saved, invalid_revision, &clean_revision, &error),
               "Restore valid catalog for HTTP test", error)) return 1;
    Server::Config live_config;
    if (!check(loadServerConfig(admin_path, &live_config, true, &error),
               "Load server for provider REST test", error)) return 1;
    live_config.port = 0;
    live_config.cache_directory = QDir(temp.path()).filePath(QStringLiteral("map-cache"));
    live_config.terrain_enabled = false;
    Server server(live_config);
    if (!check(server.start(), "Start MAP provider REST test server")) return 1;
    const QUrl endpoint(QStringLiteral("http://127.0.0.1:%1/map/v1/providers")
        .arg(MapManagedSettingsHttpTestAccess::port(server)));
    QNetworkAccessManager network;
    QNetworkRequest request(endpoint);
    request.setRawHeader("X-API-Key", live_config.api_key);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QVector<Aowis::Map::MapProvider> http_edit = live_config.map_providers;
    http_edit[0].name = QStringLiteral("Updated through MAP REST");
    QNetworkReply *reply = network.put(request, Aowis::Map::encodeProviderCatalog(http_edit));
    if (!check(await(application, reply) &&
               reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200,
               "REST provider edit persisted", httpFailureDetails(reply))) return 1;
    reply->deleteLater();
    // The unchanged built-in catalog contains CyclOSM's {s} hostname
    // placeholder. A REST edit must accept and retain it rather than treating
    // the URL template as an invalid literal hostname.
    QNetworkReply *catalog_reply = network.get(request);
    if (!check(await(application, catalog_reply) &&
               catalog_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200,
               "Read provider catalog after REST edit", httpFailureDetails(catalog_reply)))
        return 1;
    QVector<Aowis::Map::MapProvider> persisted_catalog;
    if (!check(Aowis::Map::decodeProviderCatalog(catalog_reply->readAll(), {},
                                                 &persisted_catalog, &error) &&
               persisted_catalog.size() == http_edit.size() &&
               persisted_catalog[1].url_template == http_edit[1].url_template &&
               persisted_catalog[0].name == http_edit[0].name,
               "REST round-trip preserved hostname templates", error)) return 1;
    catalog_reply->deleteLater();
    QVector<Aowis::Map::MapProvider> after_http;
    QString after_http_revision;
    if (!check(store->readProviders(&after_http, &after_http_revision, &error) &&
               after_http[0].name == QStringLiteral("Updated through MAP REST") &&
               after_http_revision != clean_revision &&
               readFile(legacy_path) == legacy_bytes && readFile(admin_path) == admin_ini,
               "REST updated CONFIG only", error)) return 1;

    after_http[0].name = QStringLiteral("Other editor's change");
    QString external_revision;
    if (!check(store->saveProviders(after_http, after_http_revision,
                                    &external_revision, &error),
               "Simulate concurrent external change", error)) return 1;
    http_edit[0].name = QStringLiteral("Outdated REST change");
    QNetworkReply *conflict = network.put(
        request, Aowis::Map::encodeProviderCatalog(http_edit));
    if (!check(await(application, conflict) &&
               conflict->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 409,
               "Stale REST update rejected with 409", httpFailureDetails(conflict))) return 1;
    conflict->deleteLater();
    if (!check(store->readProviders(&after_http, &after_http_revision, &error) &&
               after_http_revision == external_revision &&
               after_http[0].name == QStringLiteral("Other editor's change"),
               "Failed REST write preserved external changes", error)) return 1;

    return 0;
}
