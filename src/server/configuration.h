#ifndef CONFIGURATION_H
#define CONFIGURATION_H

#include <QString>
#include <QByteArray>

#include "server.h"

namespace MapServerConfiguration
{
QString defaultUserDataDirectory();
QString defaultCacheDirectory();
QString defaultProviderStateFilePath();
QString defaultManagedSettingsRoot(const Server::Config &config);
// Load MAP-managed CONFIG records after the administrator INI and before CLI
// overrides. Only an absent provider record is eligible for one-time legacy
// providers.ini migration. When migrate_legacy=false, no migration is written.
bool loadManagedSettings(Server::Config *config, bool migrate_legacy,
                         QString *error_message);
QString defaultConfigFilePath();
bool loadConfigFile(const QString &path, Server::Config *config, QString *error_message);
QByteArray defaultConfigText();
bool loadProviderStateFile(const QString &path, Server::Config *config, QString *error_message);
bool saveMapProvidersToState(const QString &path,
    const QVector<Aowis::Map::MapProvider> &providers, QString *error_message);
bool writeDefaultConfigFile(const QString &path, bool overwrite, QString *error_message);
}

#endif // CONFIGURATION_H
