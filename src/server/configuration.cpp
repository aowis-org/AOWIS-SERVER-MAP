#include "configuration.h"

#include <aowis/map/terrain_tile.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QHostAddress>
#include <QIODevice>
#include <QSaveFile>
#include <QSettings>
#include <QUuid>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QtGlobal>

#include <limits>
#include <QRegularExpression>
#include <QMap>
#include <QUrl>
#include <algorithm>

namespace
{
// Qt INI files use ';' for comments. Older AOWIS templates emitted '#'
// comments, so normalize those lines into an owner-only temporary copy.
// Never edit the original administrator-owned file.
bool compatibleIniPath(const QString &source_path, QTemporaryFile *temporary,
                       QString *effective_path, QString *error_message)
{
    QFile source(source_path);
    if (!source.open(QIODevice::ReadOnly))
    {
        *error_message = QStringLiteral("Cannot read INI file '%1': %2")
                             .arg(source_path, source.errorString());
        return false;
    }
    const QByteArray contents = source.readAll();
    if (source.error() != QFileDevice::NoError)
    {
        *error_message = QStringLiteral("Cannot read INI file '%1': %2")
                             .arg(source_path, source.errorString());
        return false;
    }

    QByteArray normalized;
    normalized.reserve(contents.size());
    bool found_legacy_comments = false;
    qsizetype begin = 0;
    while (begin < contents.size())
    {
        qsizetype end = contents.indexOf('\n', begin);
        if (end < 0)
            end = contents.size();

        qsizetype first = begin;
        while (first < end && (contents.at(first) == ' ' || contents.at(first) == '\t'))
            ++first;
        if (first < end && contents.at(first) == '#')
        {
            normalized.append(contents.constData() + begin, first - begin);
            normalized.append(';');
            normalized.append(contents.constData() + first + 1, end - first - 1);
            found_legacy_comments = true;
        }
        else
        {
            normalized.append(contents.constData() + begin, end - begin);
        }
        if (end < contents.size())
            normalized.append('\n');
        begin = end + 1;
    }

    if (!found_legacy_comments)
    {
        *effective_path = source_path;
        return true;
    }
    // QTemporaryFile creates the temporary file with owner-only permissions.
    if (!temporary->open() || temporary->write(normalized) != normalized.size() ||
        !temporary->flush())
    {
        *error_message = QStringLiteral("Cannot prepare legacy INI comments in '%1': %2")
                             .arg(source_path, temporary->errorString());
        return false;
    }
    *effective_path = temporary->fileName();
    return true;
}

QString joinedValue(const QVariant &value)
{
    return value.toStringList().join(QStringLiteral(","));
}

QString resolveDirectory(const QFileInfo &config_file, const QString &directory)
{
    const QFileInfo info(directory);
    return info.isAbsolute() ? QDir::cleanPath(directory)
        : QDir::cleanPath(QDir(config_file.absolutePath()).absoluteFilePath(directory));
}

bool readPositiveInt(QSettings *settings, const QString &key, int maximum, int *value,
                     QString *error_message)
{
    if (!settings->contains(key))
        return true;

    bool valid = false;
    const int parsed_value = settings->value(key).toString().toInt(&valid);
    if (!valid || parsed_value <= 0 || parsed_value > maximum)
    {
        *error_message = QStringLiteral("Invalid positive integer for '%1': %2")
                             .arg(key, settings->value(key).toString());
        return false;
    }

    *value = parsed_value;
    return true;
}

bool readBoolean(QSettings *settings, const QString &key, bool *value, QString *error_message)
{
    if (!settings->contains(key))
        return true;

    const QString text = settings->value(key).toString().trimmed().toLower();
    if (text == QStringLiteral("true") || text == QStringLiteral("1") ||
        text == QStringLiteral("yes") || text == QStringLiteral("on"))
    {
        *value = true;
        return true;
    }
    if (text == QStringLiteral("false") || text == QStringLiteral("0") ||
        text == QStringLiteral("no") || text == QStringLiteral("off"))
    {
        *value = false;
        return true;
    }

    *error_message = QStringLiteral("Invalid boolean for '%1': %2")
                         .arg(key, settings->value(key).toString());
    return false;
}

// The administrator INI and the optional application-managed state file use
// the same provider schema. An explicit empty list is not a request for defaults.
bool readMapProviderCatalog(QSettings *settings, Server::Config *config,
                            QString *error_message)
{
    const QStringList groups = settings->childGroups();
    QStringList configured_map_groups;
    for (const QString &group : groups)
        if (group.startsWith(QStringLiteral("map_provider.")))
            configured_map_groups.append(group);
    if (!configured_map_groups.isEmpty() || settings->value(QStringLiteral("map_providers/configured"), false).toBool())
    {
        config->map_providers.clear();
        const QVector<Aowis::Map::MapProvider> defaults = Aowis::Map::defaultMapProviders();
        for (const QString &group : configured_map_groups)
        {
            const QString stored_id = group.mid(13);
            QString id = stored_id;
            // Existing pre-UUID provider sections are accepted without rewriting
            // their administrator-owned configuration on startup.
            if (stored_id == QStringLiteral("openstreetmap")) id = defaults.at(0).id;
            else if (stored_id == QStringLiteral("osmcyclo")) id = defaults.at(1).id;
            else if (stored_id == QStringLiteral("opentopomap")) id = defaults.at(2).id;
            else if (stored_id == QStringLiteral("arcgis")) id = defaults.at(3).id;
            if (QUuid(id).isNull())
            {
                *error_message = QStringLiteral("Map provider id must be a UUID: %1").arg(stored_id);
                return false;
            }
            Aowis::Map::MapProvider provider;
            for (const auto &known : defaults)
                if (known.id == id) provider = known;
            provider.id = id;
            settings->beginGroup(group);
            provider.name = settings->value(QStringLiteral("name"), provider.name).toString();
            provider.url_template = settings->value(QStringLiteral("url"), provider.url_template).toString();
            provider.coverage = settings->value(QStringLiteral("coverage")).toString().trimmed();
            const QString min_text = settings->value(QStringLiteral("min_zoom"), provider.minimum_zoom).toString();
            const QString max_text = settings->value(QStringLiteral("max_zoom"), provider.maximum_zoom).toString();
            bool valid_min = false;
            bool valid_max = false;
            provider.minimum_zoom = min_text.toInt(&valid_min);
            provider.maximum_zoom = max_text.toInt(&valid_max);
            if (!valid_min || !valid_max)
            {
                *error_message = QStringLiteral("Invalid zoom limits in %1").arg(group);
                settings->endGroup();
                return false;
            }
            if (!readBoolean(settings, QStringLiteral("enabled"), &provider.enabled, error_message) ||
                !readBoolean(settings, QStringLiteral("remote_fetch_enabled"),
                             &provider.remote_fetch_enabled, error_message))
            {
                settings->endGroup();
                return false;
            }
            settings->endGroup();
            const QUrl url(provider.url_template);
            if (provider.url_template.isEmpty() ||
                !(url.scheme() == QStringLiteral("https") || url.scheme() == QStringLiteral("http")) ||
                !provider.url_template.contains(QStringLiteral("{z}")) ||
                !provider.url_template.contains(QStringLiteral("{x}")) ||
                !provider.url_template.contains(QStringLiteral("{y}")) ||
                provider.minimum_zoom < 0 || provider.maximum_zoom > 30 ||
                provider.minimum_zoom > provider.maximum_zoom ||
                !config->download_coverage.hasProfile(provider.coverage))
            {
                *error_message = QStringLiteral("Invalid URL, zoom limits or coverage for map provider '%1'")
                                    .arg(id);
                return false;
            }
            for (const Aowis::Map::MapProvider &previous : config->map_providers)
            {
                if (previous.id == provider.id)
                {
                    *error_message = QStringLiteral("Duplicate map provider UUID: %1").arg(provider.id);
                    return false;
                }
            }
            config->map_providers.append(provider);
        }
    }

    return true;
}
}

QString MapServerConfiguration::defaultUserDataDirectory()
{
    const QString data_directory = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return QDir(data_directory).filePath(QStringLiteral("aowis-server-map"));
}

QString MapServerConfiguration::defaultCacheDirectory()
{
    const QString systemd_cache_directory = qEnvironmentVariable("CACHE_DIRECTORY").trimmed();
    if (!systemd_cache_directory.isEmpty())
        return QDir::cleanPath(systemd_cache_directory);

    return defaultUserDataDirectory();
}

QString MapServerConfiguration::defaultProviderStateFilePath()
{
    // StateDirectory= is supplied by systemd; use a user's writable data
    // directory for ordinary non-systemd executions.
    const QString systemd_state_directory = qEnvironmentVariable("STATE_DIRECTORY").trimmed();
    const QString state_directory = systemd_state_directory.isEmpty()
        ? defaultUserDataDirectory()
        : systemd_state_directory.split(QLatin1Char(':'), Qt::SkipEmptyParts).first();
    return QDir(state_directory).filePath(QStringLiteral("providers.ini"));
}

QString MapServerConfiguration::defaultConfigFilePath()
{
    return QDir(defaultUserDataDirectory()).filePath(QStringLiteral("map-server.ini"));
}

bool MapServerConfiguration::loadConfigFile(const QString &path, Server::Config *config,
                                             QString *error_message)
{
    const QFileInfo file_info(path);
    if (!file_info.exists())
    {
        *error_message = QStringLiteral("Configuration file does not exist: %1").arg(path);
        return false;
    }
    if (!file_info.isFile())
    {
        *error_message = QStringLiteral("Configuration path is not a file: %1").arg(path);
        return false;
    }
    if (!file_info.isReadable())
    {
        *error_message = QStringLiteral("Configuration file is not readable: %1").arg(path);
        return false;
    }

    QTemporaryFile compatible_file(QDir(QDir::tempPath()).filePath(
        QStringLiteral("aowis-server-map-ini-XXXXXX.ini")));
    QString effective_path;
    if (!compatibleIniPath(path, &compatible_file, &effective_path, error_message))
        return false;
    QSettings settings(effective_path, QSettings::IniFormat);

    if (settings.contains(QStringLiteral("server/listen_address")))
    {
        const QString address_text = settings.value(QStringLiteral("server/listen_address")).toString().trimmed();
        const QHostAddress listen_address(address_text);
        if (listen_address.isNull())
        {
            *error_message = QStringLiteral("Invalid listen address in configuration: %1").arg(address_text);
            return false;
        }
        config->listen_address = listen_address;
    }

    int port = config->port;
    if (!readPositiveInt(&settings, QStringLiteral("server/port"), 65535, &port, error_message))
        return false;
    config->port = quint16(port);

    if (!readPositiveInt(&settings, QStringLiteral("server/max_pending_requests"),
                         std::numeric_limits<int>::max(), &config->maximum_pending_requests, error_message))
    {
        return false;
    }

    if (!readPositiveInt(&settings, QStringLiteral("terrain/maximum_jobs"),
                         std::numeric_limits<int>::max(), &config->maximum_terrain_jobs, error_message))
        return false;

    if (!readPositiveInt(&settings, QStringLiteral("terrain/maximum_workers"),
                         256, &config->maximum_terrain_workers, error_message))
        return false;

    if (!readPositiveInt(&settings, QStringLiteral("terrain/maximum_remote_workers"),
                         256, &config->maximum_terrain_remote_workers, error_message))
        return false;

    if (!readPositiveInt(&settings, QStringLiteral("terrain/connect_timeout_seconds"),
                         3600, &config->terrain_connect_timeout_seconds, error_message) ||
        !readPositiveInt(&settings, QStringLiteral("terrain/inactivity_timeout_seconds"),
                         3600, &config->terrain_inactivity_timeout_seconds, error_message) ||
        !readPositiveInt(&settings, QStringLiteral("terrain/download_deadline_seconds"),
                         86400, &config->terrain_download_deadline_seconds, error_message))
        return false;

    if (!readPositiveInt(&settings, QStringLiteral("downloads/max_active_downloads"),
                         std::numeric_limits<int>::max(), &config->maximum_active_downloads, error_message))
    {
        return false;
    }

    if (settings.contains(QStringLiteral("downloads/cache_directory")))
    {
        const QString configured_cache_directory =
            settings.value(QStringLiteral("downloads/cache_directory")).toString().trimmed();
        if (!configured_cache_directory.isEmpty())
        {
            const QFileInfo cache_info(configured_cache_directory);
            config->cache_directory = cache_info.isAbsolute()
                ? QDir::cleanPath(configured_cache_directory)
                : QDir::cleanPath(QDir(file_info.absolutePath()).absoluteFilePath(configured_cache_directory));
        }
    }

    if (!readBoolean(&settings, QStringLiteral("downloads/remote_fetch_enabled"),
                     &config->map_remote_fetch_enabled, error_message))
    {
        return false;
    }

    if (settings.contains(QStringLiteral("storage/permanent_data_directory")))
    {
        const QString value = settings.value(QStringLiteral("storage/permanent_data_directory"))
                                      .toString().trimmed();
        if (!value.isEmpty())
            config->permanent_data_directory = resolveDirectory(file_info, value);
    }

    if (settings.contains(QStringLiteral("storage/provider_state_file")))
    {
        const QString value = settings.value(QStringLiteral("storage/provider_state_file"))
                                  .toString().trimmed();
        if (!value.isEmpty())
            config->provider_state_file = resolveDirectory(file_info, value);
    }

    // Geographic regions are independent of provider types. A profile is a
    // series of zoom starting points, and each tier can name several regions.
    if (settings.contains(QStringLiteral("regions/geojson_file")))
    {
        const QString value = settings.value(QStringLiteral("regions/geojson_file"))
                                  .toString().trimmed();
        if (!value.isEmpty() && !config->download_coverage.addGeoJsonRegions(
                resolveDirectory(file_info, value), error_message))
            return false;
    }
    const QStringList groups = settings.childGroups();
    for (const QString &group : groups)
    {
        if (!group.startsWith(QStringLiteral("region."))) continue;
        const QString id = group.mid(7);
        settings.beginGroup(group);
        const QStringList keys = settings.childKeys();
        if (keys.isEmpty())
        {
            *error_message = QStringLiteral("Region '%1' has no rectangles").arg(id);
            settings.endGroup();
            return false;
        }
        for (const QString &key : keys)
        {
            if (key != QStringLiteral("bbox") && !key.startsWith(QStringLiteral("bbox_")))
            {
                *error_message = QStringLiteral("Unknown key in region '%1': %2").arg(id, key);
                settings.endGroup();
                return false;
            }
            const QStringList coordinates = joinedValue(settings.value(key)).split(',');
            if (coordinates.size() != 4)
            {
                *error_message = QStringLiteral("Region '%1': %2 requires west,south,east,north")
                                    .arg(id, key);
                settings.endGroup();
                return false;
            }
            Aowis::Map::DownloadCoverage::Bounds bounds;
            bool ok[4];
            bounds.west = coordinates[0].trimmed().toDouble(&ok[0]);
            bounds.south = coordinates[1].trimmed().toDouble(&ok[1]);
            bounds.east = coordinates[2].trimmed().toDouble(&ok[2]);
            bounds.north = coordinates[3].trimmed().toDouble(&ok[3]);
            if (!ok[0] || !ok[1] || !ok[2] || !ok[3] ||
                !config->download_coverage.addRectangle(id, bounds, error_message))
            {
                if (error_message->isEmpty())
                    *error_message = QStringLiteral("Invalid bbox in region '%1'").arg(id);
                settings.endGroup();
                return false;
            }
        }
        settings.endGroup();
    }
    for (const QString &group : groups)
    {
        if (!group.startsWith(QStringLiteral("coverage."))) continue;
        const QString id = group.mid(9);
        settings.beginGroup(group);
        QMap<int, QStringList> starts;
        for (const QString &key : settings.childKeys())
        {
            const QRegularExpression match_expression(QStringLiteral("^zoom_([0-9]|[12][0-9]|30)$"));
            const auto match = match_expression.match(key);
            if (!match.hasMatch())
            {
                *error_message = QStringLiteral("Invalid coverage tier '%1/%2'; expected zoom_N")
                                    .arg(group, key);
                settings.endGroup();
                return false;
            }
            const int zoom = match.captured(1).toInt();
            QStringList region_names = joinedValue(settings.value(key)).split(',', Qt::SkipEmptyParts);
            for (QString &region : region_names) region = region.trimmed();
            if (region_names.isEmpty() || starts.contains(zoom))
            {
                *error_message = QStringLiteral("Empty/duplicate tier '%1/%2'").arg(group, key);
                settings.endGroup();
                return false;
            }
            starts.insert(zoom, region_names);
        }
        settings.endGroup();
        if (starts.isEmpty() || starts.firstKey() != 0)
        {
            *error_message = QStringLiteral("Coverage '%1' must start with zoom_0").arg(id);
            return false;
        }
        Aowis::Map::DownloadCoverage::Profile profile;
        for (auto it = starts.cbegin(); it != starts.cend(); ++it)
        {
            auto next = it;
            ++next;
            profile.tiers.append({it.key(), next == starts.cend() ? 30 : next.key() - 1,
                                  it.value()});
        }
        if (!config->download_coverage.addProfile(id, profile, error_message)) return false;
    }
    config->map_coverage = settings.value(QStringLiteral("downloads/coverage"))
                                     .toString().trimmed();
    config->terrain_coverage = settings.value(QStringLiteral("terrain/coverage"))
                                         .toString().trimmed();
    if (!config->download_coverage.hasProfile(config->map_coverage) ||
        !config->download_coverage.hasProfile(config->terrain_coverage))
    {
        *error_message = QStringLiteral("Unknown global download coverage profile");
        return false;
    }

    if (!readMapProviderCatalog(&settings, config, error_message))
        return false;

    if (!readBoolean(&settings, QStringLiteral("terrain/enabled"),
                     &config->terrain_enabled, error_message))
    {
        return false;
    }
    if (!readBoolean(&settings, QStringLiteral("terrain/remote_fetch_enabled"),
                     &config->terrain_remote_fetch_enabled, error_message))
    {
        return false;
    }

    if (settings.contains(QStringLiteral("terrain/default_dataset")))
    {
        const QString dataset = settings.value(
            QStringLiteral("terrain/default_dataset")).toString().trimmed();
        if (!Aowis::Map::isValidTerrainDatasetId(dataset))
        {
            *error_message = QStringLiteral("Invalid terrain default dataset identifier: %1")
                                 .arg(dataset);
            return false;
        }
        config->terrain_default_dataset = dataset;
    }

    if (settings.contains(QStringLiteral("terrain/cache_directory")))
    {
        const QString configured_terrain_cache_directory =
            settings.value(QStringLiteral("terrain/cache_directory")).toString().trimmed();
        if (!configured_terrain_cache_directory.isEmpty())
        {
            const QFileInfo terrain_cache_info(configured_terrain_cache_directory);
            config->terrain_cache_directory = terrain_cache_info.isAbsolute()
                ? QDir::cleanPath(configured_terrain_cache_directory)
                : QDir::cleanPath(QDir(file_info.absolutePath())
                                      .absoluteFilePath(configured_terrain_cache_directory));
        }
        else
        {
            config->terrain_cache_directory.clear();
        }
    }

    for (const QString &group : groups)
    {
        if (group.startsWith(QStringLiteral("terrain_provider.")) &&
            group != QStringLiteral("terrain_provider.copernicus"))
        {
            *error_message = QStringLiteral("Unsupported terrain provider: %1").arg(group);
            return false;
        }
    }
    if (groups.contains(QStringLiteral("terrain_provider.copernicus")))
    {
        settings.beginGroup(QStringLiteral("terrain_provider.copernicus"));
        bool provider_fetch_enabled = true;
        if (!readBoolean(&settings, QStringLiteral("enabled"), &config->copernicus_enabled, error_message) ||
            !readBoolean(&settings, QStringLiteral("remote_fetch_enabled"),
                         &provider_fetch_enabled, error_message))
        {
            settings.endGroup();
            return false;
        }
        config->terrain_remote_fetch_enabled = config->terrain_remote_fetch_enabled && provider_fetch_enabled;
        const QString override_coverage = settings.value(QStringLiteral("coverage"))
                                             .toString().trimmed();
        if (!override_coverage.isEmpty()) config->terrain_coverage = override_coverage;
        settings.endGroup();
        if (!config->download_coverage.hasProfile(config->terrain_coverage))
        {
            *error_message = QStringLiteral("Unknown Copernicus coverage profile");
            return false;
        }
    }

    if (settings.contains(QStringLiteral("authentication/api_key")))
        config->api_key = settings.value(QStringLiteral("authentication/api_key")).toString().toUtf8();

    if (settings.contains(QStringLiteral("authentication/delete_api_key")))
    {
        config->delete_api_key = settings.value(QStringLiteral("authentication/delete_api_key"))
                                     .toString()
                                     .toUtf8();
    }

    if (settings.status() == QSettings::AccessError)
    {
        *error_message = QStringLiteral("Failed to access configuration file: %1").arg(path);
        return false;
    }
    if (settings.status() == QSettings::FormatError)
    {
        *error_message = QStringLiteral("Invalid INI syntax in configuration file: %1").arg(path);
        return false;
    }

    return true;
}

QByteArray MapServerConfiguration::defaultConfigText()
{
    const Server::Config defaults;
    return QStringLiteral(
        "; AOWIS map server configuration. The server never edits this file on startup.\n"
        "; Use --check-config to validate, --print-default-config for a fresh example.\n"
        "[server]\n"
        "listen_address=%1\nport=%2\nmax_pending_requests=%3\n\n"
        "[downloads]\n"
        "; 2D map downloads, independently of terrain. Existing local tiles remain available.\n"
        "remote_fetch_enabled=true\nmax_active_downloads=%4\ncache_directory=\n"
        "; Empty = unrestricted; example below: coverage=projects\n"
        "coverage=\n\n"
        "[terrain]\n"
        "enabled=true\nremote_fetch_enabled=true\n"
        "default_dataset=%5\nmaximum_jobs=%6\nmaximum_workers=%7\nmaximum_remote_workers=%8\n"
        "connect_timeout_seconds=30\ninactivity_timeout_seconds=120\n"
        "download_deadline_seconds=1800\ncache_directory=\ncoverage=\n\n"
        "[storage]\n"
        "; Permanent imported tiles and terrain packages; not a disposable cache.\n"
        "; Empty means <terrain cache>/offline-packages for legacy terrain packages.\n"
        "permanent_data_directory=\n"
        "; Optional override for GUI-managed provider state. Normally /var/lib via systemd.\n"
        "provider_state_file=\n\n"
        "; Uncomment the regions and profile for multiple distant water projects.\n"
        "; [region.chad_country]\n"
        "; bbox=13.5,7.2,24.1,23.5\n"
        "; [region.peru_country]\n"
        "; bbox=-81.4,-18.4,-68.6,0.0\n"
        "; [region.chad_project]\n"
        "; bbox=14.10,11.80,15.10,12.60\n"
        "; [region.peru_project]\n"
        "; bbox=-76.30,-12.40,-76.00,-12.10\n"
        "; [coverage.projects]\n"
        "; zoom_0=world\n"
        "; zoom_7=chad_country,peru_country\n"
        "; zoom_12=chad_project,peru_project\n\n"
        "; Optional country polygons: [regions] geojson_file=regions.geojson\n"
        "; Regions from that GeoJSON are named by their ISO_A2 (or id) property.\n\n"
        "; Provider list is authoritative once any [map_provider.*] section exists.\n"
        "; Deleting/commenting one of these sections removes that default provider.\n"
        "[map_provider.0381bc44-67e9-5af9-aeb0-023ccba36658]\nname=OpenStreetMap\n"
        "url=https://tile.openstreetmap.org/{z}/{x}/{y}.png\n"
        "min_zoom=0\nmax_zoom=19\nenabled=true\nremote_fetch_enabled=true\n\n"
        "[map_provider.49bda1b9-15aa-55e6-8933-2172c785e775]\nname=CyclOSM\n"
        "url=https://{s}.tile-cyclosm.openstreetmap.fr/cyclosm/{z}/{x}/{y}.png\n"
        "min_zoom=0\nmax_zoom=20\nenabled=true\nremote_fetch_enabled=true\n\n"
        "[map_provider.178a60e6-178a-507b-af86-f45427150986]\nname=OpenTopoMap\n"
        "url=https://tile.opentopomap.org/{z}/{x}/{y}.png\n"
        "min_zoom=0\nmax_zoom=17\nenabled=true\nremote_fetch_enabled=true\n\n"
        "[map_provider.a0d790c2-f231-55fd-b95e-23c9bd6cb3fc]\nname=ArcGIS World Imagery\n"
        "url=https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}\n"
        "min_zoom=0\nmax_zoom=17\nenabled=true\nremote_fetch_enabled=true\n\n"
        "[terrain_provider.copernicus]\n"
        "enabled=true\nremote_fetch_enabled=true\n"
        "; coverage=projects  (optional provider-specific override)\n\n"
        "[authentication]\napi_key=\ndelete_api_key=\n"
    ).arg(defaults.listen_address.toString())
     .arg(defaults.port)
     .arg(defaults.maximum_pending_requests)
     .arg(defaults.maximum_active_downloads)
     .arg(defaults.terrain_default_dataset)
     .arg(defaults.maximum_terrain_jobs)
     .arg(defaults.maximum_terrain_workers)
     .arg(defaults.maximum_terrain_remote_workers).toUtf8();
}

bool MapServerConfiguration::writeDefaultConfigFile(const QString &path, bool overwrite,
                                                      QString *error_message)
{
    if (path.trimmed().isEmpty())
    {
        *error_message = QStringLiteral("The default configuration output path must not be empty");
        return false;
    }

    const QFileInfo file_info(path);
    if (file_info.exists() && !overwrite)
    {
        *error_message = QStringLiteral("Configuration file already exists: %1\nUse --overwrite to replace it.")
                             .arg(path);
        return false;
    }
    if (file_info.exists() && !file_info.isFile())
    {
        *error_message = QStringLiteral("Configuration output path is not a file: %1").arg(path);
        return false;
    }

    QDir parent_directory = file_info.dir();
    if (!parent_directory.exists() && !parent_directory.mkpath(QStringLiteral(".")))
    {
        *error_message = QStringLiteral("Failed to create configuration directory: %1")
                             .arg(parent_directory.absolutePath());
        return false;
    }

    const QByteArray default_config = defaultConfigText();

    QSaveFile file(file_info.absoluteFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        *error_message = QStringLiteral("Failed to open configuration file for writing: %1")
                             .arg(file.errorString());
        return false;
    }

    if (file.write(default_config) != default_config.size())
    {
        *error_message = QStringLiteral("Failed to write complete configuration file: %1")
                             .arg(file.errorString());
        file.cancelWriting();
        return false;
    }

    if (!file.commit())
    {
        *error_message = QStringLiteral("Failed to commit configuration file: %1").arg(file.errorString());
        return false;
    }

    const QFileDevice::Permissions permissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    if (!QFile::setPermissions(file_info.absoluteFilePath(), permissions))
    {
        *error_message = QStringLiteral("Configuration was written, but its permissions could not be restricted: %1")
                             .arg(file_info.absoluteFilePath());
        return false;
    }

    return true;
}


bool MapServerConfiguration::loadProviderStateFile(const QString &path,
    Server::Config *config, QString *error_message)
{
    if (!QFileInfo::exists(path))
        return true;

    const QFileInfo file_info(path);
    if (!file_info.isFile() || !file_info.isReadable())
    {
        *error_message = QStringLiteral("Provider state is not a readable file: %1").arg(path);
        return false;
    }
    QTemporaryFile compatible_file(QDir(QDir::tempPath()).filePath(
        QStringLiteral("aowis-server-map-ini-XXXXXX.ini")));
    QString effective_path;
    if (!compatibleIniPath(path, &compatible_file, &effective_path, error_message))
        return false;
    QSettings settings(effective_path, QSettings::IniFormat);
    const QStringList groups = settings.childGroups();
    for (const QString &group : groups)
    {
        if (group != QStringLiteral("map_providers") &&
            !group.startsWith(QStringLiteral("map_provider.")))
        {
            *error_message = QStringLiteral("Unexpected group in provider state: %1").arg(group);
            return false;
        }
    }
    if (settings.status() != QSettings::NoError)
    {
        *error_message = QStringLiteral("Invalid provider state file: %1").arg(path);
        return false;
    }
    if (!settings.value(QStringLiteral("map_providers/configured"), false).toBool())
    {
        *error_message = QStringLiteral("Provider state has no configured=true marker: %1").arg(path);
        return false;
    }
    if (!readMapProviderCatalog(&settings, config, error_message))
        return false;
    if (settings.status() != QSettings::NoError)
    {
        *error_message = QStringLiteral("Invalid provider state file: %1").arg(path);
        return false;
    }
    return true;
}

bool MapServerConfiguration::saveMapProvidersToState(const QString &path,
    const QVector<Aowis::Map::MapProvider> &providers, QString *error_message)
{
    if (path.trimmed().isEmpty())
    {
        *error_message = QStringLiteral("No writable provider state path configured");
        return false;
    }
#if defined(Q_OS_UNIX)
    // Never let an API request write an administrator-owned file under /etc,
    // including through an existing symlink to an /etc directory.
    const QFileInfo state_info(path);
    const QString absolute = QDir::cleanPath(state_info.absoluteFilePath());
    const QString canonical_parent = state_info.dir().canonicalPath();
    if (absolute == QStringLiteral("/etc") || absolute.startsWith(QStringLiteral("/etc/")) ||
        canonical_parent == QStringLiteral("/etc") ||
        canonical_parent.startsWith(QStringLiteral("/etc/")))
    {
        *error_message = QStringLiteral("Provider state must not be written under /etc: %1").arg(path);
        return false;
    }
#endif
    // This is *only* application-managed state. Never read, rewrite, or merge
    // the administrator-owned /etc configuration from an API request.
    QString text;
    text.append(QStringLiteral("; Map providers managed by the map server API.\n"));
    for (const Aowis::Map::MapProvider &provider : providers)
    {
        text.append(QStringLiteral("[map_provider.%1]\n").arg(provider.id));
        // INI values cannot contain newlines, as validated by the API.
        text.append(QStringLiteral("name=%1\nurl=%2\nmin_zoom=%3\nmax_zoom=%4\n")
            .arg(provider.name, provider.url_template)
            .arg(provider.minimum_zoom).arg(provider.maximum_zoom));
        text.append(QStringLiteral("enabled=%1\nremote_fetch_enabled=%2\n")
            .arg(provider.enabled ? QStringLiteral("true") : QStringLiteral("false"),
                 provider.remote_fetch_enabled ? QStringLiteral("true") : QStringLiteral("false")));
        if (!provider.coverage.isEmpty())
            text.append(QStringLiteral("coverage=%1\n").arg(provider.coverage));
        text.append(QLatin1Char('\n'));
    }
    // Even an empty catalog is authoritative; never restore built-ins.
    text.append(QStringLiteral("[map_providers]\nconfigured=true\n"));
    QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath()))
    {
        *error_message = QStringLiteral("Could not create provider state directory: %1")
                             .arg(info.absolutePath());
        return false;
    }
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        *error_message = output.errorString();
        return false;
    }
    // Provider URLs may contain credentials. Do not rely on the shell umask.
    if (!output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner))
    {
        *error_message = QStringLiteral("Could not restrict provider state permissions: %1")
                             .arg(output.errorString());
        output.cancelWriting();
        return false;
    }
    const QByteArray content = text.toUtf8();
    if (output.write(content) != content.size())
    {
        *error_message = output.errorString();
        output.cancelWriting();
        return false;
    }
    if (!output.commit())
    {
        *error_message = output.errorString();
        return false;
    }
    return true;
}
