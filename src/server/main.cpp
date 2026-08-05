#include <QCoreApplication>

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDebug>
#include <QFileInfo>
#include <QHostAddress>

#include <cstdlib>

#include "configuration.h"
#include "server.h"

namespace
{
bool parsePositiveInt(const QString &value, const QString &option_name, int *result)
{
    bool valid = false;
    const int parsed_value = value.toInt(&valid);
    if (!valid || parsed_value <= 0)
    {
        qCritical().noquote() << QStringLiteral("Invalid value for --%1: %2").arg(option_name, value);
        return false;
    }

    *result = parsed_value;
    return true;
}
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("AOWIS"));
    QCoreApplication::setApplicationName(QStringLiteral("aowis-server-map"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("AOWIS caching map tile server"));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption config_option(
        { QStringLiteral("c"), QStringLiteral("config") },
        QStringLiteral("Load configuration from this INI file."),
        QStringLiteral("path"));
    const QCommandLineOption write_default_config_option(
        QStringLiteral("write-default-config"),
        QStringLiteral("Write a configuration containing the built-in defaults and exit."),
        QStringLiteral("path"));
    const QCommandLineOption overwrite_option(
        QStringLiteral("overwrite"),
        QStringLiteral("Allow --write-default-config to replace an existing file."));
    const QCommandLineOption listen_address_option(
        { QStringLiteral("a"), QStringLiteral("listen-address") },
        QStringLiteral("IP address on which the HTTP server listens. Default: 127.0.0.1."),
        QStringLiteral("address"));
    const QCommandLineOption port_option(
        { QStringLiteral("p"), QStringLiteral("port") },
        QStringLiteral("TCP port on which the HTTP server listens. Default: 8122."),
        QStringLiteral("port"));
    const QCommandLineOption maximum_pending_requests_option(
        QStringLiteral("max-pending-requests"),
        QStringLiteral("Maximum number of tile HTTP requests waiting for a result. Default: 2048."),
        QStringLiteral("count"));
    const QCommandLineOption maximum_active_downloads_option(
        QStringLiteral("max-active-downloads"),
        QStringLiteral("Maximum number of simultaneous upstream tile downloads. Default: 32."),
        QStringLiteral("count"));
    const QCommandLineOption cache_directory_option(
        QStringLiteral("cache-directory"),
        QStringLiteral("Directory used for the persistent map tile cache."),
        QStringLiteral("path"));
    const QCommandLineOption api_key_option(
        QStringLiteral("api-key"),
        QStringLiteral("API key required for status and tile GET requests."),
        QStringLiteral("key"));
    const QCommandLineOption delete_api_key_option(
        QStringLiteral("delete-api-key"),
        QStringLiteral("Separate API key required for cache DELETE requests."),
        QStringLiteral("key"));
    const QCommandLineOption require_api_key_option(
        QStringLiteral("require-api-key"),
        QStringLiteral("Refuse to start when no read API key is configured."));
    const QCommandLineOption require_delete_api_key_option(
        QStringLiteral("require-delete-api-key"),
        QStringLiteral("Refuse to start when no delete API key is configured."));

    parser.addOption(config_option);
    parser.addOption(write_default_config_option);
    parser.addOption(overwrite_option);
    parser.addOption(listen_address_option);
    parser.addOption(port_option);
    parser.addOption(maximum_pending_requests_option);
    parser.addOption(maximum_active_downloads_option);
    parser.addOption(cache_directory_option);
    parser.addOption(api_key_option);
    parser.addOption(delete_api_key_option);
    parser.addOption(require_api_key_option);
    parser.addOption(require_delete_api_key_option);
    parser.process(app);

    if (parser.isSet(overwrite_option) && !parser.isSet(write_default_config_option))
    {
        qCritical() << "--overwrite is only valid together with --write-default-config";
        return EXIT_FAILURE;
    }

    if (parser.isSet(write_default_config_option))
    {
        const QString output_path = parser.value(write_default_config_option);
        QString error_message;
        if (!MapServerConfiguration::writeDefaultConfigFile(output_path, parser.isSet(overwrite_option),
                                                               &error_message))
        {
            qCritical().noquote() << error_message;
            return EXIT_FAILURE;
        }

        qInfo().noquote() << QStringLiteral("Created default configuration: %1")
                                 .arg(QFileInfo(output_path).absoluteFilePath());
        return EXIT_SUCCESS;
    }

    Server::Config config;
    config.cache_directory = MapServerConfiguration::defaultCacheDirectory();
    QString config_path;
    if (parser.isSet(config_option))
    {
        config_path = parser.value(config_option);
        if (config_path.trimmed().isEmpty())
        {
            qCritical() << "The --config path must not be empty";
            return EXIT_FAILURE;
        }

        QString error_message;
        if (!MapServerConfiguration::loadConfigFile(config_path, &config, &error_message))
        {
            qCritical().noquote() << error_message;
            return EXIT_FAILURE;
        }
        qInfo().noquote() << QStringLiteral("Loaded configuration: %1")
                                 .arg(QFileInfo(config_path).absoluteFilePath());
    }
    else
    {
        config_path = MapServerConfiguration::defaultConfigFilePath();
        if (QFileInfo::exists(config_path))
        {
            QString error_message;
            if (!MapServerConfiguration::loadConfigFile(config_path, &config, &error_message))
            {
                qCritical().noquote() << error_message;
                return EXIT_FAILURE;
            }
            qInfo().noquote() << QStringLiteral("Loaded configuration: %1").arg(config_path);
        }
        else
        {
            qInfo().noquote() << QStringLiteral("Configuration file not found: %1\nUsing built-in defaults.")
                                     .arg(config_path);
        }
    }

    if (parser.isSet(listen_address_option))
    {
        const QHostAddress listen_address(parser.value(listen_address_option));
        if (listen_address.isNull())
        {
            qCritical() << "Invalid listen address:" << parser.value(listen_address_option);
            return EXIT_FAILURE;
        }
        config.listen_address = listen_address;
    }

    if (parser.isSet(port_option))
    {
        int port = 0;
        if (!parsePositiveInt(parser.value(port_option), QStringLiteral("port"), &port) || port > 65535)
        {
            if (port > 65535)
                qCritical() << "Port must not exceed 65535";
            return EXIT_FAILURE;
        }
        config.port = quint16(port);
    }

    if (parser.isSet(maximum_pending_requests_option))
    {
        if (!parsePositiveInt(parser.value(maximum_pending_requests_option),
                              QStringLiteral("max-pending-requests"), &config.maximum_pending_requests))
        {
            return EXIT_FAILURE;
        }
    }

    if (parser.isSet(maximum_active_downloads_option))
    {
        if (!parsePositiveInt(parser.value(maximum_active_downloads_option),
                              QStringLiteral("max-active-downloads"), &config.maximum_active_downloads))
        {
            return EXIT_FAILURE;
        }
    }

    if (parser.isSet(cache_directory_option))
    {
        const QString cache_directory = parser.value(cache_directory_option).trimmed();
        if (cache_directory.isEmpty())
        {
            qCritical() << "The --cache-directory path must not be empty";
            return EXIT_FAILURE;
        }
        config.cache_directory = QFileInfo(cache_directory).absoluteFilePath();
    }

    if (parser.isSet(api_key_option))
    {
        if (parser.value(api_key_option).isEmpty())
        {
            qCritical() << "The API key must not be empty when --api-key is specified";
            return EXIT_FAILURE;
        }
        config.api_key = parser.value(api_key_option).toUtf8();
    }

    if (parser.isSet(delete_api_key_option))
    {
        if (parser.value(delete_api_key_option).isEmpty())
        {
            qCritical() << "The delete API key must not be empty when --delete-api-key is specified";
            return EXIT_FAILURE;
        }
        config.delete_api_key = parser.value(delete_api_key_option).toUtf8();
    }

    config.require_api_key = parser.isSet(require_api_key_option);
    config.require_delete_api_key = parser.isSet(require_delete_api_key_option);

    Server server(config);
    if (!server.start())
        return EXIT_FAILURE;

    return app.exec();
}
