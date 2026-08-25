#include <aowis/map/terrain_data.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QString>
#include <QTemporaryDir>

#include <iostream>

namespace
{
bool require(bool condition, const char *message)
{
    if (condition)
        return true;

    std::cerr << message << '\n';
    return false;
}

Aowis::Map::TerrainTile makeTile()
{
    Aowis::Map::TerrainTile tile;
    tile.address.zoom = 6;
    tile.address.x = 34;
    tile.address.y = 21;
    tile.dataset = QStringLiteral("renderer-test");
    tile.nominal_resolution_m = 30.0;
    tile.vertical_datum = Aowis::Map::TerrainVerticalDatum::Egm96;
    tile.elevations_m.resize(Aowis::Map::TerrainTileSampleCount);

    for (int row = 0; row < Aowis::Map::TerrainTileGridSize; ++row)
    {
        for (int column = 0; column < Aowis::Map::TerrainTileGridSize; ++column)
        {
            const int index = row * Aowis::Map::TerrainTileGridSize + column;
            tile.elevations_m[index] = 50.0f + float(row) * 2.0f + float(column) * 0.5f;
        }
    }

    return tile;
}

bool writeEncodedTile(const Aowis::Map::TerrainData &terrain_data,
                      const Aowis::Map::TerrainTile &tile,
                      QByteArray *encoded_data,
                      QString *error_message)
{
    *encoded_data = Aowis::Map::encodeTerrainTile(tile, error_message);
    if (encoded_data->isEmpty())
        return false;

    const QString path = terrain_data.normalizedTilePath(tile.dataset, tile.address);
    const QFileInfo file_info(path);
    QDir directory;
    if (!directory.mkpath(file_info.absolutePath()))
    {
        *error_message = QStringLiteral("Failed to create normalized test tile directory");
        return false;
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
    {
        *error_message = file.errorString();
        return false;
    }
    if (file.write(*encoded_data) != encoded_data->size())
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
}

int main()
{
    using namespace Aowis::Map;

    QTemporaryDir temporary_directory;
    if (!require(temporary_directory.isValid(),
                 "Failed to create terrain delivery test directory"))
    {
        return 1;
    }

    TerrainData::Config config;
    config.enabled = true;
    config.remote_fetch_enabled = false;
    config.cache_directory = temporary_directory.path();

    TerrainData terrain_data(config);
    QString error_message;
    if (!require(terrain_data.initialize(&error_message),
                 "Failed to initialize terrain delivery test subsystem"))
    {
        return 1;
    }

    if (!require(terrainTileLookupStatusId(TerrainTileLookupStatus::Ready) ==
                     QStringLiteral("ready") &&
                 terrainTileLookupStatusId(TerrainTileLookupStatus::CorruptTile) ==
                     QStringLiteral("corrupt-tile"),
                 "Terrain tile lookup status identifiers are not stable"))
    {
        return 1;
    }

    const TerrainTile tile = makeTile();
    QByteArray encoded_data;
    if (!require(writeEncodedTile(terrain_data, tile, &encoded_data, &error_message),
                 "Failed to write normalized renderer terrain tile"))
    {
        return 1;
    }

    TerrainTileLookupResult result = terrain_data.terrainTile(tile.dataset, tile.address);
    if (!require(result.status == TerrainTileLookupStatus::Ready,
                 "Cached normalized terrain tile was not delivered"))
    {
        return 1;
    }
    if (!require(result.data == encoded_data,
                 "Terrain delivery did not preserve the canonical encoded tile bytes"))
    {
        return 1;
    }

    TerrainTileAddress missing_address = tile.address;
    ++missing_address.x;
    result = terrain_data.terrainTile(tile.dataset, missing_address);
    if (!require(result.status == TerrainTileLookupStatus::TileUnavailable,
                 "Missing normalized terrain tile did not report unavailable"))
    {
        return 1;
    }

    result = terrain_data.terrainTile(QStringLiteral("../bad"), tile.address);
    if (!require(result.status == TerrainTileLookupStatus::InvalidDataset,
                 "Unsafe terrain dataset identifier was accepted for delivery"))
    {
        return 1;
    }

    TerrainTileAddress invalid_address = tile.address;
    invalid_address.zoom = TerrainTileMaximumZoom + 1;
    result = terrain_data.terrainTile(tile.dataset, invalid_address);
    if (!require(result.status == TerrainTileLookupStatus::InvalidAddress,
                 "Invalid terrain tile address was accepted for delivery"))
    {
        return 1;
    }

    const QString tile_path = terrain_data.normalizedTilePath(tile.dataset, tile.address);
    QFile corrupt_file(tile_path);
    if (!require(corrupt_file.open(QIODevice::ReadWrite),
                 "Failed to open renderer terrain tile for corruption test"))
    {
        return 1;
    }
    if (!require(corrupt_file.seek(corrupt_file.size() - 1),
                 "Failed to seek renderer terrain corruption test byte"))
    {
        return 1;
    }
    const QByteArray original_byte = corrupt_file.read(1);
    if (!require(original_byte.size() == 1,
                 "Failed to read renderer terrain corruption test byte"))
    {
        return 1;
    }
    if (!require(corrupt_file.seek(corrupt_file.size() - 1),
                 "Failed to reseek renderer terrain corruption test tile"))
    {
        return 1;
    }
    const char changed_byte = char(uchar(original_byte.at(0)) ^ 0x01u);
    if (!require(corrupt_file.write(&changed_byte, 1) == 1,
                 "Failed to corrupt renderer terrain test tile"))
    {
        return 1;
    }
    corrupt_file.close();

    result = terrain_data.terrainTile(tile.dataset, tile.address);
    if (!require(result.status == TerrainTileLookupStatus::CorruptTile,
                 "Corrupt terrain tile was delivered to the renderer"))
    {
        return 1;
    }

    TerrainData::Config uninitialized_config;
    uninitialized_config.enabled = true;
    uninitialized_config.cache_directory = temporary_directory.path();
    TerrainData uninitialized_terrain(uninitialized_config);
    result = uninitialized_terrain.terrainTile(tile.dataset, tile.address);
    if (!require(result.status == TerrainTileLookupStatus::NotInitialized,
                 "Uninitialized terrain subsystem allowed tile delivery"))
    {
        return 1;
    }

    TerrainData::Config disabled_config;
    disabled_config.enabled = false;
    disabled_config.cache_directory = temporary_directory.path();
    TerrainData disabled_terrain(disabled_config);
    if (!require(disabled_terrain.initialize(&error_message),
                 "Disabled terrain subsystem failed to initialize"))
    {
        return 1;
    }
    result = disabled_terrain.terrainTile(tile.dataset, tile.address);
    if (!require(result.status == TerrainTileLookupStatus::Disabled,
                 "Disabled terrain subsystem allowed tile delivery"))
    {
        return 1;
    }

    return 0;
}
