#include <aowis/map/map_provider.h>

namespace Aowis::Map
{
QVector<MapProvider> defaultMapProviders()
{
    return {
        {QStringLiteral("openstreetmap"), QStringLiteral("OpenStreetMap"),
         QStringLiteral("https://tile.openstreetmap.org/{z}/{x}/{y}.png"), {}, true, true, 0, 19},
        {QStringLiteral("osmcyclo"), QStringLiteral("CyclOSM"),
         QStringLiteral("https://{s}.tile-cyclosm.openstreetmap.fr/cyclosm/{z}/{x}/{y}.png"), {}, true, true, 0, 20},
        {QStringLiteral("opentopomap"), QStringLiteral("OpenTopoMap"),
         QStringLiteral("https://tile.opentopomap.org/{z}/{x}/{y}.png"), {}, true, true, 0, 17},
        {QStringLiteral("arcgis"), QStringLiteral("ArcGIS World Imagery"),
         QStringLiteral("https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}"), {}, true, true, 0, 19}
    };
}
}
