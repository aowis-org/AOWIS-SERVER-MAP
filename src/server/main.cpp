#include <QCoreApplication>

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDebug>
#include <QHostAddress>

#include <cstdlib>

#include "server.h"

namespace
{
bool parsePositiveInt(const QString &value, const QString &option_name, int *result)
{
    bool valid = false;
    const int parsed_value = value.toInt(&valid);
    if (!valid || parsed_value <= 0)
    {
        qCritical().noquote() << QStringLiteral("Invalid value for --%1: %2")
                                    .arg(option_name, value);
        return false;
    }

    *result = parsed_value;
    return true;
}
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("aowis-server-map"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("AOWIS caching map tile server"));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption listen_address_option(
        { QStringLiteral("a"), QStringLiteral("listen-address") },
        QStringLiteral("IP address on which the HTTP server listens."),
        QStringLiteral("address"),
        QStringLiteral("127.0.0.1"));
    const QCommandLineOption port_option(
        { QStringLiteral("p"), QStringLiteral("port") },
        QStringLiteral("TCP port on which the HTTP server listens."),
        QStringLiteral("port"),
        QStringLiteral("8122"));
    const QCommandLineOption maximum_pending_requests_option(
        QStringLiteral("max-pending-requests"),
        QStringLiteral("Maximum number of tile HTTP requests waiting for a result."),
        QStringLiteral("count"),
        QStringLiteral("2048"));
    const QCommandLineOption maximum_active_downloads_option(
        QStringLiteral("max-active-downloads"),
        QStringLiteral("Maximum number of simultaneous upstream tile downloads."),
        QStringLiteral("count"),
        QStringLiteral("32"));
    const QCommandLineOption api_key_option(
        QStringLiteral("api-key"),
        QStringLiteral("Optional API key required in X-API-Key or Authorization: Bearer headers."),
        QStringLiteral("key"));

    parser.addOption(listen_address_option);
    parser.addOption(port_option);
    parser.addOption(maximum_pending_requests_option);
    parser.addOption(maximum_active_downloads_option);
    parser.addOption(api_key_option);
    parser.process(app);

    if (parser.isSet(api_key_option) && parser.value(api_key_option).isEmpty())
    {
        qCritical() << "The API key must not be empty when --api-key is specified";
        return EXIT_FAILURE;
    }

    const QHostAddress listen_address(parser.value(listen_address_option));
    if (listen_address.isNull())
    {
        qCritical() << "Invalid listen address:" << parser.value(listen_address_option);
        return EXIT_FAILURE;
    }

    int port = 0;
    if (!parsePositiveInt(parser.value(port_option), QStringLiteral("port"), &port) || port > 65535)
    {
        if (port > 65535)
            qCritical() << "Port must not exceed 65535";
        return EXIT_FAILURE;
    }

    int maximum_pending_requests = 0;
    if (!parsePositiveInt(parser.value(maximum_pending_requests_option),
                          QStringLiteral("max-pending-requests"), &maximum_pending_requests))
    {
        return EXIT_FAILURE;
    }

    int maximum_active_downloads = 0;
    if (!parsePositiveInt(parser.value(maximum_active_downloads_option),
                          QStringLiteral("max-active-downloads"), &maximum_active_downloads))
    {
        return EXIT_FAILURE;
    }

    Server::Config config;
    config.listen_address = listen_address;
    config.port = quint16(port);
    config.maximum_pending_requests = maximum_pending_requests;
    config.maximum_active_downloads = maximum_active_downloads;
    config.api_key = parser.value(api_key_option).toUtf8();

    Server server(config);
    if (!server.start())
        return EXIT_FAILURE;

    return app.exec();
}
