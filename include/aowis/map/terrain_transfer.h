#ifndef AOWIS_MAP_TERRAIN_TRANSFER_H
#define AOWIS_MAP_TERRAIN_TRANSFER_H

#include <aowis/map/terrain_tile.h>

#include <QByteArray>
#include <QString>
#include <optional>

namespace Aowis::Map
{
// Maximum canonical r2 tile size, including variable-length dataset ID.
inline constexpr qsizetype MaximumCanonicalTerrainTileBytes =
    64 + TerrainTileMaximumDatasetIdBytes + TerrainTileSampleCount * qsizetype(sizeof(quint16));

// A single zstd frame, with its uncompressed size recorded. No dictionaries,
// streaming, or changes to the canonical .aowterrain cache representation.
qsizetype maximumCompressedTerrainTileBytes();
std::optional<QByteArray> compressTerrainTileForTransfer(const QByteArray &canonical);
std::optional<QByteArray> decompressTerrainTileFromTransfer(
    const QByteArray &compressed, QString *error_message = nullptr);
}

#endif // AOWIS_MAP_TERRAIN_TRANSFER_H
