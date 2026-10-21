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
QString defaultConfigFilePath();
bool loadConfigFile(const QString &path, Server::Config *config, QString *error_message);
QByteArray defaultConfigText();
bool loadProviderStateFile(const QString &path, Server::Config *config, QString *error_message);
bool saveMapProvidersToState(const QString &path,
    const QVector<Aowis::Map::MapProvider> &providers, QString *error_message);
bool writeDefaultConfigFile(const QString &path, bool overwrite, QString *error_message);
}

#endif // CONFIGURATION_H
