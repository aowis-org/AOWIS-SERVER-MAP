#include <aowis/map/settings_repository.h>

#include <aowis/config/config_root.h>
#include <aowis/config/object_store.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QDir>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QStringList>
#include <QSet>
#include <QUuid>
#include <QUrl>

#include <cmath>
#include <filesystem>
#include <string>
#include <utility>

namespace Aowis::Map
{
namespace
{
constexpr int SettingsVersion = 1;
constexpr int MaximumUpstreams = 32;
constexpr int MaximumProviders = 100;

bool fail(QString *error, const QString &message)
{
    if (error != nullptr)
        *error = message;
    return false;
}

QString configurationError(const aowis::config::Error &error)
{
    // CONFIG errors never contain the secret JSON payload.
    return QString::fromStdString(error.message);
}

bool isInteger(const QJsonValue &value, int minimum, int maximum)
{
    if (!value.isDouble())
        return false;
    const double number = value.toDouble();
    return std::isfinite(number) && number >= minimum && number <= maximum &&
           std::floor(number) == number;
}

QJsonObject encodeCascadeObject(const MapCascadeConfig &value)
{
    QJsonArray upstreams;
    for (const MapCascadeUpstream &upstream : value.upstreams)
    {
        upstreams.append(QJsonObject{
            {QStringLiteral("name"), upstream.name},
            {QStringLiteral("url"), upstream.url.toString(QUrl::FullyEncoded)},
            {QStringLiteral("api_key"), QString::fromUtf8(upstream.api_key)}
        });
    }
    return QJsonObject{
        {QStringLiteral("version"), SettingsVersion},
        {QStringLiteral("upstreams"), upstreams},
        {QStringLiteral("allow_direct_fetch"), value.allow_direct_fetch},
        {QStringLiteral("poor_internet_mode"), value.poor_internet_mode},
        {QStringLiteral("jpeg_quality"), value.jpeg_quality},
        {QStringLiteral("adaptive_compression"), value.adaptive_compression}
    };
}

bool decodeCascadeObject(const QJsonObject &stored, MapCascadeConfig *value, QString *error)
{
    if (!isInteger(stored.value(QStringLiteral("version")), SettingsVersion, SettingsVersion) ||
        !stored.value(QStringLiteral("upstreams")).isArray() ||
        !stored.value(QStringLiteral("allow_direct_fetch")).isBool() ||
        !stored.value(QStringLiteral("poor_internet_mode")).isBool() ||
        !isInteger(stored.value(QStringLiteral("jpeg_quality")), 1, 100) ||
        !stored.value(QStringLiteral("adaptive_compression")).isBool())
    {
        return fail(error, QStringLiteral("Invalid MAP cascade record version or fields"));
    }
    MapCascadeConfig parsed;
    parsed.allow_direct_fetch = stored.value(QStringLiteral("allow_direct_fetch")).toBool();
    parsed.poor_internet_mode = stored.value(QStringLiteral("poor_internet_mode")).toBool();
    parsed.jpeg_quality = stored.value(QStringLiteral("jpeg_quality")).toInt();
    parsed.adaptive_compression = stored.value(QStringLiteral("adaptive_compression")).toBool();
    const QJsonArray upstreams = stored.value(QStringLiteral("upstreams")).toArray();
    if (upstreams.size() > MaximumUpstreams)
        return fail(error, QStringLiteral("Too many MAP upstream servers"));
    for (const QJsonValue &item : upstreams)
    {
        if (!item.isObject())
            return fail(error, QStringLiteral("MAP upstream must be an object"));
        const QJsonObject object = item.toObject();
        if (!object.value(QStringLiteral("name")).isString() ||
            !object.value(QStringLiteral("url")).isString() ||
            !object.value(QStringLiteral("api_key")).isString())
            return fail(error, QStringLiteral("Invalid MAP upstream record"));
        MapCascadeUpstream upstream;
        upstream.name = object.value(QStringLiteral("name")).toString();
        upstream.url = QUrl(object.value(QStringLiteral("url")).toString(), QUrl::StrictMode);
        upstream.api_key = object.value(QStringLiteral("api_key")).toString().toUtf8();
        parsed.upstreams.append(upstream);
    }
    if (!validateCascadeSettings(parsed, error))
        return false;
    *value = parsed;
    return true;
}

QJsonObject providerObject(const MapProvider &provider)
{
    return QJsonObject{
        {QStringLiteral("id"), provider.id},
        {QStringLiteral("name"), provider.name},
        {QStringLiteral("url"), provider.url_template},
        {QStringLiteral("coverage"), provider.coverage},
        {QStringLiteral("minimum_zoom"), provider.minimum_zoom},
        {QStringLiteral("maximum_zoom"), provider.maximum_zoom},
        {QStringLiteral("enabled"), provider.enabled},
        {QStringLiteral("remote_fetch_enabled"), provider.remote_fetch_enabled},
        {QStringLiteral("allow_lossy_transfer"), provider.allow_lossy_transfer}
    };
}

bool decodeProviderArray(const QJsonArray &array, const QVector<MapProvider> &previous,
                         QVector<MapProvider> *providers, QString *error)
{
    if (array.size() > MaximumProviders)
        return fail(error, QStringLiteral("Too many MAP providers"));
    QVector<MapProvider> parsed;
    QSet<QString> known_ids;
    for (const QJsonValue &item : array)
    {
        if (!item.isObject())
            return fail(error, QStringLiteral("MAP provider must be an object"));
        const QJsonObject object = item.toObject();
        const QStringList mandatory = {
            QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("url")};
        for (const QString &field : mandatory)
            if (!object.value(field).isString())
                return fail(error, QStringLiteral("Invalid MAP provider field: %1").arg(field));
        if (!isInteger(object.value(QStringLiteral("minimum_zoom")), 0, 30) ||
            !isInteger(object.value(QStringLiteral("maximum_zoom")), 0, 30))
            return fail(error, QStringLiteral("Invalid MAP provider zoom range"));
        const QStringList booleans = {QStringLiteral("enabled"),
                                      QStringLiteral("remote_fetch_enabled"),
                                      QStringLiteral("allow_lossy_transfer")};
        for (const QString &field : booleans)
            if (object.contains(field) && !object.value(field).isBool())
                return fail(error, QStringLiteral("Invalid MAP provider boolean: %1").arg(field));
        if (object.contains(QStringLiteral("coverage")) &&
            !object.value(QStringLiteral("coverage")).isString())
            return fail(error, QStringLiteral("Invalid MAP provider coverage"));
        MapProvider provider;
        provider.id = object.value(QStringLiteral("id")).toString().trimmed().toLower();
        provider.name = object.value(QStringLiteral("name")).toString().trimmed();
        provider.url_template = object.value(QStringLiteral("url")).toString().trimmed();
        provider.coverage = object.value(QStringLiteral("coverage")).toString().trimmed();
        provider.minimum_zoom = object.value(QStringLiteral("minimum_zoom")).toInt();
        provider.maximum_zoom = object.value(QStringLiteral("maximum_zoom")).toInt();
        provider.enabled = object.value(QStringLiteral("enabled")).toBool(true);
        provider.remote_fetch_enabled = object.value(QStringLiteral("remote_fetch_enabled")).toBool(true);
        provider.allow_lossy_transfer = object.value(QStringLiteral("allow_lossy_transfer")).toBool(true);
        if (!object.contains(QStringLiteral("allow_lossy_transfer")))
            for (const MapProvider &old : previous)
                if (old.id == provider.id)
                    provider.allow_lossy_transfer = old.allow_lossy_transfer;
        QString validation_url = provider.url_template;
        validation_url.replace(QStringLiteral("{s}"), QStringLiteral("a"));
        const QUrl parsed_url(validation_url);
        if (QUuid(provider.id).isNull() || known_ids.contains(provider.id) ||
            provider.name.isEmpty() || provider.name.contains(QLatin1Char('\n')) ||
            provider.name.contains(QLatin1Char('\r')) ||
            provider.url_template.contains(QLatin1Char('\n')) ||
            provider.url_template.contains(QLatin1Char('\r')) ||
            provider.coverage.contains(QLatin1Char('\n')) ||
            provider.coverage.contains(QLatin1Char('\r')) ||
            provider.minimum_zoom > provider.maximum_zoom ||
            (parsed_url.scheme() != QStringLiteral("https") &&
             parsed_url.scheme() != QStringLiteral("http")) ||
            parsed_url.host().isEmpty() || !parsed_url.userInfo().isEmpty() ||
            !provider.url_template.contains(QStringLiteral("{z}")) ||
            !provider.url_template.contains(QStringLiteral("{x}")) ||
            !provider.url_template.contains(QStringLiteral("{y}")))
            return fail(error, QStringLiteral("Invalid MAP provider: %1").arg(provider.id));
        known_ids.insert(provider.id);
        parsed.append(provider);
    }
    *providers = parsed;
    return true;
}

QJsonObject encodeProviderObject(const QVector<MapProvider> &providers)
{
    QJsonArray entries;
    for (const MapProvider &provider : providers)
        entries.append(providerObject(provider));
    return QJsonObject{{QStringLiteral("version"), SettingsVersion},
                       {QStringLiteral("providers"), entries}};
}

bool decodeProviderObject(const QJsonObject &stored, QVector<MapProvider> *providers,
                          QString *error)
{
    if (!isInteger(stored.value(QStringLiteral("version")), SettingsVersion, SettingsVersion) ||
        !stored.value(QStringLiteral("providers")).isArray())
        return fail(error, QStringLiteral("Invalid MAP provider catalog record"));
    // A persisted record must not inherit a policy flag from defaults.
    return decodeProviderArray(stored.value(QStringLiteral("providers")).toArray(),
                               {}, providers, error);
}

struct Record
{
    QString revision;
    QJsonObject json;
};

// Exactly one record per collection; partial corruption or duplicate records
// are *errors*, not missing settings. No fallback to direct network access.
bool readRecord(const aowis::config::ObjectCollection &collection, Record *record,
                QString *error)
{
    const aowis::config::Result<aowis::config::ObjectSnapshot> snapshot = collection.readAll();
    if (!snapshot)
        return fail(error, configurationError(snapshot.error()));
    if (!snapshot.value().unreadable.empty() || snapshot.value().objects.size() > 1)
        return fail(error, QStringLiteral("MAP configuration collection is damaged or ambiguous"));
    if (snapshot.value().objects.empty())
    {
        *record = Record();
        return true;
    }
    const aowis::config::StoredObject &stored = snapshot.value().objects.front();
    QJsonParseError parse_error;
    const QByteArray json(stored.json.data(), qsizetype(stored.json.size()));
    const QJsonDocument document = QJsonDocument::fromJson(json, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject())
        return fail(error, QStringLiteral("Invalid MAP configuration JSON record"));
    record->json = document.object();
    record->revision = QString::fromStdString(stored.info.revision);
    return true;
}

bool writeRecord(const aowis::config::ObjectCollection &collection,
                 const QJsonObject &json, const QString &expected_revision,
                 QString *revision, QString *error)
{
    // Lock the collection's owner across read-and-conditional-write, including
    // first creation. CONFIG ensures cross-process exclusion and atomic writes.
    const aowis::config::OwnerStore store = collection.store();
    const aowis::config::Result<aowis::config::OwnerSession> session = store.lock();
    if (!session)
        return fail(error, configurationError(session.error()));
    const aowis::config::Result<aowis::config::ObjectSnapshot> snapshot =
        collection.readAll(session.value());
    if (!snapshot)
        return fail(error, configurationError(snapshot.error()));
    if (!snapshot.value().unreadable.empty() || snapshot.value().objects.size() > 1)
        return fail(error, QStringLiteral("MAP configuration collection is damaged or ambiguous"));
    const QByteArray serialized = QJsonDocument(json).toJson(QJsonDocument::Compact);
    const std::string contents(serialized.constData(), size_t(serialized.size()));
    aowis::config::Result<aowis::config::StoredObject> written =
        aowis::config::Result<aowis::config::StoredObject>::failure(
            {aowis::config::ErrorCode::io_failure, "Uninitialized MAP write"});
    if (snapshot.value().objects.empty())
    {
        if (!expected_revision.isEmpty())
            return fail(error, QStringLiteral("MAP configuration revision conflict"));
        written = collection.create(session.value(), contents);
    }
    else
    {
        const aowis::config::StoredObject &previous = snapshot.value().objects.front();
        if (expected_revision.toStdString() != previous.info.revision)
            return fail(error, QStringLiteral("MAP configuration revision conflict"));
        written = collection.update(session.value(), previous.info.id, contents,
                                    previous.info.revision);
    }
    if (!written)
        return fail(error, configurationError(written.error()));
    if (revision != nullptr)
        *revision = QString::fromStdString(written.value().info.revision);
    if (error != nullptr)
        error->clear();
    return true;
}

bool readCollection(const aowis::config::OwnerStore &store, const char *name,
                    Record *record, QString *error)
{
    const aowis::config::Result<aowis::config::ObjectCollection> collection = store.collection(name);
    if (!collection)
        return fail(error, configurationError(collection.error()));
    return readRecord(collection.value(), record, error);
}

bool writeCollection(const aowis::config::OwnerStore &store, const char *name,
                     const QJsonObject &json, const QString &expected_revision,
                     QString *revision, QString *error)
{
    const aowis::config::Result<aowis::config::ObjectCollection> collection = store.collection(name);
    if (!collection)
        return fail(error, configurationError(collection.error()));
    return writeRecord(collection.value(), json, expected_revision, revision, error);
}
}

bool validateCascadeSettings(const MapCascadeConfig &value, QString *error)
{
    if (value.jpeg_quality < 1 || value.jpeg_quality > 100 ||
        value.upstreams.size() > MaximumUpstreams)
        return fail(error, QStringLiteral("Invalid MAP JPEG quality or upstream count"));
    QSet<QString> names;
    QSet<QString> urls;
    const QRegularExpression valid_name(QStringLiteral("^[A-Za-z][A-Za-z0-9_-]{0,63}$"));
    for (const MapCascadeUpstream &upstream : value.upstreams)
    {
        // JSON stores the same UTF-8 text that the HTTP client will send.
        // Reject opaque binary keys instead of silently replacing bytes.
        if (QString::fromUtf8(upstream.api_key).toUtf8() != upstream.api_key)
            return fail(error, QStringLiteral("MAP upstream API key is not UTF-8"));
        if (!valid_name.match(upstream.name).hasMatch() || names.contains(upstream.name))
            return fail(error, QStringLiteral("Invalid or duplicate MAP upstream name"));
        names.insert(upstream.name);
        const QUrl &url = upstream.url;
        const QString scheme = url.scheme().toLower();
        if (!url.isValid() || (scheme != QStringLiteral("http") &&
                               scheme != QStringLiteral("https")) ||
            url.host().isEmpty() || !url.userInfo().isEmpty() ||
            !url.query().isEmpty() || !url.fragment().isEmpty() ||
            (!url.path().isEmpty() && url.path() != QStringLiteral("/")) ||
            url.port(-1) == 0)
            return fail(error, QStringLiteral("Invalid MAP upstream URL"));
        const QString normalized = url.toString(QUrl::RemovePath | QUrl::RemoveQuery |
                                                 QUrl::RemoveFragment).toLower();
        if (urls.contains(normalized))
            return fail(error, QStringLiteral("Duplicate MAP upstream URL"));
        urls.insert(normalized);
    }
    if (error != nullptr)
        error->clear();
    return true;
}

QByteArray encodeProviderCatalog(const QVector<MapProvider> &providers)
{
    QJsonArray entries;
    for (const MapProvider &provider : providers)
        entries.append(providerObject(provider));
    return QJsonDocument(QJsonObject{{QStringLiteral("providers"), entries}})
        .toJson(QJsonDocument::Compact);
}

bool decodeProviderCatalog(const QByteArray &payload, const QVector<MapProvider> &previous,
                           QVector<MapProvider> *providers, QString *error)
{
    if (providers == nullptr)
        return fail(error, QStringLiteral("Missing MAP provider destination"));
    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject() ||
        !document.object().value(QStringLiteral("providers")).isArray())
        return fail(error, QStringLiteral("MAP provider JSON must contain a providers array"));
    const bool success = decodeProviderArray(
        document.object().value(QStringLiteral("providers")).toArray(), previous, providers, error);
    if (success && error != nullptr)
        error->clear();
    return success;
}

// Read the old Standalone MAP records as *opaque GUI preference envelopes*.
// This code is deliberately in MAP, not GUI. It exists only to migrate earlier
// released builds; after a successful import MAP never writes the old records.
namespace
{
struct LegacyStandaloneRecords
{
    QJsonObject cascade;
    QJsonObject providers;
    bool has_cascade = false;
    bool has_providers = false;
};

bool readLegacyStandaloneRecords(LegacyStandaloneRecords *result, QString *error)
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (base.isEmpty() || !QDir::isAbsolutePath(base))
        return fail(error, QStringLiteral("No GUI configuration location for MAP migration"));
    const aowis::config::Result<aowis::config::OwnerId> owner =
        aowis::config::OwnerId::parse("gui");
    if (!owner)
        return fail(error, configurationError(owner.error()));
    const std::filesystem::path namespace_root =
        std::filesystem::path(QDir::cleanPath(base).toStdString()) / "aowis";
    const aowis::config::Result<aowis::config::ConfigRoot> root =
        aowis::config::ConfigRoot::fromNamespaceRoot(owner.value(), namespace_root);
    if (!root)
        return fail(error, configurationError(root.error()));
    const aowis::config::Result<aowis::config::OwnerStore> store =
        aowis::config::OwnerStore::open(root.value());
    if (!store)
        return fail(error, configurationError(store.error()));
    const aowis::config::Result<aowis::config::ObjectCollection> collection =
        store.value().collection("preferences");
    if (!collection)
        return fail(error, configurationError(collection.error()));
    const aowis::config::Result<aowis::config::ObjectSnapshot> snapshot =
        collection.value().readAll();
    if (!snapshot)
        return fail(error, configurationError(snapshot.error()));
    if (!snapshot.value().unreadable.empty())
        return fail(error, QStringLiteral("Damaged GUI preferences prevent MAP migration"));
    for (const aowis::config::StoredObject &stored : snapshot.value().objects)
    {
        const QJsonDocument document =
            QJsonDocument::fromJson(QByteArray(stored.json.data(), qsizetype(stored.json.size())));
        if (!document.isObject())
            return fail(error, QStringLiteral("Invalid GUI preference envelope during MAP migration"));
        const QJsonObject object = document.object();
        const QString group = object.value(QStringLiteral("group")).toString();
        const bool cascade = group == QStringLiteral("embedded_map_cascade");
        const bool providers = group == QStringLiteral("embedded_map_providers");
        if (!cascade && !providers)
            continue;
        if (!isInteger(object.value(QStringLiteral("preferences_version")), 1, 1) ||
            !object.value(QStringLiteral("values")).isObject())
            return fail(error, QStringLiteral("Invalid legacy MAP preference envelope"));
        if ((cascade && result->has_cascade) || (providers && result->has_providers))
            return fail(error, QStringLiteral("Duplicate legacy MAP configuration records"));
        if (cascade)
        {
            result->has_cascade = true;
            result->cascade = object.value(QStringLiteral("values")).toObject();
        }
        if (providers)
        {
            result->has_providers = true;
            result->providers = object.value(QStringLiteral("values")).toObject();
        }
    }
    return true;
}
}

struct SettingsRepository::Impl
{
    explicit Impl(aowis::config::OwnerStore opened) : store(std::move(opened)) {}
    aowis::config::OwnerStore store;
};

SettingsRepository::SettingsRepository(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation))
{
}
SettingsRepository::~SettingsRepository() = default;

std::unique_ptr<SettingsRepository> SettingsRepository::openForCurrentUser(
    const QString &owner, QString *error)
{
    const aowis::config::Result<aowis::config::OwnerId> owner_id =
        aowis::config::OwnerId::parse(owner.toStdString());
    if (!owner_id)
    {
        fail(error, configurationError(owner_id.error()));
        return nullptr;
    }
    const aowis::config::Result<aowis::config::ConfigRoot> root =
        aowis::config::ConfigRoot::forCurrentUser(owner_id.value());
    if (!root)
    {
        fail(error, configurationError(root.error()));
        return nullptr;
    }
    const aowis::config::Result<aowis::config::OwnerStore> store =
        aowis::config::OwnerStore::open(root.value());
    if (!store)
    {
        fail(error, configurationError(store.error()));
        return nullptr;
    }
    if (error != nullptr)
        error->clear();
    return std::unique_ptr<SettingsRepository>(
        new SettingsRepository(std::make_unique<Impl>(store.value())));
}

std::unique_ptr<SettingsRepository> SettingsRepository::openStandaloneForCurrentUser(
    QString *error)
{
    std::unique_ptr<SettingsRepository> repository =
        openForCurrentUser(QStringLiteral("standalone-map"), error);
    if (repository && !repository->migrateLegacyStandaloneGuiPreferences(error))
        repository.reset();
    return repository;
}

bool SettingsRepository::migrateLegacyStandaloneGuiPreferences(QString *error) const
{
    // Existing MAP records always win. This also makes the operation safely
    // repeatable after a restart or an interrupted two-record migration.
    MapCascadeConfig cascade;
    QString cascade_revision;
    if (!readCascade(&cascade, &cascade_revision, error))
        return false;
    QVector<MapProvider> providers;
    QString provider_revision;
    if (!readProviders(&providers, &provider_revision, error))
        return false;
    if (!cascade_revision.isEmpty() && !provider_revision.isEmpty())
        return true;

    LegacyStandaloneRecords legacy;
    if (!readLegacyStandaloneRecords(&legacy, error))
        return false;

    // Validate both records before writing either, so malformed old data
    // cannot accidentally switch acquisition back to direct-provider mode.
    MapCascadeConfig imported_cascade;
    QVector<MapProvider> imported_providers;
    if (cascade_revision.isEmpty() && legacy.has_cascade &&
        !decodeCascadeObject(legacy.cascade, &imported_cascade, error))
        return false;
    if (provider_revision.isEmpty() && legacy.has_providers &&
        !decodeProviderObject(legacy.providers, &imported_providers, error))
        return false;
    if (cascade_revision.isEmpty() && legacy.has_cascade &&
        !saveCascade(imported_cascade, QString(), nullptr, error))
        return false;
    if (provider_revision.isEmpty() && legacy.has_providers &&
        !saveProviders(imported_providers, QString(), nullptr, error))
        return false;
    if (error != nullptr)
        error->clear();
    return true;
}

std::unique_ptr<SettingsRepository> SettingsRepository::openAtOwnerRoot(
    const QString &owner, const QString &absolute_root, QString *error)
{
    const aowis::config::Result<aowis::config::OwnerId> owner_id =
        aowis::config::OwnerId::parse(owner.toStdString());
    if (!owner_id)
    {
        fail(error, configurationError(owner_id.error()));
        return nullptr;
    }
    const aowis::config::Result<aowis::config::ConfigRoot> root =
        aowis::config::ConfigRoot::fromOwnerRoot(
            owner_id.value(), std::filesystem::path(absolute_root.toStdString()));
    if (!root)
    {
        fail(error, configurationError(root.error()));
        return nullptr;
    }
    const aowis::config::Result<aowis::config::OwnerStore> store =
        aowis::config::OwnerStore::open(root.value());
    if (!store)
    {
        fail(error, configurationError(store.error()));
        return nullptr;
    }
    if (error != nullptr)
        error->clear();
    return std::unique_ptr<SettingsRepository>(
        new SettingsRepository(std::make_unique<Impl>(store.value())));
}

bool SettingsRepository::readCascade(MapCascadeConfig *value, QString *revision,
                                     QString *error) const
{
    if (value == nullptr)
        return fail(error, QStringLiteral("Missing MAP cascade destination"));
    *value = MapCascadeConfig();
    value->allow_direct_fetch = false; // Fail closed until a record or known-empty store is verified.
    if (revision != nullptr)
        revision->clear();
    Record record;
    if (!readCollection(this->implementation_->store, "cascade", &record, error))
        return false;
    if (record.revision.isEmpty())
    {
        *value = MapCascadeConfig();
        if (error != nullptr) error->clear();
        return true;
    }
    if (!decodeCascadeObject(record.json, value, error))
        return false;
    if (revision != nullptr)
        *revision = record.revision;
    if (error != nullptr)
        error->clear();
    return true;
}

bool SettingsRepository::saveCascade(const MapCascadeConfig &value,
                                     const QString &expected_revision,
                                     QString *revision, QString *error) const
{
    if (!validateCascadeSettings(value, error))
        return false;
    return writeCollection(this->implementation_->store, "cascade", encodeCascadeObject(value),
                           expected_revision, revision, error);
}

bool SettingsRepository::readDisabledInheritedProviders(QSet<QString> *ids,
                                                      QString *revision, QString *error) const
{
    if (ids == nullptr)
        return fail(error, QStringLiteral("Missing inherited provider overrides destination"));
    ids->clear();
    if (revision != nullptr) revision->clear();
    Record record;
    if (!readCollection(this->implementation_->store, "inherited-provider-overrides", &record, error))
        return false;
    if (record.revision.isEmpty())
    {
        if (error != nullptr) error->clear();
        return true;
    }
    if (!isInteger(record.json.value(QStringLiteral("version")), 1, 1) ||
        !record.json.value(QStringLiteral("disabled_ids")).isArray())
        return fail(error, QStringLiteral("Invalid inherited provider overrides record"));
    const QJsonArray entries = record.json.value(QStringLiteral("disabled_ids")).toArray();
    if (entries.size() > 4096)
        return fail(error, QStringLiteral("Too many disabled inherited providers"));
    QSet<QString> parsed;
    for (const QJsonValue &item : entries)
    {
        if (!item.isString())
            return fail(error, QStringLiteral("Invalid inherited provider UUID"));
        const QString id = item.toString();
        const QUuid uuid(id);
        if (uuid.isNull() || uuid.toString(QUuid::WithoutBraces) != id || parsed.contains(id))
            return fail(error, QStringLiteral("Invalid or duplicate inherited provider UUID"));
        parsed.insert(id);
    }
    *ids = parsed;
    if (revision != nullptr) *revision = record.revision;
    if (error != nullptr) error->clear();
    return true;
}

bool SettingsRepository::saveDisabledInheritedProviders(const QSet<QString> &ids,
                                                        const QString &expected_revision,
                                                        QString *revision, QString *error) const
{
    if (ids.size() > 4096)
        return fail(error, QStringLiteral("Too many disabled inherited providers"));
    QStringList sorted = ids.values();
    sorted.sort();
    QJsonArray entries;
    for (const QString &id : sorted)
    {
        const QUuid uuid(id);
        if (uuid.isNull() || uuid.toString(QUuid::WithoutBraces) != id)
            return fail(error, QStringLiteral("Invalid inherited provider UUID"));
        entries.append(id);
    }
    return writeCollection(this->implementation_->store, "inherited-provider-overrides",
                           QJsonObject{{QStringLiteral("version"), 1},
                                       {QStringLiteral("disabled_ids"), entries}},
                           expected_revision, revision, error);
}

bool SettingsRepository::readProviders(QVector<MapProvider> *value, QString *revision,
                                       QString *error) const
{
    if (value == nullptr)
        return fail(error, QStringLiteral("Missing MAP provider destination"));
    value->clear(); // Do not expose default providers after a failed read.
    if (revision != nullptr)
        revision->clear();
    Record record;
    if (!readCollection(this->implementation_->store, "providers", &record, error))
        return false;
    if (record.revision.isEmpty())
    {
        *value = defaultMapProviders();
        if (error != nullptr) error->clear();
        return true;
    }
    if (!decodeProviderObject(record.json, value, error))
        return false;
    if (revision != nullptr)
        *revision = record.revision;
    if (error != nullptr)
        error->clear();
    return true;
}

bool SettingsRepository::saveProviders(const QVector<MapProvider> &value,
                                       const QString &expected_revision,
                                       QString *revision, QString *error) const
{
    QVector<MapProvider> validated;
    if (!decodeProviderCatalog(encodeProviderCatalog(value), value, &validated, error))
        return false;
    return writeCollection(this->implementation_->store, "providers", encodeProviderObject(validated),
                           expected_revision, revision, error);
}
}
