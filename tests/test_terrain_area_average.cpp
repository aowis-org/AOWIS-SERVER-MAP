// Normalized terrain below the provider's native zoom: every sample is the
// area average of the source over its sample box (normalization revision 2).
// The reference values are computed here independently, pixel by pixel, from
// the synthetic source, never through the provider's own accumulation.

#include <aowis/map/terrain_data.h>
#include <aowis/map/terrain_provider.h>
#include <aowis/map/terrain_tile.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <tiffio.h>

#include <algorithm>
#include <limits>
#include <cmath>
#include <functional>
#include <iostream>
#include <numbers>
#include <optional>
#include <vector>

namespace
{
using namespace Aowis::Map;

constexpr int SourcePixels = 1024; // pixels per degree on both axes
constexpr int SourceBlock = 256;

bool require(bool condition, const std::string &message)
{
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

// High-frequency relief: a point sampler spaced several pixels apart hits
// these ridges at effectively random phases, an area average does not.
double sourceElevation(int south_deg, int column, int row)
{
    // A separate source cell with a single sharp peak, a deep pit and no-data.
    if (south_deg == 52)
    {
        if (column == 512 && row == 512)
            return 4500.0;
        if (column == 513 && row == 512)
            return -200.0;
        if (column == 512 && row == 513)
            return std::numeric_limits<double>::quiet_NaN();
        return 1000.0;
    }
    return 1000.0 + double(south_deg) + 0.3 * row + 0.2 * column +
           150.0 * std::sin(column * 0.9) * std::cos(row * 0.7);
}

QString sourcePath(const TerrainData &terrain_data, int south_deg, int west_deg)
{
    return QDir(terrain_data.storagePaths().providers).filePath(
        QStringLiteral("copernicus/glo30/Copernicus_DSM_COG_10_N%1_00_E%2_00_DEM.tif")
            .arg(south_deg, 2, 10, QChar('0'))
            .arg(west_deg, 3, 10, QChar('0')));
}

bool writeSource(const QString &path, int south_deg)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    TIFF *tiff = TIFFOpen(QFile::encodeName(path).constData(), "w");
    if (tiff == nullptr)
        return false;
    TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, uint32_t(SourcePixels));
    TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, uint32_t(SourcePixels));
    TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, 32);
    TIFFSetField(tiff, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_IEEEFP);
    TIFFSetField(tiff, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
    TIFFSetField(tiff, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
    TIFFSetField(tiff, TIFFTAG_PREDICTOR, PREDICTOR_FLOATINGPOINT);
    TIFFSetField(tiff, TIFFTAG_TILEWIDTH, uint32_t(SourceBlock));
    TIFFSetField(tiff, TIFFTAG_TILELENGTH, uint32_t(SourceBlock));
    std::vector<float> block(std::size_t(SourceBlock) * SourceBlock);
    bool success = true;
    for (int tile_y = 0; tile_y < SourcePixels && success; tile_y += SourceBlock)
    {
        for (int tile_x = 0; tile_x < SourcePixels && success; tile_x += SourceBlock)
        {
            for (int row = 0; row < SourceBlock; ++row)
                for (int column = 0; column < SourceBlock; ++column)
                    block[std::size_t(row) * SourceBlock + std::size_t(column)] =
                        float(sourceElevation(south_deg, tile_x + column, tile_y + row));
            success = TIFFWriteTile(tiff, block.data(), uint32_t(tile_x), uint32_t(tile_y), 0, 0) >= 0;
        }
    }
    TIFFClose(tiff);
    return success;
}

double longitudeAt(const TerrainTileAddress &address, double grid_x)
{
    return (double(address.x) + grid_x / double(TerrainTileCellCount)) /
           std::ldexp(1.0, address.zoom) * 360.0 - 180.0;
}

double latitudeAt(const TerrainTileAddress &address, double grid_y)
{
    const double y = (double(address.y) + grid_y / double(TerrainTileCellCount)) /
                     std::ldexp(1.0, address.zoom);
    return std::atan(std::sinh(std::numbers::pi * (1.0 - 2.0 * y))) * 180.0 / std::numbers::pi;
}

TerrainTileAddress addressFor(double latitude_deg, double longitude_deg, int zoom)
{
    const double count = std::ldexp(1.0, zoom);
    const double latitude_rad = latitude_deg * std::numbers::pi / 180.0;
    TerrainTileAddress address;
    address.zoom = zoom;
    address.x = quint32(std::floor((longitude_deg + 180.0) / 360.0 * count));
    address.y = quint32(std::floor(
        (1.0 - std::asinh(std::tan(latitude_rad)) / std::numbers::pi) * 0.5 * count));
    return address;
}

// Exact area average over the box centred on grid sample (row, column): every
// pixel footprint (centre +- half a pixel, RasterPixelIsPoint) weighted by its
// overlap area with the box.
double referenceAreaAverage(const TerrainTileAddress &address, int south_deg, int west_deg,
                            int row, int column)
{
    const double west = longitudeAt(address, column - 0.5);
    const double east = longitudeAt(address, column + 0.5);
    const double north = latitudeAt(address, row - 0.5);
    const double south = latitudeAt(address, row + 0.5);
    const double cell_north = double(south_deg + 1);
    double sum = 0.0;
    double weight = 0.0;
    for (int pixel_row = 0; pixel_row < SourcePixels; ++pixel_row)
    {
        const double top = cell_north - (pixel_row - 0.5) / SourcePixels;
        const double bottom = cell_north - (pixel_row + 0.5) / SourcePixels;
        const double height = std::min(top, north) - std::max(bottom, south);
        if (height <= 0.0)
            continue;
        for (int pixel_column = 0; pixel_column < SourcePixels; ++pixel_column)
        {
            const double left = west_deg + (pixel_column - 0.5) / SourcePixels;
            const double right = west_deg + (pixel_column + 0.5) / SourcePixels;
            const double width = std::min(right, east) - std::max(left, west);
            if (width <= 0.0)
                continue;
            sum += width * height * sourceElevation(south_deg, pixel_column, pixel_row);
            weight += width * height;
        }
    }
    return sum / weight;
}

// The provider's point sample: bilinear between the four surrounding pixels.
double referenceBilinear(int south_deg, int west_deg, double latitude_deg, double longitude_deg)
{
    const double x = (longitude_deg - west_deg) * SourcePixels;
    const double y = (south_deg + 1 - latitude_deg) * SourcePixels;
    const int column = int(std::floor(x));
    const int row = int(std::floor(y));
    const double fx = x - column;
    const double fy = y - row;
    return (1 - fx) * (1 - fy) * sourceElevation(south_deg, column, row) +
           fx * (1 - fy) * sourceElevation(south_deg, column + 1, row) +
           (1 - fx) * fy * sourceElevation(south_deg, column, row + 1) +
           fx * fy * sourceElevation(south_deg, column + 1, row + 1);
}

// Independent brute-force source pixel footprint scan, without using the
// provider's overlap tables or its decoded heightfield.
TerrainSourceStatistics referenceSourceStatistics(const TerrainTileAddress &address,
                                                 int south_deg, int west_deg,
                                                 double halo)
{
    const double west = longitudeAt(address, -halo);
    const double east = longitudeAt(address, TerrainTileCellCount + halo);
    const double north = latitudeAt(address, -halo);
    const double south = latitudeAt(address, TerrainTileCellCount + halo);
    TerrainSourceStatistics result;
    for (int row = 0; row < SourcePixels; ++row)
    {
        const double top = south_deg + 1 - (row - 0.5) / SourcePixels;
        const double bottom = south_deg + 1 - (row + 0.5) / SourcePixels;
        if (std::min(top, north) <= std::max(bottom, south))
            continue;
        for (int column = 0; column < SourcePixels; ++column)
        {
            const double left = west_deg + (column - 0.5) / SourcePixels;
            const double right = west_deg + (column + 0.5) / SourcePixels;
            if (std::min(right, east) <= std::max(left, west))
                continue;
            const float value = float(sourceElevation(south_deg, column, row));
            if (!std::isfinite(value))
            {
                ++result.no_data_pixel_count;
                continue;
            }
            if (result.valid_pixel_count++ == 0)
                result.minimum_elevation_m = result.maximum_elevation_m = value;
            else
            {
                result.minimum_elevation_m = std::min(result.minimum_elevation_m, value);
                result.maximum_elevation_m = std::max(result.maximum_elevation_m, value);
            }
        }
    }
    return result;
}

bool matchesSourceStatistics(const std::optional<TerrainSourceStatistics> &actual,
                             const TerrainSourceStatistics &expected)
{
    return actual && actual->minimum_elevation_m == expected.minimum_elevation_m &&
           actual->maximum_elevation_m == expected.maximum_elevation_m &&
           actual->valid_pixel_count == expected.valid_pixel_count &&
           actual->no_data_pixel_count == expected.no_data_pixel_count &&
           actual->missing_source_cells == expected.missing_source_cells;
}

// Independent native-centre reference, using the decoded grid (the provider
// measures before quantization, so allow the v2 quantization error).
TerrainGeometricError referenceGeometricError(const TerrainTile &tile, int south_deg, int west_deg)
{
    TerrainGeometricError result;
    double squared = 0.0;
    const TerrainTileAddress &address = tile.address;
    const double north = latitudeAt(address, 0), south = latitudeAt(address, TerrainTileCellCount);
    const double west = longitudeAt(address, 0), east = longitudeAt(address, TerrainTileCellCount);
    for (int row = 0; row < SourcePixels; ++row)
    {
        const double latitude = south_deg + 1 - double(row) / SourcePixels;
        if (latitude < south || latitude > north)
            continue;
        const double mercator = std::log(std::tan(std::numbers::pi / 4.0 +
                                                  latitude * std::numbers::pi / 360.0));
        const double y = std::clamp(((1.0 - mercator / std::numbers::pi) * 0.5 *
            std::ldexp(1.0, address.zoom) - address.y) * TerrainTileCellCount,
            0.0, double(TerrainTileCellCount));
        for (int column = 0; column < SourcePixels; ++column)
        {
            const double longitude = west_deg + double(column) / SourcePixels;
            if (longitude < west || longitude > east)
                continue;
            const float source = float(sourceElevation(south_deg, column, row));
            if (!std::isfinite(source))
            {
                ++result.no_data_pixel_count;
                continue;
            }
            const double x = std::clamp((longitude - west) / (east - west) * TerrainTileCellCount,
                                         0.0, double(TerrainTileCellCount));
            const int i = std::min(int(x), TerrainTileCellCount - 1);
            const int j = std::min(int(y), TerrainTileCellCount - 1);
            const double u = x - i, v = y - j;
            const int base = j * TerrainTileGridSize + i;
            const double ne = tile.elevations_m.at(base + 1);
            const double sw = tile.elevations_m.at(base + TerrainTileGridSize);
            double mesh;
            if (u + v <= 1.0)
            {
                const double nw = tile.elevations_m.at(base);
                mesh = nw + u * (ne - nw) + v * (sw - nw);
            }
            else
            {
                const double se = tile.elevations_m.at(base + TerrainTileGridSize + 1);
                mesh = se + (1.0 - u) * (sw - se) + (1.0 - v) * (ne - se);
            }
            if (!std::isfinite(mesh))
            {
                ++result.missing_surface_pixel_count;
                continue;
            }
            const double residual = source - mesh;
            result.maximum_underestimation_m = std::max(result.maximum_underestimation_m, residual);
            result.maximum_overestimation_m = std::max(result.maximum_overestimation_m, -residual);
            squared += residual * residual;
            ++result.compared_pixel_count;
        }
    }
    if (result.compared_pixel_count)
        result.rms_geometric_error_m = std::sqrt(squared / result.compared_pixel_count);
    return result;
}

bool matchesGeometricError(const std::optional<TerrainGeometricError> &actual,
                           const TerrainGeometricError &expected)
{
    constexpr double tolerance_m = 0.05; // decoded 16-bit grid vs original floats
    return actual && actual->compared_pixel_count == expected.compared_pixel_count &&
           actual->no_data_pixel_count == expected.no_data_pixel_count &&
           actual->missing_surface_pixel_count == expected.missing_surface_pixel_count &&
           actual->missing_source_cells == expected.missing_source_cells &&
           std::abs(actual->maximum_underestimation_m - expected.maximum_underestimation_m) < tolerance_m &&
           std::abs(actual->maximum_overestimation_m - expected.maximum_overestimation_m) < tolerance_m &&
           std::abs(actual->rms_geometric_error_m - expected.rms_geometric_error_m) < tolerance_m;
}

std::optional<TerrainTile> loadTile(TerrainData &terrain_data, const TerrainTileAddress &address,
                                    std::optional<TerrainSourceStatistics> *statistics = nullptr,
                                    std::optional<TerrainGeometricError> *error = nullptr)
{
    const TerrainTileLookupResult result =
        terrain_data.terrainTile(QStringLiteral("copernicus-glo30"), address);
    if (result.status != TerrainTileLookupStatus::Ready)
    {
        std::cerr << "tile " << address.zoom << '/' << address.x << '/' << address.y
                  << " not ready: " << result.error_message.toStdString() << '\n';
        return std::nullopt;
    }
    if (statistics != nullptr)
        *statistics = result.source_statistics;
    if (error != nullptr)
        *error = result.geometric_error;
    return decodeTerrainTile(result.data);
}

bool insideCell(const TerrainTileAddress &address, int south_deg, int west_deg)
{
    // The whole box window, half a spacing beyond the tile, must lie in the
    // single synthetic cell: neighbour cells do not exist in this test.
    return longitudeAt(address, -0.5) > west_deg + 1.0 / SourcePixels &&
           longitudeAt(address, TerrainTileCellCount + 0.5) < west_deg + 1 - 1.0 / SourcePixels &&
           latitudeAt(address, -0.5) < south_deg + 1 - 1.0 / SourcePixels &&
           latitudeAt(address, TerrainTileCellCount + 0.5) > south_deg + 1.0 / SourcePixels;
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary_directory;
    if (!require(temporary_directory.isValid(), "temporary directory"))
        return 1;
    TerrainData::Config config;
    config.enabled = true;
    config.remote_fetch_enabled = false; // only the synthetic cells exist
    config.cache_directory = temporary_directory.path();
    TerrainData terrain_data(config);
    QString error_message;
    if (!require(terrain_data.initialize(&error_message), "terrain initialization") ||
        !require(writeSource(sourcePath(terrain_data, 50, 8), 50), "synthetic N50 source") ||
        !require(writeSource(sourcePath(terrain_data, 70, 8), 70), "synthetic N70 source") ||
        !require(writeSource(sourcePath(terrain_data, 52, 8), 52), "synthetic N52 source"))
        return 1;

    // Zoom 10 at 50.5 N: boxes are ~5.6 x 3.6 source pixels.
    const TerrainTileAddress centre = addressFor(50.5, 8.3, 10);
    TerrainTileAddress east_neighbour = centre;
    east_neighbour.x += 1;
    if (!require(insideCell(centre, 50, 8) && insideCell(east_neighbour, 50, 8),
                 "test tiles must lie inside the synthetic cell"))
        return 1;
    std::optional<TerrainSourceStatistics> centre_statistics;
    std::optional<TerrainGeometricError> centre_error;
    const std::optional<TerrainTile> tile = loadTile(terrain_data, centre, &centre_statistics, &centre_error);
    const std::optional<TerrainTile> neighbour = loadTile(terrain_data, east_neighbour);
    if (!require(tile.has_value() && neighbour.has_value(), "zoom 10 tiles decode"))
        return 1;

    if (!require(matchesSourceStatistics(centre_statistics,
                     referenceSourceStatistics(centre, 50, 8, 0.5)),
                 "coarse source statistics differ from independent native scan"))
        return 1;
    if (!require(matchesGeometricError(centre_error, referenceGeometricError(*tile, 50, 8)),
                 "coarse geometric error differs from independent reference"))
        return 1;
    const TerrainTileLookupResult cached = terrain_data.terrainTile(
        QStringLiteral("copernicus-glo30"), centre);
    if (!require(cached.status == TerrainTileLookupStatus::Ready &&
                     matchesSourceStatistics(cached.source_statistics, *centre_statistics) &&
                     matchesGeometricError(cached.geometric_error, *centre_error) &&
                     decodeTerrainTile(cached.data),
                 "v2 cache hit must preserve native statistics and error"))
        return 1;

    // Quantization: 16-bit codes over the tile's elevation range (< 0.01 m).
    double worst = 0.0;
    double largest_point_difference = 0.0;
    for (int row = 0; row < TerrainTileGridSize; ++row)
    {
        for (int column = 0; column < TerrainTileGridSize; ++column)
        {
            const double expected = referenceAreaAverage(centre, 50, 8, row, column);
            const double actual = tile->elevations_m.at(row * TerrainTileGridSize + column);
            worst = std::max(worst, std::abs(actual - expected));
            largest_point_difference = std::max(largest_point_difference, std::abs(
                actual - referenceBilinear(50, 8, latitudeAt(centre, row), longitudeAt(centre, column))));
        }
    }
    if (!require(worst < 0.02, "area average differs from the exact reference by " +
                                   std::to_string(worst) + " m"))
        return 1;
    // The relief swings +-150 m between neighbouring pixels: point samples
    // would differ from the box averages by tens of metres somewhere.
    if (!require(largest_point_difference > 20.0, "tile still holds point samples"))
        return 1;

    // Adjacent tiles average identical boxes for their shared edge samples.
    for (int row = 0; row < TerrainTileGridSize; ++row)
    {
        const float west_side = tile->elevations_m.at(row * TerrainTileGridSize + TerrainTileCellCount);
        const float east_side = neighbour->elevations_m.at(row * TerrainTileGridSize);
        if (!require(std::abs(west_side - east_side) < 0.02,
                     "shared edge sample differs between adjacent tiles"))
            return 1;
    }

    // Zoom 14 (native) keeps bilinear point samples.
    const TerrainTileAddress native = addressFor(50.5, 8.5, 14);
    std::optional<TerrainSourceStatistics> native_statistics;
    std::optional<TerrainGeometricError> native_error;
    const std::optional<TerrainTile> native_tile = loadTile(terrain_data, native, &native_statistics, &native_error);
    if (!require(native_tile.has_value(), "zoom 14 tile decodes"))
        return 1;
    if (!require(matchesSourceStatistics(native_statistics,
                     referenceSourceStatistics(native, 50, 8, 0.0)),
                 "native zoom source statistics differ from independent scan"))
        return 1;
    if (!require(matchesGeometricError(native_error, referenceGeometricError(*native_tile, 50, 8)),
                 "native geometric error differs from independent reference"))
        return 1;
    for (int index : {0, 32 * TerrainTileGridSize + 32, TerrainTileSampleCount - 1})
    {
        const int row = index / TerrainTileGridSize;
        const int column = index % TerrainTileGridSize;
        const double expected =
            referenceBilinear(50, 8, latitudeAt(native, row), longitudeAt(native, column));
        if (!require(std::abs(native_tile->elevations_m.at(index) - expected) < 0.02,
                     "native zoom no longer samples bilinearly"))
            return 1;
    }

    // Zoom 13 at 70.5 N: boxes (0.00069 x 0.00023 deg) are smaller than a
    // source pixel (1/1024 deg), so samples fall back to bilinear points.
    const TerrainTileAddress polar = addressFor(70.5, 8.5, 13);
    std::optional<TerrainSourceStatistics> polar_statistics;
    const std::optional<TerrainTile> polar_tile = loadTile(terrain_data, polar, &polar_statistics);
    if (!require(insideCell(polar, 70, 8) && polar_tile.has_value(), "zoom 13 polar tile decodes"))
        return 1;
    if (!require(matchesSourceStatistics(polar_statistics,
                     referenceSourceStatistics(polar, 70, 8, 0.5)),
                 "sub-pixel box source statistics differ from independent scan"))
        return 1;
    for (int index : {0, 20 * TerrainTileGridSize + 41, TerrainTileSampleCount - 1})
    {
        const int row = index / TerrainTileGridSize;
        const int column = index % TerrainTileGridSize;
        const double expected =
            referenceBilinear(70, 8, latitudeAt(polar, row), longitudeAt(polar, column));
        if (!require(std::abs(polar_tile->elevations_m.at(index) - expected) < 0.02,
                     "sub-pixel boxes did not fall back to bilinear samples"))
            return 1;
    }

    // The isolated peak and pit must survive in the statistics, not in the
    // averaged render grid. Missing pixels are counted separately.
    const TerrainTileAddress peak = addressFor(52.5, 8.5, 10);
    std::optional<TerrainSourceStatistics> peak_statistics;
    std::optional<TerrainGeometricError> peak_error;
    const std::optional<TerrainTile> peak_tile = loadTile(terrain_data, peak, &peak_statistics, &peak_error);
    if (!require(insideCell(peak, 52, 8) && peak_tile &&
                     matchesSourceStatistics(peak_statistics,
                         referenceSourceStatistics(peak, 52, 8, 0.5)) &&
                     peak_statistics->minimum_elevation_m == -200.0f &&
                     peak_statistics->maximum_elevation_m == 4500.0f &&
                     peak_statistics->no_data_pixel_count == 1 &&
                     *std::max_element(peak_tile->elevations_m.begin(),
                                       peak_tile->elevations_m.end()) < 4500.0f,
                 "source peak, pit or no-data was lost in the statistics"))
        return 1;

    if (!require(matchesGeometricError(peak_error, referenceGeometricError(*peak_tile, 52, 8)) &&
                     peak_error->maximum_underestimation_m > 3000.0 &&
                     peak_error->maximum_overestimation_m > 1000.0 &&
                     peak_error->no_data_pixel_count == 1,
                 "geometric error failed to capture peak/pit or missing native pixel"))
        return 1;

    // A tile straddling the eastern boundary can still normalize its available
    // source. The absent neighbouring source must not go unreported.
    const TerrainTileAddress partial = addressFor(50.5, 8.99, 10);
    std::optional<TerrainSourceStatistics> partial_statistics;
    std::optional<TerrainGeometricError> partial_error;
    if (!require(loadTile(terrain_data, partial, &partial_statistics, &partial_error).has_value() &&
                     partial_error && partial_error->missing_source_cells > 0 &&
                     partial_statistics && partial_statistics->valid_pixel_count > 0 &&
                     partial_statistics->missing_source_cells > 0,
                 "missing source cell was not reported"))
        return 1;

    // Tiles of this normalizer live in their own cache namespace.
    if (!require(terrain_data.normalizedTilePath(QStringLiteral("copernicus-glo30"), centre)
                     .contains(QStringLiteral("/v2/r2/copernicus-glo30/10/")),
                 "normalized cache path lacks the normalization revision"))
        return 1;

    // A normalized-format upgrade must not invalidate provider-native files.
    // Exercise a cold v2 cache with remote acquisition enabled, but with a
    // loopback-only source URL so a regression cannot contact Copernicus.
    {
        QTemporaryDir upgrade_directory;
        TerrainData::Config upgrade_config = config;
        upgrade_config.cache_directory = upgrade_directory.path();
        upgrade_config.remote_fetch_enabled = true;
        upgrade_config.integration_source_url = QUrl(QStringLiteral("http://127.0.0.1:1/source.tif"));
        upgrade_config.connect_timeout_seconds = 1;
        upgrade_config.inactivity_timeout_seconds = 1;
        upgrade_config.download_deadline_seconds = 2;
        TerrainData upgraded(upgrade_config);
        if (!require(upgrade_directory.isValid() && upgraded.initialize(&error_message),
                     "initialize cold v2 cache"))
            return 1;
        const QString original = sourcePath(terrain_data, 50, 8);
        const QString retained = sourcePath(upgraded, 50, 8);
        if (!require(QDir().mkpath(QFileInfo(retained).absolutePath()) &&
                         QFile::copy(original, retained), "retain native source across upgrade"))
            return 1;
        const QString dataset = QStringLiteral("copernicus-glo30");
        for (const TerrainTileAddress &address : {centre, native})
        {
            const QString path = upgraded.normalizedTilePath(dataset, address);
            if (!require(!QFileInfo::exists(path) &&
                             !upgraded.tileNeedsRemoteAcquisition(dataset, address),
                         "cold v2 tile with cached native source must not need remote acquisition"))
                return 1;
            if (!require(upgraded.acquireTileSources(dataset, address).status ==
                             TerrainProviderFetchStatus::Ready,
                         "cached native source acquisition must succeed locally"))
                return 1;
            const TerrainTileLookupResult rebuilt = upgraded.terrainTile(dataset, address);
            if (!require(rebuilt.status == TerrainTileLookupStatus::Ready &&
                             rebuilt.origin == TerrainDataOrigin::Cache &&
                             rebuilt.source_statistics && rebuilt.geometric_error &&
                             QFileInfo::exists(path),
                         "v2 terrain and LOD metadata must rebuild from cached native data"))
                return 1;
        }
        QFile before(original), after(retained);
        if (!require(before.open(QIODevice::ReadOnly) && after.open(QIODevice::ReadOnly) &&
                         before.readAll() == after.readAll(),
                     "v2 rebuilding changed the retained provider-native file"))
            return 1;
    }

    std::cout << "area average: worst " << worst << " m, largest point-sample difference "
              << largest_point_difference << " m\n";
    return 0;
}
