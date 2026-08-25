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
#include <QStandardPaths>
#include <QtGlobal>

#include <limits>

namespace
{
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

    QSettings settings(path, QSettings::IniFormat);

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

    const Server::Config defaults;
    const QByteArray default_config = QStringLiteral(
        "# AOWIS map server configuration\n"
        "# Explicit command-line options override these values.\n"
        "\n"
        "[server]\n"
        "listen_address=%1\n"
        "port=%2\n"
        "max_pending_requests=%3\n"
        "\n"
        "[downloads]\n"
        "max_active_downloads=%4\n"
        "# Empty uses the automatically selected cache directory shown at startup.\n"
        "cache_directory=\n"
        "\n"
        "[terrain]\n"
        "# Disable terrain/elevation support without affecting raster map tiles.\n"
        "enabled=true\n"
        "# When false, terrain may use only local/cache/offline data.\n"
        "remote_fetch_enabled=true\n"
        "# Dataset used by point-elevation requests that do not name a dataset.\n"
        "default_dataset=%5\n"
        "# Empty uses <downloads cache>/terrain.\n"
        "cache_directory=\n"
        "\n"
        "[authentication]\n"
        "# Empty disables API-key authentication for status and tile GET requests.\n"
        "api_key=\n"
        "# Empty allows cache DELETE requests without API-key authentication.\n"
        "# The supplied systemd service requires this key and refuses startup when it is empty.\n"
        "delete_api_key=\n")
                                          .arg(defaults.listen_address.toString())
                                          .arg(defaults.port)
                                          .arg(defaults.maximum_pending_requests)
                                          .arg(defaults.maximum_active_downloads)
                                          .arg(defaults.terrain_default_dataset)
                                          .toUtf8();

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
