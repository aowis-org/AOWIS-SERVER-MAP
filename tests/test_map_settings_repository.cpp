#include <aowis/map/settings_repository.h>

#include <aowis/config/config_root.h>
#include <aowis/config/object_store.h>

#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUrl>

#include <cstdio>
#include <memory>

namespace
{
bool require(bool condition, const char *message)
{
    if (!condition)
        std::fprintf(stderr, "FAILED: %s\n", message);
    return condition;
}
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QTemporaryDir temporary;
    if (!require(temporary.isValid(), "Temporary directory"))
        return 1;
    const QString root = QDir(temporary.path()).filePath(QStringLiteral("standalone"));
    const QString independent_root = QDir(temporary.path()).filePath(QStringLiteral("server"));
    QString error;
    std::unique_ptr<Aowis::Map::SettingsRepository> store =
        Aowis::Map::SettingsRepository::openAtOwnerRoot(
            QStringLiteral("standalone-map"), root, &error);
    if (!require(store != nullptr, "Open standalone MAP CONFIG repository"))
    {
        std::fprintf(stderr, "%s\n", qPrintable(error));
        return 1;
    }
    Aowis::Map::MapCascadeConfig config;
    QString first_revision;
    if (!require(store->readCascade(&config, &first_revision, &error) &&
                 first_revision.isEmpty() && config.allow_direct_fetch &&
                 config.upstreams.isEmpty(), "Empty repository uses MAP defaults"))
        return 1;
    config.upstreams.append({QStringLiteral("primary"), QUrl(QStringLiteral("https://primary.example:8443")),
                             QByteArray("key-1")});
    config.upstreams.append({QStringLiteral("backup"), QUrl(QStringLiteral("https://secondary.example")),
                             QByteArray("key-2")});
    config.allow_direct_fetch = false;
    config.poor_internet_mode = true;
    config.jpeg_quality = 65;
    config.adaptive_compression = false;
    QString updated_revision;
    if (!require(store->saveCascade(config, first_revision, &updated_revision, &error) &&
                 !updated_revision.isEmpty(), "Persist first cascade configuration"))
        return 1;

    std::unique_ptr<Aowis::Map::SettingsRepository> reopened =
        Aowis::Map::SettingsRepository::openAtOwnerRoot(
            QStringLiteral("standalone-map"), root, &error);
    Aowis::Map::MapCascadeConfig loaded;
    QString loaded_revision;
    if (!require(reopened != nullptr &&
                 reopened->readCascade(&loaded, &loaded_revision, &error) &&
                 loaded_revision == updated_revision && loaded.upstreams.size() == 2 &&
                 loaded.upstreams.at(0).name == QStringLiteral("primary") &&
                 loaded.upstreams.at(1).api_key == QByteArrayLiteral("key-2") &&
                 !loaded.allow_direct_fetch && loaded.poor_internet_mode &&
                 loaded.jpeg_quality == 65 && !loaded.adaptive_compression,
                 "Restart preserves ordered upstreams and all settings"))
        return 1;

    Aowis::Map::MapCascadeConfig newer = loaded;
    newer.jpeg_quality = 80;
    QString final_revision;
    if (!require(reopened->saveCascade(newer, loaded_revision, &final_revision, &error) &&
                 final_revision != loaded_revision, "Revision changes on write"))
        return 1;
    if (!require(!store->saveCascade(config, loaded_revision, nullptr, &error) &&
                 error.contains(QStringLiteral("conflict")),
                 "A stale MAP autosave cannot overwrite newer data"))
        return 1;
    config.upstreams[1].url = config.upstreams[0].url;
    if (!require(!store->saveCascade(config, final_revision, nullptr, &error),
                 "Duplicate upstream origin is rejected"))
        return 1;
    config.upstreams[1].url = QUrl(QStringLiteral("file:///etc/passwd"));
    if (!require(!store->saveCascade(config, final_revision, nullptr, &error),
                 "Non-HTTP upstream is rejected"))
        return 1;
    if (!require(reopened->readCascade(&loaded, &loaded_revision, &error) &&
                 loaded.jpeg_quality == 80 && loaded.upstreams.size() == 2,
                 "Rejected edits do not change persisted configuration"))
        return 1;

    std::unique_ptr<Aowis::Map::SettingsRepository> independent =
        Aowis::Map::SettingsRepository::openAtOwnerRoot(
            QStringLiteral("server-map"), independent_root, &error);
    Aowis::Map::MapCascadeConfig other;
    if (!require(independent != nullptr && independent->readCascade(&other, nullptr, &error) &&
                 other.upstreams.isEmpty() && other.allow_direct_fetch,
                 "Server and Standalone have isolated CONFIG roots"))
        return 1;

    // Inherited enablement is independent from provider definitions and uses
    // revision-checked AOWIS-CONFIG storage.
    QSet<QString> disabled_ids;
    QString inherited_revision;
    const QString inherited_uuid = QStringLiteral("15c2d930-1e4a-4b76-9675-31ea802989fa");
    if (!require(store->readDisabledInheritedProviders(&disabled_ids, &inherited_revision, &error) &&
                 disabled_ids.isEmpty() && inherited_revision.isEmpty(),
                 "Default inherited providers are enabled")) return 1;
    disabled_ids.insert(inherited_uuid);
    QString saved_inherited_revision;
    if (!require(store->saveDisabledInheritedProviders(disabled_ids, inherited_revision,
                                                       &saved_inherited_revision, &error) &&
                 !saved_inherited_revision.isEmpty(),
                 "Persist disabled inherited UUID")) return 1;
    QSet<QString> restored_ids;
    QString restored_revision;
    if (!require(reopened->readDisabledInheritedProviders(&restored_ids, &restored_revision, &error) &&
                 restored_ids == disabled_ids && restored_revision == saved_inherited_revision,
                 "Disabled inherited UUID survives reopening")) return 1;
    if (!require(!store->saveDisabledInheritedProviders({}, inherited_revision, nullptr, &error) &&
                 error.contains(QStringLiteral("revision conflict")),
                 "Reject stale inherited override writes")) return 1;
    if (!require(!store->saveDisabledInheritedProviders({QStringLiteral("bad-uuid")},
                                                        saved_inherited_revision, nullptr, &error),
                 "Reject invalid inherited provider identifiers")) return 1;
    if (!require(reopened->saveDisabledInheritedProviders({}, restored_revision, nullptr, &error),
                 "Clear inherited provider override")) return 1;

    // Routes are neither provider definitions nor global cascade settings.
    QHash<QString, QStringList> routes;
    QString route_revision;
    if (!require(store->readProviderUpstreamRoutes(&routes, &route_revision, &error) &&
                 routes.isEmpty() && route_revision.isEmpty(),
                 "Default provider route is the global upstream order")) return 1;
    routes.insert(inherited_uuid,
        {QStringLiteral("backup"), QStringLiteral("primary")});
    QString saved_route_revision;
    if (!require(store->saveProviderUpstreamRoutes(routes, route_revision,
                                                  &saved_route_revision, &error) &&
                 !saved_route_revision.isEmpty(),
                 "Save per-provider route with revision checking")) return 1;
    QHash<QString, QStringList> restored_routes;
    QString restored_route_revision;
    if (!require(reopened->readProviderUpstreamRoutes(&restored_routes,
                            &restored_route_revision, &error) &&
                 restored_routes == routes && restored_route_revision == saved_route_revision,
                 "Restore routing preferences across process restart")) return 1;
    if (!require(!store->saveProviderUpstreamRoutes(routes, route_revision, nullptr, &error) &&
                 error.contains(QStringLiteral("revision conflict")),
                 "Reject stale provider route changes")) return 1;
    QHash<QString, QStringList> invalid_routes = routes;
    invalid_routes.insert(inherited_uuid, {QStringLiteral("primary"),
                                          QStringLiteral("primary")});
    if (!require(!reopened->saveProviderUpstreamRoutes(invalid_routes,
                    restored_route_revision, nullptr, &error),
                 "Reject duplicate upstream route entries")) return 1;
    invalid_routes.clear();
    invalid_routes.insert(QStringLiteral("invalid-uuid"), {QStringLiteral("primary")});
    if (!require(!reopened->saveProviderUpstreamRoutes(invalid_routes,
                    restored_route_revision, nullptr, &error),
                 "Reject invalid route provider identities")) return 1;
    if (!require(independent->readProviderUpstreamRoutes(&restored_routes, nullptr, &error) &&
                 restored_routes.isEmpty(),
                 "Independent MAP server does not see Standalone routes")) return 1;
    QHash<QString, Aowis::Map::MapProviderAcquisitionMode> modes;
    QString mode_revision;
    if (!require(store->readProviderAcquisitionModes(&modes, &mode_revision, &error) &&
                 modes.isEmpty() && mode_revision.isEmpty(),
                 "Missing acquisition mode record uses provider defaults")) return 1;
    modes.insert(inherited_uuid, Aowis::Map::MapProviderAcquisitionMode::Cascade);
    QString saved_mode_revision;
    if (!require(store->saveProviderAcquisitionModes(modes, mode_revision,
                 &saved_mode_revision, &error) && !saved_mode_revision.isEmpty(),
                 "Save MAP-owned acquisition policy")) return 1;
    QHash<QString, Aowis::Map::MapProviderAcquisitionMode> restored_modes;
    QString restored_mode_revision;
    if (!require(reopened->readProviderAcquisitionModes(&restored_modes,
                    &restored_mode_revision, &error) && restored_modes == modes &&
                 restored_mode_revision == saved_mode_revision,
                 "Read acquisition policy after reopening")) return 1;
    if (!require(!store->saveProviderAcquisitionModes(modes, mode_revision,
                    nullptr, &error) && error.contains(QStringLiteral("revision conflict")),
                 "Reject stale acquisition policy updates")) return 1;
    modes.insert(QStringLiteral("invalid-id"), Aowis::Map::MapProviderAcquisitionMode::Direct);
    if (!require(!reopened->saveProviderAcquisitionModes(modes, restored_mode_revision,
                    nullptr, &error), "Reject invalid mode provider UUID")) return 1;
    if (!require(independent->readProviderAcquisitionModes(&restored_modes, nullptr, &error) &&
                 restored_modes.isEmpty(), "Isolate acquisition policy between MAP owners")) return 1;
    QVector<Aowis::Map::MapProvider> providers;
    QString providers_revision;
    if (!require(store->readProviders(&providers, &providers_revision, &error) &&
                 providers_revision.isEmpty() && providers.size() == 4,
                 "Default MAP provider catalog"))
        return 1;
    providers[0].name = QStringLiteral("Provider, with % and \"quotes\"");
    providers[0].allow_lossy_transfer = false;
    providers[0].coverage = QStringLiteral("projects");
    if (!require(store->saveProviders(providers, providers_revision,
                                      &providers_revision, &error),
                 "Store provider catalog through MAP"))
    {
        std::fprintf(stderr, "%s\n", qPrintable(error));
        return 1;
    }
    QVector<Aowis::Map::MapProvider> providers_after_restart;
    if (!require(reopened->readProviders(&providers_after_restart, nullptr, &error) &&
                 providers_after_restart.at(0).name == providers.at(0).name &&
                 providers_after_restart.at(0).coverage == QStringLiteral("projects") &&
                 !providers_after_restart.at(0).allow_lossy_transfer,
                 "Provider catalog persists with coverage and lossless flag"))
        return 1;
    QJsonObject wire = QJsonDocument::fromJson(
        Aowis::Map::encodeProviderCatalog(providers_after_restart)).object();
    QJsonArray entries = wire.value(QStringLiteral("providers")).toArray();
    QJsonObject first = entries.at(0).toObject();
    first.remove(QStringLiteral("allow_lossy_transfer"));
    entries.replace(0, first);
    wire.insert(QStringLiteral("providers"), entries);
    QVector<Aowis::Map::MapProvider> decoded;
    if (!require(Aowis::Map::decodeProviderCatalog(
                     QJsonDocument(wire).toJson(QJsonDocument::Compact),
                     providers_after_restart, &decoded, &error) &&
                 !decoded.at(0).allow_lossy_transfer,
                 "Older provider editors cannot accidentally re-enable lossy compression"))
        return 1;
    first.insert(QStringLiteral("url"), QStringLiteral("file:///etc/passwd"));
    entries.replace(0, first);
    wire.insert(QStringLiteral("providers"), entries);
    if (!require(!Aowis::Map::decodeProviderCatalog(
                     QJsonDocument(wire).toJson(QJsonDocument::Compact),
                     providers_after_restart, &decoded, &error),
                 "Invalid provider URLs rejected in MAP library"))
        return 1;

    // A WireGuard upstream stores only its profile reference, in a version 2
    // record, so an older MAP release rejects the record instead of fetching
    // from that upstream directly. Unknown or incomplete transports fail.
    Aowis::Map::MapCascadeConfig tunnelled;
    QString tunnel_revision;
    if (!require(reopened->readCascade(&tunnelled, &tunnel_revision, &error),
                 "Read cascade before WireGuard upstream"))
        return 1;
    tunnelled.upstreams.clear();
    Aowis::Map::MapCascadeUpstream via_tunnel;
    via_tunnel.name = QStringLiteral("regional");
    via_tunnel.url = QUrl(QStringLiteral("https://regional.example.invalid"));
    via_tunnel.api_key = QByteArrayLiteral("regional-read");
    via_tunnel.wireguard_profile_id = QStringLiteral("019a47b1-2040-7af9-8d1c-2670badb6399");
    Aowis::Map::MapCascadeUpstream direct_backup;
    direct_backup.name = QStringLiteral("backup");
    direct_backup.url = QUrl(QStringLiteral("https://backup.example.invalid"));
    tunnelled.upstreams.append(via_tunnel);
    tunnelled.upstreams.append(direct_backup);
    Aowis::Map::MapCascadeConfig invalid_profile = tunnelled;
    invalid_profile.upstreams[0].wireguard_profile_id = QStringLiteral("not-a-profile-id");
    if (!require(!reopened->saveCascade(invalid_profile, tunnel_revision, nullptr, &error),
                 "Invalid WireGuard profile reference accepted"))
        return 1;
    QString tunnel_saved_revision;
    if (!require(reopened->saveCascade(tunnelled, tunnel_revision, &tunnel_saved_revision, &error),
                 "Save WireGuard upstream"))
        return 1;
    Aowis::Map::MapCascadeConfig tunnelled_loaded;
    if (!require(reopened->readCascade(&tunnelled_loaded, nullptr, &error) &&
                 tunnelled_loaded.upstreams == tunnelled.upstreams,
                 "WireGuard upstream reference did not round-trip"))
        return 1;
    {
        const aowis::config::Result<aowis::config::OwnerId> transport_owner =
            aowis::config::OwnerId::parse("standalone-map");
        const aowis::config::Result<aowis::config::ConfigRoot> transport_root =
            aowis::config::ConfigRoot::fromOwnerRoot(transport_owner.value(),
                                                    std::filesystem::path(root.toStdString()));
        const aowis::config::Result<aowis::config::OwnerStore> transport_store =
            aowis::config::OwnerStore::open(transport_root.value());
        const aowis::config::Result<aowis::config::ObjectCollection> cascade_records =
            transport_store.value().collection("cascade");
        const aowis::config::Result<aowis::config::ObjectSnapshot> snapshot =
            cascade_records.value().readAll();
        if (!require(snapshot.hasValue() && snapshot.value().objects.size() == 1,
                     "Read raw cascade record"))
            return 1;
        const aowis::config::StoredObject stored = snapshot.value().objects.front();
        const QJsonObject record = QJsonDocument::fromJson(
            QByteArray::fromStdString(stored.json)).object();
        const QJsonArray stored_upstreams = record.value(QStringLiteral("upstreams")).toArray();
        const QJsonObject transport = stored_upstreams.at(0).toObject()
            .value(QStringLiteral("transport")).toObject();
        if (!require(record.value(QStringLiteral("version")).toInt() == 2 &&
                     transport.value(QStringLiteral("type")).toString() == QStringLiteral("wireguard") &&
                     transport.value(QStringLiteral("profile_id")).toString() ==
                         via_tunnel.wireguard_profile_id &&
                     !stored_upstreams.at(1).toObject().contains(QStringLiteral("transport")),
                     "WireGuard upstream not stored as a version 2 profile reference"))
            return 1;
        const QJsonObject unusable[] = {
            QJsonObject{{QStringLiteral("type"), QStringLiteral("carrier-pigeon")}},
            QJsonObject{{QStringLiteral("type"), QStringLiteral("wireguard")}}};
        for (const QJsonObject &planted_transport : unusable)
        {
            QJsonArray planted_upstreams = stored_upstreams;
            QJsonObject planted_upstream = planted_upstreams.at(0).toObject();
            planted_upstream.insert(QStringLiteral("transport"), planted_transport);
            planted_upstreams.replace(0, planted_upstream);
            QJsonObject planted = record;
            planted.insert(QStringLiteral("upstreams"), planted_upstreams);
            const aowis::config::Result<aowis::config::StoredObject> current =
                cascade_records.value().read(stored.info.id);
            const aowis::config::Result<aowis::config::StoredObject> updated =
                cascade_records.value().update(stored.info.id,
                    QJsonDocument(planted).toJson(QJsonDocument::Compact).toStdString(),
                    current.value().info.revision);
            Aowis::Map::MapCascadeConfig refused;
            if (!require(updated.hasValue() &&
                         !reopened->readCascade(&refused, nullptr, &error),
                         "Unusable upstream transport did not fail closed"))
                return 1;
        }
        const aowis::config::Result<aowis::config::StoredObject> current =
            cascade_records.value().read(stored.info.id);
        if (!require(cascade_records.value().update(stored.info.id, stored.json,
                         current.value().info.revision).hasValue(),
                     "Restore valid cascade record"))
            return 1;
    }

    // CONFIG rejects any ambiguous collection, so a second record must not
    // silently become a new default that could re-enable direct fetching.
    const aowis::config::Result<aowis::config::OwnerId> owner =
        aowis::config::OwnerId::parse("standalone-map");
    const aowis::config::Result<aowis::config::ConfigRoot> config_root =
        aowis::config::ConfigRoot::fromOwnerRoot(owner.value(),
                                                std::filesystem::path(root.toStdString()));
    const aowis::config::Result<aowis::config::OwnerStore> raw =
        aowis::config::OwnerStore::open(config_root.value());
    const aowis::config::Result<aowis::config::ObjectCollection> collection =
        raw.value().collection("cascade");
    if (!require(collection.value().create("{\"version\":99}").hasValue(),
                 "Create test duplicate record"))
        return 1;
    Aowis::Map::MapCascadeConfig unsafe;
    unsafe.allow_direct_fetch = true;
    if (!require(!reopened->readCascade(&unsafe, nullptr, &error) &&
                 !unsafe.allow_direct_fetch && unsafe.upstreams.isEmpty(),
                 "Ambiguous MAP configuration fails closed"))
        return 1;
    return 0;
}
