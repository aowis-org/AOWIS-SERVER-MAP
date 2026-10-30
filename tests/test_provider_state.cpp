#include "../src/server/configuration.h"

#include <aowis/map/map_provider.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextStream>

#include <cstdio>

namespace
{
bool expect(bool condition, const QString &message)
{
    if (!condition)
        std::fprintf(stderr, "%s\n", qPrintable(message));
    return condition;
}

bool writeFile(const QString &path, const QByteArray &content)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), QStringLiteral("Could not create temporary directory")))
        return 1;

    const QString admin_path = QDir(temporary.path()).filePath(QStringLiteral("admin.ini"));
    const QString state_path = QDir(temporary.path()).filePath(QStringLiteral("state/providers.ini"));
    const QByteArray admin_content = QByteArrayLiteral(
        "# Must stay exactly as the administrator wrote it.\n"
        "[downloads]\nremote_fetch_enabled=false\n"
        "[storage]\nprovider_state_file=state/providers.ini\n"
        "[region.local]\nbbox=10,10,20,20\n"
        "[coverage.projects]\nzoom_0=local\n"
        "[map_provider.0381bc44-67e9-5af9-aeb0-023ccba36658]\n"
        "name=Administrator Provider\n"
        "url=https://example.org/{z}/{x}/{y}.png\n"
        "min_zoom=0\nmax_zoom=12\n"
        "coverage=projects\n"
        "[authentication]\napi_key=unchanged-secret\n");
    if (!expect(writeFile(admin_path, admin_content), QStringLiteral("Could not write admin INI")))
        return 1;

    // The generated configuration must also be valid for Qt's unmodified
    // INI reader, without any compatibility filtering.
    const QString defaults_path = QDir(temporary.path()).filePath(QStringLiteral("defaults.ini"));
    if (!expect(writeFile(defaults_path, MapServerConfiguration::defaultConfigText()),
                QStringLiteral("Could not write default INI")))
        return 1;
    QSettings generated_settings(defaults_path, QSettings::IniFormat);
    generated_settings.allKeys();
    if (!expect(generated_settings.status() == QSettings::NoError,
                QStringLiteral("Generated default INI is invalid for QSettings")))
        return 1;
    Server::Config generated_config;
    QString generated_error;
    if (!expect(MapServerConfiguration::loadConfigFile(defaults_path, &generated_config,
                                                       &generated_error), generated_error))
        return 1;

    Server::Config config;
    QString error;
    if (!expect(MapServerConfiguration::loadConfigFile(admin_path, &config, &error), error))
        return 1;
    if (!expect(config.provider_state_file == state_path,
                QStringLiteral("Relative provider state path was not resolved against the admin INI")))
        return 1;
    if (!expect(!config.map_remote_fetch_enabled && config.map_providers.size() == 1 &&
                config.map_providers.first().name == QStringLiteral("Administrator Provider"),
                QStringLiteral("Administrator provider catalog was not loaded")))
        return 1;

    if (!expect(MapServerConfiguration::loadProviderStateFile(state_path, &config, &error), error) ||
        !expect(!QFileInfo::exists(state_path), QStringLiteral("Startup created provider state")))
        return 1;

    QVector<Aowis::Map::MapProvider> modified = config.map_providers;
    modified[0].name = QStringLiteral("GUI Provider");
    if (!expect(MapServerConfiguration::saveMapProvidersToState(state_path, modified, &error), error) ||
        !expect(readFile(admin_path) == admin_content,
                QStringLiteral("Provider save changed the administrator INI")))
        return 1;
    QSettings saved_settings(state_path, QSettings::IniFormat);
    saved_settings.allKeys();
    if (!expect(saved_settings.status() == QSettings::NoError,
                QStringLiteral("Saved provider INI is invalid for QSettings")))
        return 1;

    Server::Config restarted;
    if (!expect(MapServerConfiguration::loadConfigFile(admin_path, &restarted, &error), error) ||
        !expect(MapServerConfiguration::loadProviderStateFile(state_path, &restarted, &error), error))
        return 1;
    if (!expect(restarted.map_providers.size() == 1 &&
                restarted.map_providers.first().name == QStringLiteral("GUI Provider") &&
                !restarted.map_remote_fetch_enabled && restarted.map_coverage.isEmpty(),
                QStringLiteral("State failed to override only the provider catalog")))
        return 1;

    // Provider strings with INI metacharacters and percent-encoding must
    // survive a save and restart unchanged.
    QVector<Aowis::Map::MapProvider> tricky = modified;
    tricky[0].name = QStringLiteral("Esri, Maxar; \"quoted\" \\ = #1 %1 %3");
    tricky[0].url_template =
        QStringLiteral("https://example.org/{z}/{x}/{y}.png?layers=a,b;c=d%2C%3&x=\\");
    if (!expect(MapServerConfiguration::saveMapProvidersToState(state_path, tricky, &error), error))
        return 1;
    Server::Config tricky_restarted;
    if (!expect(MapServerConfiguration::loadConfigFile(admin_path, &tricky_restarted, &error), error) ||
        !expect(MapServerConfiguration::loadProviderStateFile(state_path, &tricky_restarted, &error),
                error) ||
        !expect(tricky_restarted.map_providers.size() == 1 &&
                tricky_restarted.map_providers.first().name == tricky[0].name &&
                tricky_restarted.map_providers.first().url_template == tricky[0].url_template &&
                tricky_restarted.map_providers.first().coverage == tricky[0].coverage,
                QStringLiteral("Provider strings with INI metacharacters did not round-trip")))
        return 1;
    tricky[0].name = QStringLiteral("@Variant(not a variant)");
    if (!expect(MapServerConfiguration::saveMapProvidersToState(state_path, tricky, &error), error))
        return 1;
    Server::Config at_restarted;
    if (!expect(MapServerConfiguration::loadConfigFile(admin_path, &at_restarted, &error), error) ||
        !expect(MapServerConfiguration::loadProviderStateFile(state_path, &at_restarted, &error),
                error) ||
        !expect(at_restarted.map_providers.size() == 1 &&
                at_restarted.map_providers.first().name == tricky[0].name,
                QStringLiteral("Provider name with a leading '@' did not round-trip")))
        return 1;
    if (!expect(MapServerConfiguration::saveMapProvidersToState(state_path, modified, &error), error))
        return 1;

    // An unquoted comma turns a single-string INI value into a list. That
    // must be rejected instead of silently emptying an API key.
    const QString comma_key_path = QDir(temporary.path()).filePath(QStringLiteral("comma-key.ini"));
    if (!expect(writeFile(comma_key_path, QByteArrayLiteral("[authentication]\napi_key=abc,def\n")),
                QStringLiteral("Could not write comma API-key INI")))
        return 1;
    Server::Config comma_key;
    QString comma_error;
    if (!expect(!MapServerConfiguration::loadConfigFile(comma_key_path, &comma_key, &comma_error),
                QStringLiteral("Unquoted comma in api_key was silently accepted")))
        return 1;
    if (!expect(writeFile(comma_key_path,
                          QByteArrayLiteral("[authentication]\napi_key=\"abc,def\"\n")),
                QStringLiteral("Could not write quoted API-key INI")))
        return 1;
    Server::Config quoted_key;
    if (!expect(MapServerConfiguration::loadConfigFile(comma_key_path, &quoted_key, &error), error) ||
        !expect(quoted_key.api_key == QByteArrayLiteral("abc,def"),
                QStringLiteral("Quoted api_key with a comma was not read verbatim")))
        return 1;

    // Older versions emitted '#' instead of ';' in provider state too.
    const QByteArray old_state = QByteArrayLiteral("# Old provider state comment.\n") +
                                 readFile(state_path);
    if (!expect(writeFile(state_path, old_state), QStringLiteral("Could not write legacy state")))
        return 1;
    Server::Config legacy_state;
    if (!expect(MapServerConfiguration::loadConfigFile(admin_path, &legacy_state, &error) &&
                MapServerConfiguration::loadProviderStateFile(state_path, &legacy_state, &error), error) ||
        !expect(legacy_state.map_providers.size() == 1 &&
                legacy_state.map_providers.first().name == QStringLiteral("GUI Provider") &&
                readFile(state_path) == old_state,
                QStringLiteral("Legacy state comments were not handled without rewriting")))
        return 1;

    if (!expect(MapServerConfiguration::saveMapProvidersToState(state_path, {}, &error), error))
        return 1;
    Server::Config empty_catalog;
    if (!expect(MapServerConfiguration::loadConfigFile(admin_path, &empty_catalog, &error), error) ||
        !expect(MapServerConfiguration::loadProviderStateFile(state_path, &empty_catalog, &error), error) ||
        !expect(empty_catalog.map_providers.isEmpty(),
                QStringLiteral("Explicitly empty provider catalog restored defaults")))
        return 1;

    if (!expect(writeFile(state_path, QByteArrayLiteral("[unexpected]\nkey=value\n")),
                QStringLiteral("Could not write invalid state")))
        return 1;
    Server::Config invalid;
    if (!expect(!MapServerConfiguration::loadProviderStateFile(state_path, &invalid, &error),
                QStringLiteral("Invalid state was silently accepted")))
        return 1;

    if (!expect(readFile(admin_path) == admin_content,
                QStringLiteral("The admin configuration was modified")))
        return 1;
    return 0;
}
