#ifndef AOWIS_MAP_CASCADE_CONFIG_H
#define AOWIS_MAP_CASCADE_CONFIG_H

#include <QByteArray>
#include <QString>
#include <QUrl>
#include <QVector>

namespace Aowis::Map
{
// Per-raster-provider policy. Inherited providers can never fetch their advertised
// origin directly, regardless of this setting or global administrator policy.
enum class MapProviderAcquisitionMode
{
    Automatic,
    Cascade,
    Direct
};

inline QString providerAcquisitionModeName(MapProviderAcquisitionMode mode)
{
    switch (mode)
    {
    case MapProviderAcquisitionMode::Automatic: return QStringLiteral("automatic");
    case MapProviderAcquisitionMode::Cascade: return QStringLiteral("cascade");
    case MapProviderAcquisitionMode::Direct: return QStringLiteral("direct");
    }
    return QStringLiteral("cascade");
}

inline bool parseProviderAcquisitionMode(const QString &name,
                                         MapProviderAcquisitionMode *mode)
{
    if (mode == nullptr) return false;
    if (name == QStringLiteral("automatic")) *mode = MapProviderAcquisitionMode::Automatic;
    else if (name == QStringLiteral("cascade")) *mode = MapProviderAcquisitionMode::Cascade;
    else if (name == QStringLiteral("direct")) *mode = MapProviderAcquisitionMode::Direct;
    else return false;
    return true;
}

// Ordered read-only connections to other AOWIS MAP servers. This configuration
// belongs to the reusable MAP library: HTTP and embedded Standalone consumers
// must apply exactly the same source-selection policy.
struct MapCascadeUpstream
{
    QString name;
    QUrl url;
    QByteArray api_key;
};

inline bool operator==(const MapCascadeUpstream &left, const MapCascadeUpstream &right)
{
    return left.name == right.name && left.url == right.url && left.api_key == right.api_key;
}

inline bool operator!=(const MapCascadeUpstream &left, const MapCascadeUpstream &right)
{
    return !(left == right);
}

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

inline bool operator==(const MapCascadeConfig &left, const MapCascadeConfig &right)
{
    return left.upstreams == right.upstreams &&
           left.allow_direct_fetch == right.allow_direct_fetch &&
           left.poor_internet_mode == right.poor_internet_mode &&
           left.jpeg_quality == right.jpeg_quality &&
           left.adaptive_compression == right.adaptive_compression;
}

inline bool operator!=(const MapCascadeConfig &left, const MapCascadeConfig &right)
{
    return !(left == right);
}
}

#endif // AOWIS_MAP_CASCADE_CONFIG_H
