#include <aowis/map/terrain_tile.h>

#include <QByteArray>
#include <QString>

#include <cmath>
#include <iostream>
#include <limits>

namespace
{
bool require(bool condition, const char *message)
{
    if (condition)
        return true;

    std::cerr << message << '\n';
    return false;
}
}

int main()
{
    using namespace Aowis::Map;

    TerrainTile tile;
    tile.address.zoom = 3;
    tile.address.x = 4;
    tile.address.y = 2;
    tile.dataset = QStringLiteral("test-dem");
    tile.nominal_resolution_m = 30.0;
    tile.vertical_datum = TerrainVerticalDatum::Egm96;
    tile.elevations_m.resize(TerrainTileSampleCount);

    for (int row = 0; row < TerrainTileGridSize; ++row)
    {
        for (int column = 0; column < TerrainTileGridSize; ++column)
        {
            const int index = row * TerrainTileGridSize + column;
            tile.elevations_m[index] = 100.0f + float(row) * 0.5f + float(column) * 0.25f;
        }
    }
    tile.elevations_m[123] = std::numeric_limits<float>::quiet_NaN();

    tile.source_statistics = TerrainSourceStatistics{-200.0f, 4500.0f, 12345678901ULL, 3, 2};
    tile.geometric_error = TerrainGeometricError{1234.125, 250.25, 16.5, 9876543210ULL, 4, 5, 6};
    QString error_message;
    const QByteArray encoded = encodeTerrainTile(tile, &error_message);
    if (!require(!encoded.isEmpty(), "Failed to encode terrain tile"))
        return 1;
    if (!require(error_message.isEmpty(), "Encoder returned an unexpected error message"))
        return 1;

    const std::optional<TerrainTile> decoded = decodeTerrainTile(encoded, &error_message);
    if (!require(decoded.has_value(), "Failed to decode encoded terrain tile"))
        return 1;
    if (!require(error_message.isEmpty(), "Decoder returned an unexpected error message"))
        return 1;

    const TerrainTile &round_trip = decoded.value();
    if (!require(round_trip.source_statistics && round_trip.geometric_error &&
                 round_trip.source_statistics->minimum_elevation_m == -200.0f &&
                 round_trip.source_statistics->maximum_elevation_m == 4500.0f &&
                 round_trip.source_statistics->valid_pixel_count == 12345678901ULL &&
                 round_trip.source_statistics->no_data_pixel_count == 3 &&
                 round_trip.source_statistics->missing_source_cells == 2 &&
                 round_trip.geometric_error->maximum_underestimation_m == 1234.125 &&
                 round_trip.geometric_error->maximum_overestimation_m == 250.25 &&
                 round_trip.geometric_error->rms_geometric_error_m == 16.5 &&
                 round_trip.geometric_error->compared_pixel_count == 9876543210ULL &&
                 round_trip.geometric_error->no_data_pixel_count == 4 &&
                 round_trip.geometric_error->missing_surface_pixel_count == 5 &&
                 round_trip.geometric_error->missing_source_cells == 6,
                 "LOD metadata round trip failed")) return 1;
    TerrainTile absent = tile;
    absent.source_statistics.reset(); absent.geometric_error.reset();
    const std::optional<TerrainTile> absent_round_trip = decodeTerrainTile(encodeTerrainTile(absent));
    if (!require(absent_round_trip && !absent_round_trip->source_statistics &&
                 !absent_round_trip->geometric_error, "Absent LOD metadata became present")) return 1;
    TerrainTile invalid = tile;
    invalid.geometric_error->maximum_underestimation_m = -1;
    if (!require(encodeTerrainTile(invalid).isEmpty(), "Negative error was encoded")) return 1;
    QByteArray old_version = encoded;
    old_version[8] = char(1);
    if (!require(!decodeTerrainTile(old_version), "v1 format was accepted")) return 1;
    QByteArray damaged_metadata = encoded;
    damaged_metadata[64 + tile.dataset.toUtf8().size() + 4] = char(1);
    if (!require(!decodeTerrainTile(damaged_metadata), "Damaged metadata passed CRC")) return 1;
    if (!require(round_trip.address.zoom == tile.address.zoom &&
                     round_trip.address.x == tile.address.x &&
                     round_trip.address.y == tile.address.y,
                 "Terrain tile address changed during round-trip"))
    {
        return 1;
    }
    if (!require(round_trip.dataset == tile.dataset,
                 "Terrain dataset identifier changed during round-trip"))
    {
        return 1;
    }
    if (!require(round_trip.vertical_datum == tile.vertical_datum,
                 "Terrain vertical datum changed during round-trip"))
    {
        return 1;
    }
    if (!require(round_trip.elevations_m.size() == TerrainTileSampleCount,
                 "Terrain sample count changed during round-trip"))
    {
        return 1;
    }
    if (!require(std::isnan(double(round_trip.elevations_m.at(123))),
                 "Terrain no-data sample was not preserved"))
    {
        return 1;
    }

    for (int index = 0; index < TerrainTileSampleCount; ++index)
    {
        if (index == 123)
            continue;

        const double difference = std::abs(
            double(round_trip.elevations_m.at(index)) - double(tile.elevations_m.at(index)));
        if (!require(difference < 0.01,
                     "Terrain quantization exceeded expected round-trip tolerance"))
        {
            return 1;
        }
    }

    const QString expected_path = QStringLiteral("v2/r2/test-dem/3/4/2.aowterrain");
    if (!require(normalizedTerrainTileRelativePath(tile.dataset, tile.address) == expected_path,
                 "Normalized terrain tile path is not canonical"))
    {
        return 1;
    }

    QByteArray corrupted = encoded;
    corrupted[corrupted.size() - 1] = char(uchar(corrupted.at(corrupted.size() - 1)) ^ 0x01u);
    error_message.clear();
    if (!require(!decodeTerrainTile(corrupted, &error_message).has_value(),
                 "Corrupted terrain payload unexpectedly decoded"))
    {
        return 1;
    }
    if (!require(error_message.contains(QStringLiteral("CRC32")),
                 "Corrupted terrain payload did not report a CRC32 error"))
    {
        return 1;
    }

    if (!require(!isValidTerrainDatasetId(QStringLiteral("../bad")),
                 "Unsafe terrain dataset identifier was accepted"))
    {
        return 1;
    }

    TerrainTile invalid_datum_tile = tile;
    invalid_datum_tile.vertical_datum = static_cast<TerrainVerticalDatum>(255);
    error_message.clear();
    if (!require(encodeTerrainTile(invalid_datum_tile, &error_message).isEmpty(),
                 "Invalid terrain vertical datum was silently encoded"))
    {
        return 1;
    }
    if (!require(error_message.contains(QStringLiteral("vertical datum")),
                 "Invalid terrain vertical datum did not report a useful error"))
    {
        return 1;
    }

    return 0;
}
