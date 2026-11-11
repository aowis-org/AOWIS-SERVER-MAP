#include <aowis/map/settings_repository.h>

#include <aowis/config/config_root.h>
#include <aowis/config/object_store.h>

#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

namespace
{
bool check(bool condition, const char *message)
{
    if (!condition)
        std::fprintf(stderr, "FAILED: %s\n", message);
    return condition;
}

std::string envelope(const QString &group, const QJsonObject &values)
{
    return QJsonDocument(QJsonObject{
        {QStringLiteral("preferences_version"), 1},
        {QStringLiteral("group"), group},
        {QStringLiteral("values"), values}}).toJson(QJsonDocument::Compact).toStdString();
}
}

int main(int argc, char **argv)
{
    QTemporaryDir temporary;
    if (!check(temporary.isValid(), "temporary XDG config root"))
        return 1;
    qputenv("XDG_CONFIG_HOME", temporary.path().toUtf8());
    QCoreApplication app(argc, argv);
    const aowis::config::Result<aowis::config::OwnerId> owner =
        aowis::config::OwnerId::parse("gui");
    const aowis::config::Result<aowis::config::ConfigRoot> root =
        aowis::config::ConfigRoot::fromNamespaceRoot(
            owner.value(), std::filesystem::path(temporary.path().toStdString()) / "aowis");
    const aowis::config::Result<aowis::config::OwnerStore> store =
        aowis::config::OwnerStore::open(root.value());
    const aowis::config::Result<aowis::config::ObjectCollection> collection =
        store.value().collection("preferences");
    if (!check(collection.hasValue(), "open GUI legacy preferences"))
        return 1;

    const QJsonObject invalid_cascade{
        {QStringLiteral("version"), 1},
        {QStringLiteral("upstreams"), QJsonArray{QJsonObject{
            {QStringLiteral("name"), QStringLiteral("primary")},
            {QStringLiteral("url"), QStringLiteral("file:///bad")},
            {QStringLiteral("api_key"), QStringLiteral("private-key")}}}},
        {QStringLiteral("allow_direct_fetch"), false},
        {QStringLiteral("poor_internet_mode"), true},
        {QStringLiteral("jpeg_quality"), 66},
        {QStringLiteral("adaptive_compression"), true}};
    const aowis::config::Result<aowis::config::StoredObject> legacy_cascade =
        collection.value().create(envelope(QStringLiteral("embedded_map_cascade"), invalid_cascade));
    if (!check(legacy_cascade.hasValue(), "create old cascade preference"))
        return 1;

    QVector<Aowis::Map::MapProvider> providers = Aowis::Map::defaultMapProviders();
    providers[0].name = QStringLiteral("Existing locally edited provider");
    providers[0].allow_lossy_transfer = false;
    QJsonObject provider_legacy = QJsonDocument::fromJson(
        Aowis::Map::encodeProviderCatalog(providers)).object();
    provider_legacy.insert(QStringLiteral("version"), 1);
    if (!check(collection.value().create(
            envelope(QStringLiteral("embedded_map_providers"), provider_legacy)).hasValue(),
            "create old provider preference"))
        return 1;

    QString error;
    std::unique_ptr<Aowis::Map::SettingsRepository> migrated =
        Aowis::Map::SettingsRepository::openStandaloneForCurrentUser(&error);
    if (!check(migrated == nullptr, "invalid legacy upstream cannot migrate"))
        return 1;

    QJsonObject valid_cascade = invalid_cascade;
    QJsonArray upstreams = valid_cascade.value(QStringLiteral("upstreams")).toArray();
    QJsonObject first = upstreams.at(0).toObject();
    first.insert(QStringLiteral("url"), QStringLiteral("https://primary.example:8443"));
    upstreams.replace(0, first);
    valid_cascade.insert(QStringLiteral("upstreams"), upstreams);
    if (!check(collection.value().update(
            legacy_cascade.value().info.id,
            envelope(QStringLiteral("embedded_map_cascade"), valid_cascade),
            legacy_cascade.value().info.revision).hasValue(), "repair legacy upstream"))
        return 1;

    migrated = Aowis::Map::SettingsRepository::openStandaloneForCurrentUser(&error);
    if (!check(migrated != nullptr, "migrate valid GUI legacy records"))
    {
        std::fprintf(stderr, "%s\n", qPrintable(error));
        return 1;
    }
    Aowis::Map::MapCascadeConfig cascade;
    QString revision;
    if (!check(migrated->readCascade(&cascade, &revision, &error) &&
               !revision.isEmpty() && cascade.upstreams.size() == 1 &&
               cascade.upstreams.at(0).api_key == QByteArrayLiteral("private-key") &&
               !cascade.allow_direct_fetch && cascade.poor_internet_mode &&
               cascade.jpeg_quality == 66, "legacy cascade preserved exactly"))
        return 1;
    QVector<Aowis::Map::MapProvider> migrated_providers;
    if (!check(migrated->readProviders(&migrated_providers, nullptr, &error) &&
               migrated_providers.at(0).name == providers.at(0).name &&
               !migrated_providers.at(0).allow_lossy_transfer,
               "legacy providers preserved exactly"))
        return 1;

    // Subsequent openings must not clobber changes written directly to MAP.
    cascade.jpeg_quality = 55;
    if (!check(migrated->saveCascade(cascade, revision, &revision, &error),
               "update migrated MAP record"))
        return 1;
    migrated = Aowis::Map::SettingsRepository::openStandaloneForCurrentUser(&error);
    if (!check(migrated != nullptr && migrated->readCascade(&cascade, nullptr, &error) &&
               cascade.jpeg_quality == 55, "migration idempotent after edits"))
        return 1;
    return 0;
}
