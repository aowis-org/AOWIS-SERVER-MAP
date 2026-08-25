#include <aowis/map/terrain_elevation.h>

#include <cmath>

namespace Aowis::Map
{

QString terrainVerticalDatumId(TerrainVerticalDatum datum)
{
    switch (datum)
    {
        case TerrainVerticalDatum::Wgs84Ellipsoid:
            return QStringLiteral("wgs84-ellipsoid");
        case TerrainVerticalDatum::Egm96:
            return QStringLiteral("egm96");
        case TerrainVerticalDatum::Egm2008:
            return QStringLiteral("egm2008");
        case TerrainVerticalDatum::Local:
            return QStringLiteral("local");
        case TerrainVerticalDatum::Unknown:
        default:
            return QStringLiteral("unknown");
    }
}

std::optional<TerrainVerticalDatum> terrainVerticalDatumFromId(const QString &id)
{
    const QString normalized = id.trimmed().toLower();
    if (normalized == QStringLiteral("unknown"))
        return TerrainVerticalDatum::Unknown;
    if (normalized == QStringLiteral("wgs84-ellipsoid"))
        return TerrainVerticalDatum::Wgs84Ellipsoid;
    if (normalized == QStringLiteral("egm96"))
        return TerrainVerticalDatum::Egm96;
    if (normalized == QStringLiteral("egm2008"))
        return TerrainVerticalDatum::Egm2008;
    if (normalized == QStringLiteral("local"))
        return TerrainVerticalDatum::Local;
    return std::nullopt;
}

QString terrainVerticalDatumDisplayName(TerrainVerticalDatum datum)
{
    switch (datum)
    {
        case TerrainVerticalDatum::Wgs84Ellipsoid:
            return QStringLiteral("WGS 84 ellipsoidal height");
        case TerrainVerticalDatum::Egm96:
            return QStringLiteral("EGM96 geoid height");
        case TerrainVerticalDatum::Egm2008:
            return QStringLiteral("EGM2008 geoid height");
        case TerrainVerticalDatum::Local:
            return QStringLiteral("Local/project vertical datum");
        case TerrainVerticalDatum::Unknown:
        default:
            return QStringLiteral("Unknown vertical datum");
    }
}

QString terrainVerticalDatumAuthorityCode(TerrainVerticalDatum datum)
{
    switch (datum)
    {
        case TerrainVerticalDatum::Wgs84Ellipsoid:
            // EPSG:4979 is the 3D WGS 84 geographic CRS carrying ellipsoidal height.
            return QStringLiteral("EPSG:4979");
        case TerrainVerticalDatum::Egm96:
            return QStringLiteral("EPSG:5773");
        case TerrainVerticalDatum::Egm2008:
            return QStringLiteral("EPSG:3855");
        case TerrainVerticalDatum::Local:
        case TerrainVerticalDatum::Unknown:
        default:
            return QString();
    }
}

TerrainVerticalReference terrainVerticalReference(TerrainVerticalDatum datum)
{
    switch (datum)
    {
        case TerrainVerticalDatum::Wgs84Ellipsoid:
            return TerrainVerticalReference::Ellipsoidal;
        case TerrainVerticalDatum::Egm96:
        case TerrainVerticalDatum::Egm2008:
            return TerrainVerticalReference::Orthometric;
        case TerrainVerticalDatum::Local:
            return TerrainVerticalReference::Local;
        case TerrainVerticalDatum::Unknown:
        default:
            return TerrainVerticalReference::Unknown;
    }
}

QString terrainVerticalReferenceId(TerrainVerticalReference reference)
{
    switch (reference)
    {
        case TerrainVerticalReference::Ellipsoidal:
            return QStringLiteral("ellipsoidal");
        case TerrainVerticalReference::Orthometric:
            return QStringLiteral("orthometric");
        case TerrainVerticalReference::Local:
            return QStringLiteral("local");
        case TerrainVerticalReference::Unknown:
        default:
            return QStringLiteral("unknown");
    }
}

bool isValidTerrainVerticalDatum(TerrainVerticalDatum datum)
{
    switch (datum)
    {
        case TerrainVerticalDatum::Unknown:
        case TerrainVerticalDatum::Wgs84Ellipsoid:
        case TerrainVerticalDatum::Egm96:
        case TerrainVerticalDatum::Egm2008:
        case TerrainVerticalDatum::Local:
            return true;
        default:
            return false;
    }
}

bool isExplicitTerrainVerticalDatum(TerrainVerticalDatum datum)
{
    return isValidTerrainVerticalDatum(datum) && datum != TerrainVerticalDatum::Unknown;
}

bool isRequestableTerrainVerticalDatum(TerrainVerticalDatum datum)
{
    // "local" is intentionally not requestable: v1 has no globally unique local-datum
    // identifier, so treating two unrelated local datums as interchangeable would be unsafe.
    return datum == TerrainVerticalDatum::Wgs84Ellipsoid ||
           datum == TerrainVerticalDatum::Egm96 ||
           datum == TerrainVerticalDatum::Egm2008;
}

QString terrainDataOriginId(TerrainDataOrigin origin)
{
    switch (origin)
    {
        case TerrainDataOrigin::Cache:
            return QStringLiteral("cache");
        case TerrainDataOrigin::Remote:
            return QStringLiteral("remote");
        case TerrainDataOrigin::OfflinePackage:
            return QStringLiteral("offline-package");
        case TerrainDataOrigin::LocalDataset:
            return QStringLiteral("local-dataset");
        case TerrainDataOrigin::Unknown:
        default:
            return QStringLiteral("unknown");
    }
}

std::optional<TerrainDataOrigin> terrainDataOriginFromId(const QString &id)
{
    const QString normalized = id.trimmed().toLower();
    if (normalized == QStringLiteral("unknown"))
        return TerrainDataOrigin::Unknown;
    if (normalized == QStringLiteral("cache"))
        return TerrainDataOrigin::Cache;
    if (normalized == QStringLiteral("remote"))
        return TerrainDataOrigin::Remote;
    if (normalized == QStringLiteral("offline-package"))
        return TerrainDataOrigin::OfflinePackage;
    if (normalized == QStringLiteral("local-dataset"))
        return TerrainDataOrigin::LocalDataset;
    return std::nullopt;
}

bool isValidTerrainElevationSample(const TerrainElevationSample &sample)
{
    if (!std::isfinite(sample.elevation_m))
        return false;
    if (!std::isfinite(sample.nominal_resolution_m) || sample.nominal_resolution_m < 0.0)
        return false;
    if (sample.dataset.trimmed().isEmpty())
        return false;
    if (!isValidTerrainVerticalDatum(sample.vertical_datum) ||
        !isValidTerrainVerticalDatum(sample.source_vertical_datum))
    {
        return false;
    }

    // A transformed result cannot claim a known target datum if the source datum is unknown.
    if (sample.source_vertical_datum == TerrainVerticalDatum::Unknown &&
        sample.vertical_datum != TerrainVerticalDatum::Unknown)
    {
        return false;
    }

    return true;
}

} // namespace Aowis::Map
