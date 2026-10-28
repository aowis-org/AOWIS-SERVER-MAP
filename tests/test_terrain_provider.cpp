#include <aowis/map/terrain_data.h>
#include <aowis/map/terrain_provider.h>
#include "../src/lib/copernicus_download_validation.h"
#include "../src/lib/copernicus_resume_state.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QTemporaryDir>

#include <tiffio.h>

#include <chrono>
#include <cmath>
#include <future>
#include <thread>
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

    // Native provider cache must never publish partial HTTP entities.
    if (!require(Internal::completeCopernicusResponse(200, 1024, 1024) &&
                 Internal::completeCopernicusResponse(200, 1024, -1) &&
                 !Internal::completeCopernicusResponse(206, 1024, 1024) &&
                 !Internal::completeCopernicusResponse(204, 0, 0) &&
                 !Internal::completeCopernicusResponse(200, 0, -1) &&
                 !Internal::completeCopernicusResponse(200, 512, 1024) &&
                 !Internal::completeCopernicusResponse(200, 1536, 1024),
                 "Copernicus complete-response validation is incorrect"))
        return 1;

    // A continuation is valid only for the same strong-ETag entity, with
    // contiguous, fully received bytes and a bounded declared total.
    using Internal::CopernicusResumeAction;
    using Internal::classifyCopernicusResumeResponse;
    const QByteArray strong_etag = "\"source-v1\"";
    if (!require(
            Internal::parseCopernicusContentRange("bytes 100-199/200").has_value() &&
            !Internal::parseCopernicusContentRange("bytes 100-199/*").has_value() &&
            !Internal::parseCopernicusContentRange("bytes 10-9/200").has_value() &&
            !Internal::parseCopernicusContentRange("bytes 0-9223372036854775808/9223372036854775809").has_value() &&
            classifyCopernicusResumeResponse(206, 100, 100, "bytes 100-199/200",
                strong_etag, strong_etag, 1024) ==
                CopernicusResumeAction::AppendVerifiedRange &&
            classifyCopernicusResumeResponse(206, 100, 100, "bytes 101-200/201",
                strong_etag, strong_etag, 1024) ==
                CopernicusResumeAction::RejectResponse &&
            classifyCopernicusResumeResponse(206, 100, 99, "bytes 100-199/200",
                strong_etag, strong_etag, 1024) ==
                CopernicusResumeAction::RejectResponse &&
            classifyCopernicusResumeResponse(206, 100, 100, "bytes 100-199/200",
                strong_etag, "\"source-v2\"", 1024) ==
                CopernicusResumeAction::RejectResponse &&
            classifyCopernicusResumeResponse(206, 100, 100, "bytes 100-199/200",
                "W/\"source-v1\"", "W/\"source-v1\"", 1024) ==
                CopernicusResumeAction::RejectResponse &&
            classifyCopernicusResumeResponse(206, 100, 100, "bytes 100-199/200",
                strong_etag, strong_etag, 150) ==
                CopernicusResumeAction::RejectResponse &&
            classifyCopernicusResumeResponse(200, 100, 200, "", "", "", 1024) ==
                CopernicusResumeAction::RestartWithFullEntity &&
            classifyCopernicusResumeResponse(416, 100, 0, "", "", "", 1024) ==
                CopernicusResumeAction::RejectResponse,
            "Copernicus HTTP Range resume validation is incorrect"))
        return 1;

    QTemporaryDir temporary_directory;
    if (!require(temporary_directory.isValid(),
                 "Failed to create terrain provider test directory"))
    {
        return 1;
    }

    // Partial source state survives process restarts only if both the source
    // bytes and strong-ETag metadata agree. Never accept stale/altered bytes.
    const QString partial_path = temporary_directory.filePath("source.partial");
    const QString metadata_path = temporary_directory.filePath("source.partial.meta");
    {
        QFile partial(partial_path);
        if (!require(partial.open(QIODevice::WriteOnly) &&
                     partial.write("0123456789", 10) == 10,
                     "Failed to create Copernicus partial test file"))
            return 1;
    }
    const Internal::CopernicusPartialState partial_state{10, 20, "\"source-v1\""};
    if (!require(Internal::saveCopernicusPartialState(metadata_path, partial_state, 1024) &&
                 Internal::loadCopernicusPartialState(partial_path, metadata_path, 1024)
                     .has_value() &&
                 !Internal::saveCopernicusPartialState(metadata_path,
                     {10, 20, "W/\"weak\""}, 1024),
                 "Copernicus partial metadata validation failed"))
        return 1;
    {
        QFile partial(partial_path);
        if (!require(partial.open(QIODevice::Append) && partial.write("x", 1) == 1,
                     "Failed to alter Copernicus partial test file"))
            return 1;
    }
    if (!require(!Internal::loadCopernicusPartialState(partial_path, metadata_path, 1024)
                     .has_value(),
                 "Mismatched Copernicus partial bytes were accepted"))
        return 1;

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

    // Hydrology requests native source-precision samples without creating
    // quantized Web-Mercator renderer tiles. Cell centres are at 8.125/50.875,
    // etc. The synthetic cached source has 128 pixels per axis.
    TerrainAnalysisGridRequest grid_request;
    grid_request.dataset = QStringLiteral("copernicus-glo30");
    grid_request.bounds = {8.0, 50.75, 8.25, 51.0};
    grid_request.columns = 4;
    grid_request.rows = 4;
    const TerrainAnalysisGridResult grid_result = terrain_data.analysisGrid(grid_request);
    if (!require(grid_result.status == TerrainProviderFetchStatus::Ready &&
                 grid_result.elevations_m.size() == 16 &&
                 grid_result.vertical_datum == TerrainVerticalDatum::Egm2008 &&
                 grid_result.origin == TerrainDataOrigin::Cache,
                 "Native DEM window status, dimensions or datum incorrect"))
    {
        std::cerr << grid_result.error_message.toStdString() << '\n';
        return 1;
    }
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            const float expected = 1000.0f + float(row * 8 + 4) * 2.0f +
                                   float(column * 8 + 4) * 0.5f;
            if (!require(grid_result.elevations_m.at(row * 4 + column) == expected,
                         "Native DEM window interpolated or misaligned source samples"))
                return 1;
        }
    }
    grid_request.columns = 513;
    if (!require(terrain_data.analysisGrid(grid_request).status ==
                     TerrainProviderFetchStatus::UnsupportedAddress,
                 "Oversized source DEM window must be rejected"))
        return 1;
    grid_request.columns = 4;

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

    TerrainAnalysisGridRequest offline_grid;
    offline_grid.dataset = QStringLiteral("copernicus-glo30");
    offline_grid.bounds = {8.0, 50.9, 8.1, 51.0};
    offline_grid.columns = 2;
    offline_grid.rows = 2;
    const TerrainAnalysisGridResult offline_result =
        empty_offline_terrain_data.analysisGrid(offline_grid);
    if (!require(offline_result.status == TerrainProviderFetchStatus::SourceUnavailable,
                 "Missing source DEM must not trigger downloads in offline mode"))
        return 1;

    result = empty_offline_terrain_data.terrainTile(
        QStringLiteral("copernicus-glo30"), address);
    if (!require(result.status == TerrainTileLookupStatus::TileUnavailable,
                 "Offline mode attempted a remote Copernicus fetch when source data was absent"))
    {
        return 1;
    }

    // A competing process can hold a provider-native source lock for minutes.
    // Cancellation must interrupt that wait promptly rather than occupy a
    // terrain worker until the normal source lock timeout expires.
    QTemporaryDir cancellation_directory;
    if (!require(cancellation_directory.isValid(),
                 "Failed to create source-lock cancellation test directory"))
        return 1;

    TerrainData::Config cancellation_config;
    cancellation_config.enabled = true;
    cancellation_config.remote_fetch_enabled = true;
    cancellation_config.cache_directory = cancellation_directory.path();
    TerrainData cancellation_terrain_data(cancellation_config);
    if (!require(cancellation_terrain_data.initialize(&error_message),
                 "Failed to initialize source-lock cancellation terrain provider"))
        return 1;

    const QString locked_source_path = copernicusSourcePath(cancellation_terrain_data);
    if (!require(QDir().mkpath(QFileInfo(locked_source_path).absolutePath()),
                 "Failed to create source-lock cancellation test directory"))
        return 1;
    QLockFile held_source_lock(locked_source_path + QStringLiteral(".lock"));
    if (!require(held_source_lock.tryLock(0),
                 "Failed to hold source lock for cancellation test"))
        return 1;

    std::future<TerrainTileLookupResult> blocked_request = std::async(
        std::launch::async, [&cancellation_terrain_data, address]()
        {
            return cancellation_terrain_data.terrainTile(
                QStringLiteral("copernicus-glo30"), address);
        });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    cancellation_terrain_data.cancelUpstreamDownloads();
    const bool canceled_promptly =
        blocked_request.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    // Unlock before joining in the failure case to prevent hanging this test.
    held_source_lock.unlock();
    if (!require(canceled_promptly,
                 "Copernicus source lock wait ignored upstream cancellation"))
        return 1;
    blocked_request.get();

    return 0;
}
