#include <aowis/map/map_provider.h>

#include <QSet>

namespace Aowis::Map
{
QVector<MapProvider> defaultMapProviders()
{
    return {
        {QStringLiteral("c84ccc10-ff57-54d7-babf-e2e68a4dc7d2"), QStringLiteral("Google Satellite"),
         QStringLiteral("https://mt0.google.com/vt/lyrs=s&x={x}&y={y}&z={z}"), {}, true, true, 0, 21},
        {QStringLiteral("0381bc44-67e9-5af9-aeb0-023ccba36658"), QStringLiteral("OpenStreetMap"),
         QStringLiteral("https://tile.openstreetmap.org/{z}/{x}/{y}.png"), {}, true, true, 0, 19},
        {QStringLiteral("178a60e6-178a-507b-af86-f45427150986"), QStringLiteral("OpenTopoMap"),
         QStringLiteral("https://tile.opentopomap.org/{z}/{x}/{y}.png"), {}, true, true, 0, 17},
        {QStringLiteral("49bda1b9-15aa-55e6-8933-2172c785e775"), QStringLiteral("CyclOSM"),
         QStringLiteral("https://{s}.tile-cyclosm.openstreetmap.fr/cyclosm/{z}/{x}/{y}.png"), {}, true, true, 0, 20},
        {QStringLiteral("a0d790c2-f231-55fd-b95e-23c9bd6cb3fc"), QStringLiteral("ArcGIS World Imagery"),
         QStringLiteral("https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}"), {}, true, true, 0, 17}
    };
}

bool addGoogleSatelliteToLegacyPresets(QVector<MapProvider> *providers)
{
    if (providers == nullptr)
        return false;
    const QVector<MapProvider> defaults = defaultMapProviders();
    QSet<QString> ids;
    for (const MapProvider &provider : *providers)
        ids.insert(provider.id);
    // Only upgrade complete pre-Google catalogs. Respect explicitly trimmed
    // catalogs; preserve all existing provider settings and user-added entries.
    if (ids.contains(defaults.first().id))
        return false;
    for (qsizetype index = 1; index < defaults.size(); ++index)
        if (!ids.contains(defaults.at(index).id))
            return false;
    providers->prepend(defaults.first());
    return true;
}
}
