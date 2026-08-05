#ifndef CONFIGURATION_H
#define CONFIGURATION_H

#include <QString>

#include "server.h"

namespace MapServerConfiguration
{
QString defaultUserDataDirectory();
QString defaultCacheDirectory();
QString defaultConfigFilePath();
bool loadConfigFile(const QString &path, Server::Config *config, QString *error_message);
bool writeDefaultConfigFile(const QString &path, bool overwrite, QString *error_message);
}

#endif // CONFIGURATION_H
