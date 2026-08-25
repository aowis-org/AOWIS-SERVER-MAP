#ifndef AOWIS_MAP_TERRAIN_TILE_H
#define AOWIS_MAP_TERRAIN_TILE_H

#include <aowis/map/terrain_elevation.h>

#include <QByteArray>
#include <QString>
#include <QVector>

#include <optional>

namespace Aowis::Map
{

inline constexpr quint16 TerrainTileFormatVersion = 1;
inline constexpr int TerrainTileGridSize = 65;
inline constexpr int TerrainTileCellCount = TerrainTileGridSize - 1;
inline constexpr int TerrainTileSampleCount = TerrainTileGridSize * TerrainTileGridSize;
inline constexpr quint16 TerrainTileNoDataCode = 65535;
inline constexpr quint16 TerrainTileMaximumElevationCode = 65534;
inline constexpr int TerrainTileMaximumZoom = 30;
inline constexpr qsizetype TerrainTileMaximumDatasetIdBytes = 128;

struct TerrainTileAddress
{
    int zoom = 0;
    quint32 x = 0;
    quint32 y = 0;
};

struct TerrainTile
{
    TerrainTileAddress address;
    QString dataset;
    double nominal_resolution_m = 0.0;
    TerrainVerticalDatum vertical_datum = TerrainVerticalDatum::Unknown;

    // Row-major, north-to-south then west-to-east. NaN represents no-data in memory.
    QVector<float> elevations_m;
};

bool isValidTerrainDatasetId(const QString &dataset);
bool isValidTerrainTileAddress(const TerrainTileAddress &address);
bool isValidTerrainTile(const TerrainTile &tile, QString *error_message = nullptr);

QString terrainTileFileExtension();
QString terrainTileMimeType();
QString normalizedTerrainTileRelativePath(const QString &dataset,
                                          const TerrainTileAddress &address);

QByteArray encodeTerrainTile(const TerrainTile &tile, QString *error_message = nullptr);
std::optional<TerrainTile> decodeTerrainTile(const QByteArray &data,
                                             QString *error_message = nullptr);

} // namespace Aowis::Map

#endif // AOWIS_MAP_TERRAIN_TILE_H
