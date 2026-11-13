#ifndef AOWIS_MAP_CASCADE_CONFIG_H
#define AOWIS_MAP_CASCADE_CONFIG_H

#include <QByteArray>
#include <QNetworkProxy>
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
    // Stable ID of the hosting application's central WireGuard profile this
    // upstream must be reached through; empty: direct HTTP(S). MAP stores
    // only this reference (never keys or WireGuard configuration). An
    // upstream with a profile is never contacted directly: MAP skips it
    // unless its host provides that tunnel's transport in proxy.
    QString wireguard_profile_id;
    // Runtime only, never persisted: the host's local SOCKS5 proxy of the
    // running tunnel for wireguard_profile_id (host names are resolved
    // inside the tunnel). Ignored for direct upstreams.
    QNetworkProxy proxy;
};

inline bool operator==(const MapCascadeUpstream &left, const MapCascadeUpstream &right)
{
    return left.name == right.name && left.url == right.url && left.api_key == right.api_key &&
           left.wireguard_profile_id == right.wireguard_profile_id &&
           left.proxy == right.proxy;
}

// True when the upstream must be reached through a WireGuard tunnel.
inline bool requiresWireGuardTransport(const MapCascadeUpstream &upstream)
{
    return !upstream.wireguard_profile_id.isEmpty();
}

// True when the upstream's tunnel transport is available (SOCKS5 proxy).
inline bool hasWireGuardTransport(const MapCascadeUpstream &upstream)
{
    return requiresWireGuardTransport(upstream) &&
           upstream.proxy.type() == QNetworkProxy::Socks5Proxy;
}

// True when the upstream may be contacted now: directly, or through its
// running tunnel. A WireGuard upstream without one is never contacted.
inline bool cascadeUpstreamUsable(const MapCascadeUpstream &upstream)
{
    return !requiresWireGuardTransport(upstream) || hasWireGuardTransport(upstream);
}

// Identifies one tunnel proxy (for sharing one network manager per tunnel).
inline QString cascadeProxyKey(const QNetworkProxy &proxy)
{
    return QStringLiteral("%1:%2:%3").arg(proxy.hostName()).arg(proxy.port()).arg(proxy.user());
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
