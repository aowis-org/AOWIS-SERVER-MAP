#include <QCoreApplication>

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDebug>
#include <QElapsedTimer>
#include <QTimer>
#include <QFileInfo>
#include <QHostAddress>
#include <QTextStream>

#include <cstdlib>

#ifdef Q_OS_UNIX
#include <QSocketNotifier>
#include <csignal>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "configuration.h"
#include "server.h"

namespace
{
#ifdef Q_OS_UNIX
// SIGTERM (systemctl stop) and SIGINT end the event loop, so the server is
// destroyed normally: workers are canceled and joined, cache access hints
// are flushed. The handler only writes to a socket (async-signal-safe).
int shutdown_signal_sockets[2] = {-1, -1};

void handleShutdownSignal(int)
{
    const char byte = 1;
    const ssize_t written = ::write(shutdown_signal_sockets[0], &byte, sizeof(byte));
    (void)written;
}

bool installShutdownSignalHandlers(QCoreApplication *application)
{
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, shutdown_signal_sockets) != 0)
        return false;
    QSocketNotifier *notifier = new QSocketNotifier(
        shutdown_signal_sockets[1], QSocketNotifier::Read, application);
    QObject::connect(notifier, &QSocketNotifier::activated, application,
                     [notifier, application]()
    {
        notifier->setEnabled(false);
        char byte = 0;
        const ssize_t received = ::read(shutdown_signal_sockets[1], &byte, sizeof(byte));
        (void)received;
        qInfo() << "Shutdown requested; stopping map server";
        application->quit();
    });
    struct sigaction action = {};
    action.sa_handler = handleShutdownSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    return ::sigaction(SIGTERM, &action, nullptr) == 0 &&
           ::sigaction(SIGINT, &action, nullptr) == 0;
}
#endif

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
    const QCommandLineOption print_default_config_option(
        QStringLiteral("print-default-config"),
        QStringLiteral("Print the complete default INI to standard output without writing files."));
    const QCommandLineOption check_config_option(
        QStringLiteral("check-config"),
        QStringLiteral("Validate the effective configuration (the default file if --config is absent) and exit."));
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

    parser.addOption(config_option);
    parser.addOption(write_default_config_option);
    parser.addOption(overwrite_option);
    parser.addOption(print_default_config_option);
    parser.addOption(check_config_option);
    parser.addOption(listen_address_option);
    parser.addOption(port_option);
    parser.addOption(maximum_pending_requests_option);
    parser.addOption(maximum_active_downloads_option);
    parser.addOption(cache_directory_option);
    parser.process(app);

    if (int(parser.isSet(write_default_config_option)) + int(parser.isSet(print_default_config_option)) +
        int(parser.isSet(check_config_option)) > 1)
    {
        qCritical() << "Configuration generation, printing and checking are mutually exclusive";
        return EXIT_FAILURE;
    }
    if (parser.isSet(print_default_config_option))
    {
        QTextStream(stdout) << MapServerConfiguration::defaultConfigText();
        return EXIT_SUCCESS;
    }
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
    config.provider_state_file = MapServerConfiguration::defaultProviderStateFilePath();
    QString config_path;
    if (parser.isSet(config_option))
    {
        config_path = parser.value(config_option);
        if (config_path.trimmed().isEmpty())
        {
            qCritical() << "The --config path must not be empty";
            return EXIT_FAILURE;
        }

        // A manual --check-config --config /etc/... runs outside systemd,
        // where STATE_DIRECTORY is unavailable. Inspect the same persistent
        // provider state the packaged service normally uses.
        if (qEnvironmentVariableIsEmpty("STATE_DIRECTORY") &&
            QFileInfo(config_path).absoluteFilePath() ==
                QStringLiteral("/etc/aowis-server-map/map-server.ini"))
        {
            config.provider_state_file = QStringLiteral("/var/lib/aowis-server-map/providers.ini");
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

    // MAP itself owns writable provider/cascade records. Existing providers.ini
    // is imported once, never rewritten; --check-config only reads/validates.
    {
        QString error_message;
        if (!MapServerConfiguration::loadManagedSettings(
                &config, !parser.isSet(check_config_option), &error_message))
        {
            qCritical().noquote() << error_message;
            return EXIT_FAILURE;
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

    if (parser.isSet(check_config_option))
    {
        QTextStream output(stdout);
        output << "Configuration valid: " << (QFileInfo::exists(config_path) ?
            QFileInfo(config_path).absoluteFilePath() : QStringLiteral("built-in defaults")) << '\n';
        output << "Listen: " << config.listen_address.toString() << ':' << config.port << '\n';
        output << "2D upstream downloads: " << (config.map_remote_fetch_enabled ? "enabled" : "disabled")
               << ", coverage: " << (config.map_coverage.isEmpty() ? "world" : config.map_coverage) << '\n';
        output << "Terrain: " << (config.terrain_enabled ? "enabled" : "disabled")
               << ", upstream downloads: " << (config.terrain_remote_fetch_enabled ? "enabled" : "disabled")
               << ", coverage: " << (config.terrain_coverage.isEmpty() ? "world" : config.terrain_coverage) << '\n';
        output << "Map cache: " << config.cache_directory << '\n';
        output << "Terrain cache: " << (config.terrain_cache_directory.isEmpty() ?
            QStringLiteral("<map cache>/terrain") : config.terrain_cache_directory) << '\n';
        output << "Permanent data: " << (config.permanent_data_directory.isEmpty() ?
            QStringLiteral("<terrain cache>/offline-packages") : config.permanent_data_directory) << '\n';
        output << "Managed MAP settings: " << config.managed_settings_root << '\n';
        output << "Legacy provider state: " << config.provider_state_file
               << (QFileInfo::exists(config.provider_state_file) ? " (available for migration)\n" : " (not present)\n");
        output << "Cascade source: " << (config.cascade_defined_in_admin_ini ?
            "administrator INI" : "AOWIS-CONFIG or defaults") << '\n';
        output << "Map providers:\n";
        for (const Aowis::Map::MapProvider &provider : config.map_providers)
            output << "  " << provider.id << " (" << (provider.enabled ? "enabled" : "disabled")
                   << ", upstream: " << (provider.remote_fetch_enabled ? "enabled" : "disabled")
                   << ", coverage: " << (provider.coverage.isEmpty() ? "inherit" : provider.coverage)
                   << ")\n";
        output << "Copernicus terrain provider: " << (config.copernicus_enabled ? "enabled" : "disabled") << '\n';
        output << "Coverage profiles: " << config.download_coverage.profileNames().join(", ") << '\n';
        return EXIT_SUCCESS;
    }

    Server server(config);
    if (!server.start())
        return EXIT_FAILURE;
#ifdef Q_OS_UNIX
    if (!installShutdownSignalHandlers(&app))
        qWarning() << "Could not install SIGTERM/SIGINT handlers; shutdown will not flush caches";
#endif

    // Optional event-loop stall instrumentation for performance diagnostics.
    // Configure AOWIS_MAP_EVENT_LOOP_WARN_MS to a positive integer to enable.
    // Warnings are throttled to avoid a logging storm during sustained overload.
    bool valid_warning_threshold = false;
    const int event_loop_warning_ms =
        qEnvironmentVariableIntValue("AOWIS_MAP_EVENT_LOOP_WARN_MS", &valid_warning_threshold);
    QTimer event_loop_monitor;
    QElapsedTimer event_loop_clock;
    QElapsedTimer event_loop_warning_clock;
    if (valid_warning_threshold && event_loop_warning_ms > 0)
    {
        constexpr qint64 monitor_interval_ms = 100;
        event_loop_clock.start();
        event_loop_warning_clock.start();
        event_loop_monitor.setTimerType(Qt::PreciseTimer);
        event_loop_monitor.setInterval(int(monitor_interval_ms));
        QObject::connect(&event_loop_monitor, &QTimer::timeout, &app,
                         [&event_loop_clock, &event_loop_warning_clock, event_loop_warning_ms]()
        {
            constexpr qint64 expected_interval_ms = 100;
            constexpr qint64 warning_interval_ms = 30000;
            const qint64 elapsed_ms = event_loop_clock.restart();
            const qint64 lag_ms = qMax(qint64(0), elapsed_ms - expected_interval_ms);
            if (lag_ms >= event_loop_warning_ms &&
                event_loop_warning_clock.elapsed() >= warning_interval_ms)
            {
                qWarning().noquote() << "HTTP event-loop scheduling delay:" << lag_ms << "ms";
                event_loop_warning_clock.restart();
            }
        });
        event_loop_monitor.start();
        qInfo() << "Event-loop stall warnings enabled, threshold:" << event_loop_warning_ms << "ms";
    }

    return app.exec();
}
