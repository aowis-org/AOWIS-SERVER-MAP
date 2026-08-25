#include <aowis/map/terrain_data.h>
#include <aowis/map/terrain_provider.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <tiffio.h>

#include <cmath>
#include <iostream>
#include <numbers>
#include <vector>

namespace
{
bool require(bool condition, const char *message)
{
    if (condition)
        return true;

    std::cerr << message << '\n';
    return false;
}

Aowis::Map::TerrainTileAddress addressForCoordinate(double latitude_deg,
                                                    double longitude_deg,
                                                    int zoom)
{
    const double tile_count = std::ldexp(1.0, zoom);
    const double tile_x = (longitude_deg + 180.0) / 360.0 * tile_count;
    const double latitude_rad = latitude_deg * std::numbers::pi / 180.0;
    const double tile_y =
        (1.0 - std::asinh(std::tan(latitude_rad)) / std::numbers::pi) * 0.5 * tile_count;

    Aowis::Map::TerrainTileAddress address;
    address.zoom = zoom;
    address.x = quint32(std::floor(tile_x));
    address.y = quint32(std::floor(tile_y));
    return address;
}

void coordinateForGridSample(const Aowis::Map::TerrainTileAddress &address,
                             int column, int row,
                             double *latitude_deg, double *longitude_deg)
{
    const double tile_count = std::ldexp(1.0, address.zoom);
    const double tile_x = double(address.x) +
                          double(column) / double(Aowis::Map::TerrainTileCellCount);
    const double tile_y = double(address.y) +
                          double(row) / double(Aowis::Map::TerrainTileCellCount);

    *longitude_deg = tile_x / tile_count * 360.0 - 180.0;
    const double mercator_y = std::numbers::pi * (1.0 - 2.0 * tile_y / tile_count);
    *latitude_deg = std::atan(std::sinh(mercator_y)) * 180.0 / std::numbers::pi;
}

QString copernicusSourcePath(const Aowis::Map::TerrainData &terrain_data)
{
    return QDir(terrain_data.storagePaths().providers).filePath(
        QStringLiteral(
            "copernicus/glo30/Copernicus_DSM_COG_10_N50_00_E008_00_DEM.tif"));
}

bool writeSyntheticCopernicusSource(const QString &path, QString *error_message)
{
    const QFileInfo file_info(path);
    QDir directory;
    if (!directory.mkpath(file_info.absolutePath()))
    {
        *error_message = QStringLiteral("Failed to create synthetic Copernicus source directory");
        return false;
    }

    const QByteArray encoded_path = QFile::encodeName(path);
    TIFF *tiff = TIFFOpen(encoded_path.constData(), "w");
    if (tiff == nullptr)
    {
        *error_message = QStringLiteral("Failed to create synthetic Copernicus GeoTIFF");
        return false;
    }

    constexpr uint32_t Width = 128;
    constexpr uint32_t Height = 128;
    constexpr uint32_t TileWidth = 64;
    constexpr uint32_t TileHeight = 64;
    TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, Width);
    TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, Height);
    TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, 32);
    TIFFSetField(tiff, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_IEEEFP);
    TIFFSetField(tiff, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
    TIFFSetField(tiff, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
    TIFFSetField(tiff, TIFFTAG_PREDICTOR, PREDICTOR_FLOATINGPOINT);
    TIFFSetField(tiff, TIFFTAG_TILEWIDTH, TileWidth);
    TIFFSetField(tiff, TIFFTAG_TILELENGTH, TileHeight);

    std::vector<float> tile_data(TileWidth * TileHeight);
    bool success = true;
    for (uint32_t tile_y = 0; tile_y < Height; tile_y += TileHeight)
    {
        for (uint32_t tile_x = 0; tile_x < Width; tile_x += TileWidth)
        {
            for (uint32_t local_row = 0; local_row < TileHeight; ++local_row)
            {
                for (uint32_t local_column = 0; local_column < TileWidth; ++local_column)
                {
                    const uint32_t row = tile_y + local_row;
                    const uint32_t column = tile_x + local_column;
                    tile_data[local_row * TileWidth + local_column] =
                        1000.0f + float(row) * 2.0f + float(column) * 0.5f;
                }
            }

            if (TIFFWriteTile(tiff, tile_data.data(), tile_x, tile_y, 0, 0) < 0)
            {
                success = false;
                break;
            }
        }
        if (!success)
            break;
    }

    TIFFClose(tiff);
    if (!success)
    {
        *error_message = QStringLiteral("Failed to write synthetic Copernicus GeoTIFF samples");
        return false;
    }

    return true;
}
}

int main()
{
    using namespace Aowis::Map;

    if (!require(terrainProviderFetchStatusId(TerrainProviderFetchStatus::Ready) ==
                     QStringLiteral("ready") &&
                 terrainProviderFetchStatusId(TerrainProviderFetchStatus::NetworkError) ==
                     QStringLiteral("network-error"),
                 "Terrain provider status identifiers are not stable"))
    {
        return 1;
    }

    QTemporaryDir temporary_directory;
    if (!require(temporary_directory.isValid(),
                 "Failed to create terrain provider test directory"))
    {
        return 1;
    }

    TerrainData::Config config;
    config.enabled = true;
    config.remote_fetch_enabled = true;
    config.cache_directory = temporary_directory.path();
    TerrainData terrain_data(config);

    QString error_message;
    if (!require(terrain_data.initialize(&error_message),
                 "Failed to initialize terrain provider test subsystem"))
    {
        return 1;
    }

    const QString source_path = copernicusSourcePath(terrain_data);
    if (!require(writeSyntheticCopernicusSource(source_path, &error_message),
                 "Failed to prepare synthetic Copernicus provider source"))
    {
        return 1;
    }

    const TerrainTileAddress address = addressForCoordinate(50.5, 8.5, 14);
    TerrainTileLookupResult result =
        terrain_data.terrainTile(QStringLiteral("copernicus-glo30"), address);
    if (!require(result.status == TerrainTileLookupStatus::Ready,
                 "Copernicus provider did not normalize a provider-native GeoTIFF"))
    {
        std::cerr << result.error_message.toStdString() << '\n';
        return 1;
    }
    if (!require(result.origin == TerrainDataOrigin::Cache,
                 "Provider-native cached source did not report cache origin"))
    {
        return 1;
    }

    const std::optional<TerrainTile> tile = decodeTerrainTile(result.data, &error_message);
    if (!require(tile.has_value(), "Copernicus normalized tile could not be decoded"))
        return 1;
    if (!require(tile->dataset == QStringLiteral("copernicus-glo30") &&
                     tile->vertical_datum == TerrainVerticalDatum::Egm2008 &&
                     std::abs(tile->nominal_resolution_m - 30.0) < 0.001,
                 "Copernicus normalized tile metadata is incorrect"))
    {
        return 1;
    }

    double center_latitude_deg = 0.0;
    double center_longitude_deg = 0.0;
    coordinateForGridSample(address, 32, 32,
                            &center_latitude_deg, &center_longitude_deg);
    const double source_column = (center_longitude_deg - 8.0) * 128.0;
    const double source_row = (51.0 - center_latitude_deg) * 128.0;
    const double expected_elevation_m =
        1000.0 + source_row * 2.0 + source_column * 0.5;
    const int center_index = 32 * TerrainTileGridSize + 32;
    if (!require(std::abs(double(tile->elevations_m.at(center_index)) - expected_elevation_m) < 0.05,
                 "Copernicus GeoTIFF bilinear resampling is incorrect"))
    {
        return 1;
    }

    if (!require(QFileInfo::exists(terrain_data.normalizedTilePath(
                     QStringLiteral("copernicus-glo30"), address)),
                 "Provider result was not persisted to the normalized terrain cache"))
    {
        return 1;
    }

    const QByteArray first_data = result.data;
    result = terrain_data.terrainTile(QStringLiteral("copernicus-glo30"), address);
    if (!require(result.status == TerrainTileLookupStatus::Ready &&
                     result.origin == TerrainDataOrigin::Cache &&
                     result.data == first_data,
                 "Second Copernicus lookup did not use the normalized cache"))
    {
        return 1;
    }

    const QString normalized_path = terrain_data.normalizedTilePath(
        QStringLiteral("copernicus-glo30"), address);
    QFile corrupt_normalized_file(normalized_path);
    if (!require(corrupt_normalized_file.open(QIODevice::ReadWrite),
                 "Failed to open normalized Copernicus tile for read-through repair test"))
    {
        return 1;
    }
    if (!require(corrupt_normalized_file.seek(corrupt_normalized_file.size() - 1),
                 "Failed to seek normalized Copernicus tile for repair test"))
    {
        return 1;
    }
    QByteArray corrupt_byte = corrupt_normalized_file.read(1);
    if (!require(corrupt_byte.size() == 1,
                 "Failed to read normalized Copernicus repair-test byte"))
    {
        return 1;
    }
    corrupt_byte[0] = char(uchar(corrupt_byte.at(0)) ^ 0x5a);
    if (!require(corrupt_normalized_file.seek(corrupt_normalized_file.size() - 1) &&
                     corrupt_normalized_file.write(corrupt_byte) == 1,
                 "Failed to corrupt normalized Copernicus tile for repair test"))
    {
        return 1;
    }
    corrupt_normalized_file.close();

    result = terrain_data.terrainTile(QStringLiteral("copernicus-glo30"), address);
    if (!require(result.status == TerrainTileLookupStatus::Ready &&
                     result.origin == TerrainDataOrigin::Cache &&
                     result.data == first_data,
                 "Read-through cache did not repair a corrupt normalized tile from provider-native cache"))
    {
        std::cerr << result.error_message.toStdString() << '\n';
        return 1;
    }

    TerrainTileAddress unsupported_address = address;
    unsupported_address.zoom = 15;
    unsupported_address.x *= 2;
    unsupported_address.y *= 2;
    result = terrain_data.terrainTile(QStringLiteral("copernicus-glo30"), unsupported_address);
    if (!require(result.status == TerrainTileLookupStatus::TileUnavailable,
                 "Copernicus provider accepted an intentionally unsupported normalized zoom"))
    {
        return 1;
    }

    QTemporaryDir offline_directory;
    if (!require(offline_directory.isValid(),
                 "Failed to create offline terrain provider test directory"))
    {
        return 1;
    }

    TerrainData::Config offline_config;
    offline_config.enabled = true;
    offline_config.remote_fetch_enabled = false;
    offline_config.cache_directory = offline_directory.path();
    TerrainData offline_terrain_data(offline_config);
    if (!require(offline_terrain_data.initialize(&error_message),
                 "Failed to initialize offline terrain subsystem"))
    {
        return 1;
    }

    if (!require(writeSyntheticCopernicusSource(
                     copernicusSourcePath(offline_terrain_data), &error_message),
                 "Failed to prepare offline provider-native Copernicus source"))
    {
        return 1;
    }

    result = offline_terrain_data.terrainTile(QStringLiteral("copernicus-glo30"), address);
    if (!require(result.status == TerrainTileLookupStatus::Ready &&
                     result.origin == TerrainDataOrigin::Cache,
                 "Offline mode did not normalize an already cached provider-native COG"))
    {
        return 1;
    }

    QTemporaryDir empty_offline_directory;
    if (!require(empty_offline_directory.isValid(),
                 "Failed to create empty offline terrain provider test directory"))
    {
        return 1;
    }

    TerrainData::Config empty_offline_config;
    empty_offline_config.enabled = true;
    empty_offline_config.remote_fetch_enabled = false;
    empty_offline_config.cache_directory = empty_offline_directory.path();
    TerrainData empty_offline_terrain_data(empty_offline_config);
    if (!require(empty_offline_terrain_data.initialize(&error_message),
                 "Failed to initialize empty offline terrain subsystem"))
    {
        return 1;
    }

    result = empty_offline_terrain_data.terrainTile(
        QStringLiteral("copernicus-glo30"), address);
    if (!require(result.status == TerrainTileLookupStatus::TileUnavailable,
                 "Offline mode attempted a remote Copernicus fetch when source data was absent"))
    {
        return 1;
    }

    return 0;
}
