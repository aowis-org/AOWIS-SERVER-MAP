#include <aowis/map/map_provider.h>

namespace Aowis::Map
{
QVector<MapProvider> defaultMapProviders()
{
    return {
        {QStringLiteral("0381bc44-67e9-5af9-aeb0-023ccba36658"), QStringLiteral("OpenStreetMap"),
         QStringLiteral("https://tile.openstreetmap.org/{z}/{x}/{y}.png"), {}, true, true, 0, 19},
        {QStringLiteral("49bda1b9-15aa-55e6-8933-2172c785e775"), QStringLiteral("CyclOSM"),
         QStringLiteral("https://{s}.tile-cyclosm.openstreetmap.fr/cyclosm/{z}/{x}/{y}.png"), {}, true, true, 0, 20},
        {QStringLiteral("178a60e6-178a-507b-af86-f45427150986"), QStringLiteral("OpenTopoMap"),
         QStringLiteral("https://tile.opentopomap.org/{z}/{x}/{y}.png"), {}, true, true, 0, 17},
        {QStringLiteral("a0d790c2-f231-55fd-b95e-23c9bd6cb3fc"), QStringLiteral("ArcGIS World Imagery"),
         QStringLiteral("https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}"), {}, true, true, 0, 17}
    };
}
}
