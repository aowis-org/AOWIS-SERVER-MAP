#ifndef AOWIS_MAP_MAP_PROVIDER_H
#define AOWIS_MAP_MAP_PROVIDER_H

#include <QString>
#include <QVector>

namespace Aowis::Map
{
struct MapProvider
{
    QString id;
    QString name;
    QString url_template;
    QString coverage; // Empty inherits the [downloads] coverage setting.
    bool enabled = true;
    bool remote_fetch_enabled = true;
    int minimum_zoom = 0;
    int maximum_zoom = 19;
};

QVector<MapProvider> defaultMapProviders();
}
#endif
