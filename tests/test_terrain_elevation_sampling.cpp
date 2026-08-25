#include <aowis/map/terrain_data.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QString>
#include <QTemporaryDir>

#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>

namespace
{
bool require(bool condition, const char *message)
{
    if (condition)
        return true;

    std::cerr << message << '\n';
    return false;
}

bool writeTile(const Aowis::Map::TerrainData &terrain_data,
               const Aowis::Map::TerrainTile &tile,
               QString *error_message)
{
    const QString path = terrain_data.normalizedTilePath(tile.dataset, tile.address);
    if (path.isEmpty())
    {
        *error_message = QStringLiteral("Normalized tile path is empty");
        return false;
    }

    const QFileInfo file_info(path);
    QDir directory;
    if (!directory.mkpath(file_info.absolutePath()))
    {
        *error_message = QStringLiteral("Failed to create test tile directory");
        return false;
    }

    const QByteArray encoded = Aowis::Map::encodeTerrainTile(tile, error_message);
    if (encoded.isEmpty())
        return false;

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
    {
        *error_message = file.errorString();
        return false;
    }
    if (file.write(encoded) != encoded.size())
    {
        *error_message = file.errorString();
        file.cancelWriting();
        return false;
    }
    if (!file.commit())
    {
        *error_message = file.errorString();
        return false;
    }
    return true;
}

void coordinateForTileGridPosition(const Aowis::Map::TerrainTileAddress &address,
                                   double column,
                                   double row,
                                   double *latitude_deg,
                                   double *longitude_deg)
{
    const double tile_count = double(quint64(1) << address.zoom);
    const double tile_x = double(address.x) + column / double(Aowis::Map::TerrainTileCellCount);
    const double tile_y = double(address.y) + row / double(Aowis::Map::TerrainTileCellCount);

    *longitude_deg = tile_x / tile_count * 360.0 - 180.0;
    const double mercator_y = std::numbers::pi * (1.0 - 2.0 * tile_y / tile_count);
    *latitude_deg = std::atan(std::sinh(mercator_y)) * 180.0 / std::numbers::pi;
}

Aowis::Map::TerrainTile makeGradientTile(const Aowis::Map::TerrainTileAddress &address,
                                         const QString &dataset)
{
    Aowis::Map::TerrainTile tile;
    tile.address = address;
    tile.dataset = dataset;
    tile.nominal_resolution_m = 30.0;
    tile.vertical_datum = Aowis::Map::TerrainVerticalDatum::Egm96;
    tile.elevations_m.resize(Aowis::Map::TerrainTileSampleCount);

    for (int row = 0; row < Aowis::Map::TerrainTileGridSize; ++row)
    {
        for (int column = 0; column < Aowis::Map::TerrainTileGridSize; ++column)
        {
            const int index = row * Aowis::Map::TerrainTileGridSize + column;
            tile.elevations_m[index] = 100.0f + float(row) * 10.0f + float(column);
        }
    }

    return tile;
}

Aowis::Map::TerrainTile makeConstantTile(const Aowis::Map::TerrainTileAddress &address,
                                         const QString &dataset,
                                         float elevation_m)
{
    Aowis::Map::TerrainTile tile;
    tile.address = address;
    tile.dataset = dataset;
    tile.nominal_resolution_m = 15.0;
    tile.vertical_datum = Aowis::Map::TerrainVerticalDatum::Egm96;
    tile.elevations_m.fill(elevation_m, Aowis::Map::TerrainTileSampleCount);
    return tile;
}
}

int main()
{
    using namespace Aowis::Map;

    QTemporaryDir temporary_directory;
    if (!require(temporary_directory.isValid(), "Failed to create terrain sampling test directory"))
        return 1;

    TerrainData::Config config;
    config.enabled = true;
    config.remote_fetch_enabled = false;
    config.cache_directory = temporary_directory.path();
    TerrainData terrain_data(config);

    QString error_message;
    if (!require(terrain_data.initialize(&error_message), "Failed to initialize TerrainData"))
        return 1;
    if (!require(error_message.isEmpty(), "TerrainData initialization returned an unexpected error"))
        return 1;

    if (!require(terrainElevationLookupStatusId(TerrainElevationLookupStatus::Ready) ==
                     QStringLiteral("ready") &&
                     terrainElevationLookupStatusId(TerrainElevationLookupStatus::CorruptTile) ==
                     QStringLiteral("corrupt-tile"),
                 "Terrain lookup status identifiers are not stable"))
    {
        return 1;
    }

    TerrainData::Config uninitialized_config;
    uninitialized_config.enabled = true;
    uninitialized_config.cache_directory = temporary_directory.path();
    TerrainData uninitialized_terrain(uninitialized_config);
    TerrainElevationLookupResult uninitialized_result =
        uninitialized_terrain.sampleElevation(QStringLiteral("test-dem"), 0.0, 0.0);
    if (!require(uninitialized_result.status == TerrainElevationLookupStatus::NotInitialized,
                 "Uninitialized terrain subsystem did not reject point lookup"))
    {
        return 1;
    }

    const QString dataset = QStringLiteral("test-dem");
    TerrainTileAddress coarse_address;
    coarse_address.zoom = 2;
    coarse_address.x = 2;
    coarse_address.y = 1;

    const TerrainTile coarse_tile = makeGradientTile(coarse_address, dataset);
    if (!require(writeTile(terrain_data, coarse_tile, &error_message),
                 "Failed to write coarse normalized terrain tile"))
    {
        return 1;
    }

    const double expected_column = 16.25;
    const double expected_row = 32.5;
    double latitude_deg = 0.0;
    double longitude_deg = 0.0;
    coordinateForTileGridPosition(coarse_address, expected_column, expected_row,
                                  &latitude_deg, &longitude_deg);

    TerrainElevationLookupResult result =
        terrain_data.sampleElevation(dataset, latitude_deg, longitude_deg);
    if (!require(result.status == TerrainElevationLookupStatus::Ready,
                 "Cached terrain point lookup did not succeed"))
    {
        return 1;
    }
    if (!require(result.sample.has_value(), "Successful lookup did not contain a terrain sample"))
        return 1;
    if (!require(result.tile_address.has_value() && result.tile_address->zoom == 2,
                 "Lookup did not report the sampled terrain tile"))
    {
        return 1;
    }

    const double expected_elevation_m = 100.0 + expected_row * 10.0 + expected_column;
    if (!require(std::abs(result.sample->elevation_m - expected_elevation_m) < 0.02,
                 "Bilinear terrain elevation interpolation is incorrect"))
    {
        return 1;
    }
    if (!require(result.sample->dataset == dataset &&
                     result.sample->vertical_datum == TerrainVerticalDatum::Egm96 &&
                     result.sample->origin == TerrainDataOrigin::Cache,
                 "Terrain elevation sample metadata is incorrect"))
    {
        return 1;
    }

    const double tile_count_zoom3 = 8.0;
    const double continuous_x_zoom3 = (longitude_deg + 180.0) / 360.0 * tile_count_zoom3;
    const double latitude_rad = latitude_deg * std::numbers::pi / 180.0;
    const double continuous_y_zoom3 =
        (1.0 - std::asinh(std::tan(latitude_rad)) / std::numbers::pi) * 0.5 * tile_count_zoom3;

    TerrainTileAddress fine_address;
    fine_address.zoom = 3;
    fine_address.x = quint32(std::floor(continuous_x_zoom3));
    fine_address.y = quint32(std::floor(continuous_y_zoom3));
    TerrainTile fine_tile = makeConstantTile(fine_address, dataset, 900.0f);
    if (!require(writeTile(terrain_data, fine_tile, &error_message),
                 "Failed to write fine normalized terrain tile"))
    {
        return 1;
    }

    result = terrain_data.sampleElevation(dataset, latitude_deg, longitude_deg);
    if (!require(result.status == TerrainElevationLookupStatus::Ready &&
                     result.tile_address.has_value() && result.tile_address->zoom == 3 &&
                     result.sample.has_value() &&
                     std::abs(result.sample->elevation_m - 900.0) < 0.001,
                 "Point lookup did not prefer the finest cached terrain tile"))
    {
        return 1;
    }

    fine_tile.elevations_m.fill(std::numeric_limits<float>::quiet_NaN());
    fine_tile.elevations_m[0] = 900.0f;
    if (!require(writeTile(terrain_data, fine_tile, &error_message),
                 "Failed to write fine no-data test tile"))
    {
        return 1;
    }

    result = terrain_data.sampleElevation(dataset, latitude_deg, longitude_deg);
    if (!require(result.status == TerrainElevationLookupStatus::Ready &&
                     result.tile_address.has_value() && result.tile_address->zoom == 2 &&
                     result.sample.has_value() &&
                     std::abs(result.sample->elevation_m - expected_elevation_m) < 0.02,
                 "No-data in a fine tile did not fall back to the coarser cached tile"))
    {
        return 1;
    }

    TerrainTileAddress isolated_address;
    isolated_address.zoom = 4;
    isolated_address.x = 1;
    isolated_address.y = 1;
    TerrainTile isolated_tile = makeConstantTile(isolated_address, dataset, 42.0f);
    isolated_tile.elevations_m.fill(std::numeric_limits<float>::quiet_NaN());
    isolated_tile.elevations_m[0] = 42.0f;
    if (!require(writeTile(terrain_data, isolated_tile, &error_message),
                 "Failed to write isolated no-data terrain tile"))
    {
        return 1;
    }

    double isolated_latitude_deg = 0.0;
    double isolated_longitude_deg = 0.0;
    coordinateForTileGridPosition(isolated_address, 31.5, 31.5,
                                  &isolated_latitude_deg, &isolated_longitude_deg);
    result = terrain_data.sampleElevation(dataset, isolated_latitude_deg, isolated_longitude_deg);
    if (!require(result.status == TerrainElevationLookupStatus::NoData,
                 "Terrain lookup did not report no-data when cached tiles lacked a usable sample"))
    {
        return 1;
    }

    result = terrain_data.sampleElevation(QStringLiteral("missing-dem"), latitude_deg, longitude_deg);
    if (!require(result.status == TerrainElevationLookupStatus::TileUnavailable,
                 "Missing cached terrain dataset did not report tile unavailable"))
    {
        return 1;
    }

    const QString edge_dataset = QStringLiteral("edge-dem");
    TerrainTileAddress edge_address;
    edge_address.zoom = 1;
    edge_address.x = 1;
    edge_address.y = 0;
    const TerrainTile edge_tile = makeConstantTile(edge_address, edge_dataset, 321.0f);
    if (!require(writeTile(terrain_data, edge_tile, &error_message),
                 "Failed to write antimeridian edge terrain tile"))
    {
        return 1;
    }

    double edge_latitude_deg = 0.0;
    double edge_longitude_deg = 0.0;
    coordinateForTileGridPosition(edge_address, 64.0, 32.0,
                                  &edge_latitude_deg, &edge_longitude_deg);
    if (!require(std::abs(edge_longitude_deg - 180.0) < 1e-12,
                 "Antimeridian test coordinate was not generated at +180 degrees"))
    {
        return 1;
    }
    result = terrain_data.sampleElevation(edge_dataset, edge_latitude_deg, edge_longitude_deg);
    if (!require(result.status == TerrainElevationLookupStatus::Ready &&
                     result.tile_address.has_value() && result.tile_address->x == 1 &&
                     result.sample.has_value() &&
                     std::abs(result.sample->elevation_m - 321.0) < 0.001,
                 "Terrain lookup did not sample the east edge correctly at +180 longitude"))
    {
        return 1;
    }

    result = terrain_data.sampleElevation(dataset, 89.0, 0.0);
    if (!require(result.status == TerrainElevationLookupStatus::OutsideCoverage,
                 "Terrain lookup did not reject coordinates outside Web-Mercator coverage"))
    {
        return 1;
    }

    result = terrain_data.sampleElevation(QStringLiteral("../bad"), latitude_deg, longitude_deg);
    if (!require(result.status == TerrainElevationLookupStatus::InvalidDataset,
                 "Terrain lookup accepted an unsafe dataset identifier"))
    {
        return 1;
    }

    result = terrain_data.sampleElevation(dataset,
                                          std::numeric_limits<double>::quiet_NaN(),
                                          longitude_deg);
    if (!require(result.status == TerrainElevationLookupStatus::InvalidCoordinate,
                 "Terrain lookup accepted a non-finite coordinate"))
    {
        return 1;
    }

    const QString fine_path = terrain_data.normalizedTilePath(dataset, fine_address);
    QFile corrupt_file(fine_path);
    if (!require(corrupt_file.open(QIODevice::ReadWrite), "Failed to open terrain tile for corruption test"))
        return 1;
    if (!require(corrupt_file.seek(corrupt_file.size() - 1), "Failed to seek terrain corruption test tile"))
        return 1;
    const QByteArray byte = corrupt_file.read(1);
    if (!require(byte.size() == 1, "Failed to read terrain corruption test byte"))
        return 1;
    if (!require(corrupt_file.seek(corrupt_file.size() - 1), "Failed to reseek terrain corruption test tile"))
        return 1;
    const char corrupted_byte = char(uchar(byte.at(0)) ^ 0x01u);
    if (!require(corrupt_file.write(&corrupted_byte, 1) == 1,
                 "Failed to corrupt terrain test tile"))
    {
        return 1;
    }
    corrupt_file.close();

    result = terrain_data.sampleElevation(dataset, latitude_deg, longitude_deg);
    if (!require(result.status == TerrainElevationLookupStatus::CorruptTile,
                 "Corrupt finest terrain tile was silently ignored"))
    {
        return 1;
    }

    TerrainData::Config disabled_config;
    disabled_config.enabled = false;
    disabled_config.cache_directory = temporary_directory.path();
    TerrainData disabled_terrain(disabled_config);
    if (!require(disabled_terrain.initialize(&error_message),
                 "Disabled TerrainData failed to initialize"))
    {
        return 1;
    }
    result = disabled_terrain.sampleElevation(dataset, latitude_deg, longitude_deg);
    if (!require(result.status == TerrainElevationLookupStatus::Disabled,
                 "Disabled terrain subsystem did not report disabled point lookup"))
    {
        return 1;
    }

    return 0;
}
