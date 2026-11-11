#ifndef AOWIS_MAP_CASCADE_CONFIG_H
#define AOWIS_MAP_CASCADE_CONFIG_H

#include <QByteArray>
#include <QString>
#include <QUrl>
#include <QVector>

namespace Aowis::Map
{
// Ordered read-only connections to other AOWIS MAP servers. This configuration
// belongs to the reusable MAP library: HTTP and embedded Standalone consumers
// must apply exactly the same source-selection policy.
struct MapCascadeUpstream
{
    QString name;
    QUrl url;
    QByteArray api_key;
};

struct MapCascadeConfig
{
    QVector<MapCascadeUpstream> upstreams;
    // Defaults preserve the existing no-upstream acquisition behavior. With
    // this false, missing source assets must never trigger origin downloads.
    bool allow_direct_fetch = true;
    bool poor_internet_mode = false;
    int jpeg_quality = 75;
    bool adaptive_compression = true;
};
}

#endif // AOWIS_MAP_CASCADE_CONFIG_H
