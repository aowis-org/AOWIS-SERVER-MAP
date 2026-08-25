#ifndef AOWIS_MAP_TERRAIN_ELEVATION_H
#define AOWIS_MAP_TERRAIN_ELEVATION_H

#include <QString>

#include <optional>

namespace Aowis::Map
{

enum class TerrainVerticalDatum
{
    Unknown,
    Wgs84Ellipsoid,
    Egm96,
    Egm2008,
    Local
};

enum class TerrainVerticalReference
{
    Unknown,
    Ellipsoidal,
    Orthometric,
    Local
};

enum class TerrainDataOrigin
{
    Unknown,
    Cache,
    Remote,
    OfflinePackage,
    LocalDataset
};

struct TerrainElevationSample
{
    double elevation_m = 0.0;
    QString dataset;
    double nominal_resolution_m = 0.0;

    // Datum of elevation_m after any requested vertical transformation.
    TerrainVerticalDatum vertical_datum = TerrainVerticalDatum::Unknown;

    // Datum carried by the normalized source tile before any transformation.
    TerrainVerticalDatum source_vertical_datum = TerrainVerticalDatum::Unknown;
    TerrainDataOrigin origin = TerrainDataOrigin::Unknown;
};

QString terrainVerticalDatumId(TerrainVerticalDatum datum);
std::optional<TerrainVerticalDatum> terrainVerticalDatumFromId(const QString &id);
QString terrainVerticalDatumDisplayName(TerrainVerticalDatum datum);
QString terrainVerticalDatumAuthorityCode(TerrainVerticalDatum datum);
TerrainVerticalReference terrainVerticalReference(TerrainVerticalDatum datum);
QString terrainVerticalReferenceId(TerrainVerticalReference reference);
bool isValidTerrainVerticalDatum(TerrainVerticalDatum datum);
bool isExplicitTerrainVerticalDatum(TerrainVerticalDatum datum);
bool isRequestableTerrainVerticalDatum(TerrainVerticalDatum datum);

QString terrainDataOriginId(TerrainDataOrigin origin);
std::optional<TerrainDataOrigin> terrainDataOriginFromId(const QString &id);
bool isValidTerrainElevationSample(const TerrainElevationSample &sample);

} // namespace Aowis::Map

#endif // AOWIS_MAP_TERRAIN_ELEVATION_H
