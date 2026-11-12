#include <aowis/map/raster_transfer.h>
#include <aowis/map/terrain_tile.h>
#include <aowis/map/terrain_transfer.h>

#include <QByteArray>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QCryptographicHash>
#include <QMap>
#include <QStringList>
#include <QVector>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <png.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <optional>
#include <vector>

namespace
{
struct Totals
{
    quint64 examined = 0;
    quint64 valid = 0;
    quint64 invalid = 0;
    quint64 original_bytes = 0;
    quint64 transfer_bytes = 0;
    quint64 optimized = 0;
    quint64 codec_nanoseconds = 0;
};

bool readFile(const QString &path, QByteArray *data, qint64 maximum)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 ||
        file.size() > maximum) return false;
    *data = file.readAll();
    return data->size() == file.size();
}

bool isRasterFile(const QString &path)
{
    const QString lower = path.toLower();
    return lower.endsWith(QStringLiteral(".png")) ||
           lower.endsWith(QStringLiteral(".jpg")) ||
           lower.endsWith(QStringLiteral(".jpeg")) ||
           lower.endsWith(QStringLiteral(".webp"));
}

bool isRasterPayload(const QByteArray &data)
{
    return data.startsWith("\x89PNG\r\n\x1a\n") ||
           data.startsWith("\xff\xd8\xff") ||
           (data.size() >= 12 && data.startsWith("RIFF") && data.mid(8, 4) == "WEBP");
}

// Traverse directories in filename order. This makes the adaptive selector's
// exploration choices repeatable for a given cache and --max-files limit.
void visitFiles(const QString &root, quint64 limit,
                const std::function<bool(const QString &)> &accept,
                const std::function<void(const QString &)> &consume)
{
    quint64 found = 0;
    const std::function<void(const QString &)> visit = [&](const QString &directory)
    {
        if (found >= limit) return;
        const QFileInfoList entries = QDir(directory).entryInfoList(
            QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks,
            QDir::Name | QDir::IgnoreCase);
        for (const QFileInfo &entry : entries)
        {
            if (found >= limit) break;
            if (entry.isDir()) visit(entry.absoluteFilePath());
            else if (entry.isFile() && accept(entry.fileName()))
            {
                ++found;
                consume(entry.absoluteFilePath());
            }
        }
    };
    visit(root);
}

// Region filtering uses the cached tile's XYZ bounds, not its file order.
// Bounding-box intersection at low zoom matters: a coarse tile may cover the
// town even when the tile centre is hundreds of kilometres away.
struct GeographicRegion
{
    QString name;
    double latitude = 0.0;
    double longitude = 0.0;
    double radius_km = 25.0;
    int minimum_zoom = 8;
    int maximum_zoom = 16;
};

struct CachedTile
{
    QString path;
    QString source;
    int zoom = 0;
    int x = 0;
    int y = 0;
    QByteArray shuffle_key;
};

struct RegionCoverage
{
    quint64 available = 0;
    quint64 selected = 0;
    QMap<int, quint64> available_by_zoom;
    QMap<int, quint64> selected_by_zoom;
    QMap<QString, quint64> selected_by_source;
};

constexpr double Pi = 3.14159265358979323846;

int tileXAtLongitude(double longitude, int zoom)
{
    const double scale = std::ldexp(1.0, zoom);
    return std::clamp(int(std::floor((longitude + 180.0) / 360.0 * scale)),
                      0, int(scale) - 1);
}

int tileYAtLatitude(double latitude, int zoom)
{
    const double clamped = std::clamp(latitude, -85.05112878, 85.05112878);
    const double radians = clamped * Pi / 180.0;
    const double scale = std::ldexp(1.0, zoom);
    return std::clamp(int(std::floor((1.0 - std::asinh(std::tan(radians)) / Pi) *
                                     0.5 * scale)), 0, int(scale) - 1);
}

double latitudeAtTileY(int y, int zoom)
{
    const double scale = std::ldexp(1.0, zoom);
    return std::atan(std::sinh(Pi * (1.0 - 2.0 * double(y) / scale))) *
           180.0 / Pi;
}

bool intersectsRegion(int zoom, int x, int y, const GeographicRegion &region)
{
    const double scale = std::ldexp(1.0, zoom);
    const double west = double(x) / scale * 360.0 - 180.0;
    const double east = double(x + 1) / scale * 360.0 - 180.0;
    const double north = latitudeAtTileY(y, zoom);
    const double south = latitudeAtTileY(y + 1, zoom);
    const double nearest_lat = std::clamp(region.latitude, south, north);
    const double nearest_lon = std::clamp(region.longitude, west, east);
    const double north_south_km = (nearest_lat - region.latitude) * 111.32;
    const double east_west_km = (nearest_lon - region.longitude) * 111.32 *
        std::cos(region.latitude * Pi / 180.0);
    return north_south_km * north_south_km + east_west_km * east_west_km <=
           region.radius_km * region.radius_km;
}

std::optional<CachedTile> cachedTileAddress(const QString &root,
                                           const QString &path, bool raster)
{
    const QStringList parts = QDir::fromNativeSeparators(
        QDir(root).relativeFilePath(path)).split(QLatin1Char('/'));
    // Raster: provider/z/x/y.png, also provider/variants/hash/z/x/y.png.
    // Terrain: v1/r2/dataset/z/x/y.aowterrain.
    if (parts.size() < (raster ? 4 : 6)) return std::nullopt;
    bool zoom_ok = false;
    bool x_ok = false;
    bool y_ok = false;
    const int zoom = parts.at(parts.size() - 3).toInt(&zoom_ok);
    const int x = parts.at(parts.size() - 2).toInt(&x_ok);
    const int y = QFileInfo(parts.last()).completeBaseName().toInt(&y_ok);
    if (!zoom_ok || !x_ok || !y_ok || zoom < 0 || zoom > 22 ||
        x < 0 || y < 0 || x >= (1 << zoom) || y >= (1 << zoom))
        return std::nullopt;
    CachedTile tile;
    tile.path = path;
    tile.zoom = zoom;
    tile.x = x;
    tile.y = y;
    tile.source = raster ? parts.first() : parts.at(parts.size() - 4);
    tile.shuffle_key = QCryptographicHash::hash(
        QDir(root).relativeFilePath(path).toUtf8(), QCryptographicHash::Sha256);
    return tile;
}

void visitRegionFiles(const QString &root, quint64 limit, bool raster,
                      const GeographicRegion &region,
                      const std::function<void(const QString &)> &consume,
                      RegionCoverage *coverage)
{
    // Enumerate *all* cached tiles in this area before sampling. The previous
    // first-N directory traversal disproportionately selected the first
    // provider/zoom/geographic region, making results unrepresentative.
    QMap<QString, QVector<CachedTile>> groups;
    QDirIterator iterator(root, QDir::Files | QDir::NoSymLinks,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext())
    {
        const QString path = iterator.next();
        if (raster ? !isRasterFile(path) :
            !path.endsWith(QStringLiteral(".aowterrain"), Qt::CaseInsensitive))
            continue;
        const std::optional<CachedTile> tile = cachedTileAddress(root, path, raster);
        if (!tile.has_value() || tile->zoom < region.minimum_zoom ||
            tile->zoom > region.maximum_zoom ||
            !intersectsRegion(tile->zoom, tile->x, tile->y, region))
            continue;
        ++coverage->available;
        ++coverage->available_by_zoom[tile->zoom];
        groups[QStringLiteral("%1/%2").arg(tile->source).arg(tile->zoom)].append(*tile);
    }
    for (QMap<QString, QVector<CachedTile>>::iterator it = groups.begin();
         it != groups.end(); ++it)
    {
        std::sort(it.value().begin(), it.value().end(),
                  [](const CachedTile &a, const CachedTile &b)
                  {
                      return a.shuffle_key < b.shuffle_key ||
                             (a.shuffle_key == b.shuffle_key && a.path < b.path);
                  });
    }
    // Deterministic round-robin among provider/zoom strata, seeded by a
    // stable content-independent hash of each filename. Re-running adaptive
    // and exhaustive JPEG therefore benchmarks *the exact same* input files.
    qsizetype round = 0;
    while (coverage->selected < limit)
    {
        bool any = false;
        for (QMap<QString, QVector<CachedTile>>::const_iterator it = groups.cbegin();
             it != groups.cend() && coverage->selected < limit; ++it)
        {
            if (round >= it.value().size()) continue;
            const CachedTile &tile = it.value().at(round);
            any = true;
            ++coverage->selected;
            ++coverage->selected_by_zoom[tile.zoom];
            ++coverage->selected_by_source[tile.source];
            consume(tile.path);
        }
        if (!any) break;
        ++round;
    }
}

QJsonObject geographicCoverageJson(const RegionCoverage &coverage)
{
    QJsonObject result;
    result.insert(QStringLiteral("cached_candidates"), qint64(coverage.available));
    result.insert(QStringLiteral("sampled_tiles"), qint64(coverage.selected));
    QJsonObject available_by_zoom;
    QJsonObject sampled_by_zoom;
    QJsonObject sampled_by_source;
    for (QMap<int, quint64>::const_iterator it = coverage.available_by_zoom.cbegin();
         it != coverage.available_by_zoom.cend(); ++it)
        available_by_zoom.insert(QString::number(it.key()), qint64(it.value()));
    for (QMap<int, quint64>::const_iterator it = coverage.selected_by_zoom.cbegin();
         it != coverage.selected_by_zoom.cend(); ++it)
        sampled_by_zoom.insert(QString::number(it.key()), qint64(it.value()));
    for (QMap<QString, quint64>::const_iterator it = coverage.selected_by_source.cbegin();
         it != coverage.selected_by_source.cend(); ++it)
        sampled_by_source.insert(it.key(), qint64(it.value()));
    result.insert(QStringLiteral("cached_by_zoom"), available_by_zoom);
    result.insert(QStringLiteral("sampled_by_zoom"), sampled_by_zoom);
    result.insert(QStringLiteral("sampled_by_source"), sampled_by_source);
    return result;
}

QJsonObject jsonTotals(const Totals &totals)
{
    const double saved = totals.original_bytes == 0 ? 0.0 :
        100.0 * double(totals.original_bytes - totals.transfer_bytes) /
        double(totals.original_bytes);
    QJsonObject result;
    result.insert(QStringLiteral("examined_files"), qint64(totals.examined));
    result.insert(QStringLiteral("valid_tiles"), qint64(totals.valid));
    result.insert(QStringLiteral("invalid_or_unreadable"), qint64(totals.invalid));
    result.insert(QStringLiteral("original_payload_bytes"), qint64(totals.original_bytes));
    result.insert(QStringLiteral("selected_payload_bytes"), qint64(totals.transfer_bytes));
    result.insert(QStringLiteral("saved_percent"), saved);
    result.insert(QStringLiteral("optimized_tiles"), qint64(totals.optimized));
    result.insert(QStringLiteral("codec_time_ms"), double(totals.codec_nanoseconds) / 1e6);
    return result;
}

QJsonObject benchmarkRaster(const QString &root, int quality, quint64 limit, bool adaptive,
                            const std::optional<GeographicRegion> &region = std::nullopt)
{
    Totals totals;
    Aowis::Map::RasterTransferOptimizer optimizer;
    RegionCoverage coverage;
    const std::function<void(const QString &)> consume = [&](const QString &path)
    {
        ++totals.examined;
        QByteArray original;
        if (!readFile(path, &original, 16 * 1024 * 1024) || !isRasterPayload(original))
        {
            ++totals.invalid;
            return;
        }
        const std::optional<CachedTile> address = cachedTileAddress(root, path, true);
        const QByteArray source = address.has_value() ?
            address->source.toUtf8() : QByteArrayLiteral("unknown");
        const int zoom = address.has_value() ? address->zoom : 0;
        const Aowis::Map::RasterTransferOptimizer::Selection selection =
            optimizer.select(original, source, zoom, quality, adaptive);
        ++totals.valid;
        totals.original_bytes += quint64(original.size());
        totals.transfer_bytes += quint64(selection.bytes.size());
        if (selection.jpeg) ++totals.optimized;
    };
    if (region.has_value()) visitRegionFiles(root, limit, true, *region, consume, &coverage);
    else visitFiles(root, limit, isRasterFile, consume);
    const Aowis::Map::RasterTransferOptimizer::Statistics statistics = optimizer.statistics();
    totals.codec_nanoseconds = statistics.encoding_microseconds * 1000;
    QJsonObject result = jsonTotals(totals);
    result.insert(QStringLiteral("jpeg_encodes_attempted"), qint64(statistics.tiles_evaluated));
    result.insert(QStringLiteral("skipped_small"), qint64(statistics.skipped_small));
    result.insert(QStringLiteral("skipped_by_predictor"), qint64(statistics.skipped_predictor));
    result.insert(QStringLiteral("skipped_unsafe"), qint64(statistics.skipped_unsafe));
    result.insert(QStringLiteral("variant_cache_hits"), qint64(statistics.cache_hits));
    if (region.has_value()) result.insert(QStringLiteral("region_coverage"), geographicCoverageJson(coverage));
    return result;
}

QJsonObject benchmarkTerrain(const QString &root, quint64 limit,
                             const std::optional<GeographicRegion> &region = std::nullopt)
{
    Totals totals;
    RegionCoverage coverage;
    const std::function<void(const QString &)> consume = [&](const QString &path)
    {
        ++totals.examined;
        QByteArray original;
        if (!readFile(path, &original, Aowis::Map::MaximumCanonicalTerrainTileBytes) ||
            !Aowis::Map::decodeTerrainTile(original).has_value())
        {
            ++totals.invalid;
            return;
        }
        QElapsedTimer timer;
        timer.start();
        const std::optional<QByteArray> compressed =
            Aowis::Map::compressTerrainTileForTransfer(original);
        totals.codec_nanoseconds += quint64(timer.nsecsElapsed());
        if (compressed.has_value())
        {
            const std::optional<QByteArray> roundtrip =
                Aowis::Map::decompressTerrainTileFromTransfer(*compressed);
            if (!roundtrip.has_value() || *roundtrip != original)
            {
                ++totals.invalid;
                return;
            }
        }
        ++totals.valid;
        totals.original_bytes += quint64(original.size());
        totals.transfer_bytes += quint64(compressed.has_value() ?
                                         compressed->size() : original.size());
        if (compressed.has_value()) ++totals.optimized;
    };
    if (region.has_value()) visitRegionFiles(root, limit, false, *region, consume, &coverage);
    else visitFiles(root, limit,
                    [](const QString &name) { return name.endsWith(QStringLiteral(".aowterrain")); },
                    consume);
    QJsonObject result = jsonTotals(totals);
    if (region.has_value()) result.insert(QStringLiteral("region_coverage"), geographicCoverageJson(coverage));
    return result;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

bool selfTest()
{
    QTemporaryDir temporary;
    if (!temporary.isValid()) return false;
    std::vector<unsigned char> pixels(256 * 256 * 3);
    quint32 seed = 0x12345678;
    for (size_t i = 0; i < pixels.size(); ++i)
    {
        seed = seed * 1664525u + 1013904223u;
        pixels[i] = static_cast<unsigned char>((i + (seed >> 27)) & 0xff);
    }
    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    png.width = 256;
    png.height = 256;
    png.format = PNG_FORMAT_RGB;
    png_alloc_size_t length = 0;
    if (!png_image_write_to_memory(&png, nullptr, &length, 0, pixels.data(), 0, nullptr))
        return false;
    QByteArray image(qsizetype(length), Qt::Uninitialized);
    if (!png_image_write_to_memory(&png, image.data(), &length, 0,
                                   pixels.data(), 0, nullptr)) return false;
    image.resize(qsizetype(length));

    Aowis::Map::TerrainTile terrain;
    terrain.dataset = QStringLiteral("copernicus-glo30");
    terrain.address = {12, 2034, 1355};
    terrain.nominal_resolution_m = 30.0;
    terrain.vertical_datum = Aowis::Map::TerrainVerticalDatum::Egm2008;
    terrain.elevations_m.fill(111.0f, Aowis::Map::TerrainTileSampleCount);
    const QByteArray canonical = Aowis::Map::encodeTerrainTile(terrain);
    const QString raster_dir = temporary.path() + QStringLiteral("/maptiles");
    const QString terrain_dir = temporary.path() + QStringLiteral("/terrain/normalized");
    if (image.isEmpty() || canonical.isEmpty() ||
        !writeFile(raster_dir + QStringLiteral("/provider/12/2034/1355.png"), image) ||
        !writeFile(terrain_dir + QStringLiteral("/v1/r2/copernicus-glo30/12/2034/1355.aowterrain"), canonical))
        return false;
    const QJsonObject raster = benchmarkRaster(raster_dir, 75, 10, false);
    const QJsonObject heights = benchmarkTerrain(terrain_dir, 10);
    GeographicRegion bitkine{QStringLiteral("Bitkine, Chad"), 11.9801, 18.2138, 25.0, 8, 16};
    const int bitkine_x = tileXAtLongitude(bitkine.longitude, 12);
    const int bitkine_y = tileYAtLatitude(bitkine.latitude, 12);
    terrain.address = {12, bitkine_x, bitkine_y};
    const QByteArray bitkine_canonical = Aowis::Map::encodeTerrainTile(terrain);
    if (bitkine_canonical.isEmpty() ||
        !writeFile(raster_dir + QStringLiteral("/provider/12/%1/%2.png")
                    .arg(bitkine_x).arg(bitkine_y), image) ||
        !writeFile(terrain_dir + QStringLiteral("/v1/r2/copernicus-glo30/12/%1/%2.aowterrain")
                    .arg(bitkine_x).arg(bitkine_y), bitkine_canonical))
        return false;
    const QJsonObject region_raster = benchmarkRaster(raster_dir, 75, 10, false, bitkine);
    const QJsonObject region_terrain = benchmarkTerrain(terrain_dir, 10, bitkine);
    const QJsonObject absent_raster = benchmarkRaster(raster_dir, 75, 10, false,
        GeographicRegion{QStringLiteral("unpopulated"), -33.0, -60.0, 5.0, 12, 12});
    return raster.value(QStringLiteral("valid_tiles")).toInteger() == 1 &&
           heights.value(QStringLiteral("valid_tiles")).toInteger() == 1 &&
           heights.value(QStringLiteral("optimized_tiles")).toInteger() == 1 &&
           heights.value(QStringLiteral("saved_percent")).toDouble() > 0.0 &&
           region_raster.value(QStringLiteral("valid_tiles")).toInteger() == 1 &&
           region_terrain.value(QStringLiteral("valid_tiles")).toInteger() == 1 &&
           absent_raster.value(QStringLiteral("valid_tiles")).toInteger() == 0 &&
           region_raster.value(QStringLiteral("region_coverage")).toObject()
             .value(QStringLiteral("cached_candidates")).toInteger() == 1;
}
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Read-only benchmark of cached MAP transfer payloads"));
    parser.addHelpOption();
    const QCommandLineOption raster_option(QStringList{QStringLiteral("raster")},
        QStringLiteral("Raster tile cache directory"), QStringLiteral("directory"));
    const QCommandLineOption terrain_option(QStringList{QStringLiteral("terrain")},
        QStringLiteral("Normalized terrain tile cache directory"), QStringLiteral("directory"));
    const QCommandLineOption quality_option(QStringList{QStringLiteral("jpeg-quality")},
        QStringLiteral("JPEG quality (1-100; default 75)"), QStringLiteral("quality"),
        QStringLiteral("75"));
    const QCommandLineOption max_option(QStringList{QStringLiteral("max-files")},
        QStringLiteral("Maximum files per category (default 1000)"), QStringLiteral("count"),
        QStringLiteral("1000"));
    const QCommandLineOption exhaustive_option(QStringList{QStringLiteral("exhaustive-jpeg")},
        QStringLiteral("Try JPEG on all eligible PNGs instead of adaptive sampling"));
    const QCommandLineOption region_option(QStringList{QStringLiteral("region")},
        QStringLiteral("Geographic sample: bitkine (Chad)"), QStringLiteral("name"));
    const QCommandLineOption radius_option(QStringList{QStringLiteral("radius-km")},
        QStringLiteral("Region radius in km (1-100; default 25)"),
        QStringLiteral("km"), QStringLiteral("25"));
    const QCommandLineOption min_zoom_option(QStringList{QStringLiteral("min-zoom")},
        QStringLiteral("Minimum zoom for geographic sample (default 8)"),
        QStringLiteral("zoom"), QStringLiteral("8"));
    const QCommandLineOption max_zoom_option(QStringList{QStringLiteral("max-zoom")},
        QStringLiteral("Maximum zoom for geographic sample (default 16)"),
        QStringLiteral("zoom"), QStringLiteral("16"));
    const QCommandLineOption selftest_option(QStringList{QStringLiteral("self-test")},
        QStringLiteral("Run the built-in codec and geographic selection smoke test"));
    parser.addOptions({raster_option, terrain_option, quality_option, max_option,
                       exhaustive_option, region_option, radius_option,
                       min_zoom_option, max_zoom_option, selftest_option});
    parser.process(application);
    if (parser.isSet(selftest_option))
        return selfTest() ? 0 : 1;
    bool quality_ok = false;
    bool max_ok = false;
    const int quality = parser.value(quality_option).toInt(&quality_ok);
    const qulonglong maximum = parser.value(max_option).toULongLong(&max_ok);
    if (!quality_ok || quality < 1 || quality > 100 || !max_ok || maximum == 0)
    {
        std::fprintf(stderr, "Invalid JPEG quality or max-files limit\n");
        return 2;
    }
    if (!parser.isSet(raster_option) && !parser.isSet(terrain_option))
    {
        std::fprintf(stderr, "Provide --raster and/or --terrain (see --help)\n");
        return 2;
    }
    std::optional<GeographicRegion> region;
    if (parser.isSet(region_option))
    {
        if (parser.value(region_option).compare(QStringLiteral("bitkine"),
                                                Qt::CaseInsensitive) != 0)
        {
            std::fprintf(stderr, "Unknown region. Supported: bitkine\n");
            return 2;
        }
        bool radius_ok = false;
        bool minimum_ok = false;
        bool maximum_ok = false;
        const double radius = parser.value(radius_option).toDouble(&radius_ok);
        const int minimum = parser.value(min_zoom_option).toInt(&minimum_ok);
        const int maximum_zoom = parser.value(max_zoom_option).toInt(&maximum_ok);
        if (!radius_ok || !std::isfinite(radius) || radius < 1.0 || radius > 100.0 ||
            !minimum_ok || !maximum_ok || minimum < 0 || maximum_zoom > 20 ||
            minimum > maximum_zoom)
        {
            std::fprintf(stderr, "Invalid geographic radius or zoom limits\n");
            return 2;
        }
        region = GeographicRegion{QStringLiteral("Bitkine, Chad"), 11.9801, 18.2138,
                                  radius, minimum, maximum_zoom};
    }
    else if (parser.isSet(radius_option) || parser.isSet(min_zoom_option) ||
             parser.isSet(max_zoom_option))
    {
        std::fprintf(stderr, "--radius-km and zoom limits require --region\n");
        return 2;
    }
    QJsonObject report;
    if (region.has_value())
    {
        QJsonObject area;
        area.insert(QStringLiteral("name"), region->name);
        area.insert(QStringLiteral("latitude"), region->latitude);
        area.insert(QStringLiteral("longitude"), region->longitude);
        area.insert(QStringLiteral("radius_km"), region->radius_km);
        area.insert(QStringLiteral("min_zoom"), region->minimum_zoom);
        area.insert(QStringLiteral("max_zoom"), region->maximum_zoom);
        report.insert(QStringLiteral("region"), area);
    }
    report.insert(QStringLiteral("jpeg_quality"), quality);
    report.insert(QStringLiteral("adaptive_jpeg"), !parser.isSet(exhaustive_option));
    report.insert(QStringLiteral("max_files_per_category"), qint64(maximum));
    quint64 valid = 0;
    bool region_missing_data = false;
    if (parser.isSet(raster_option))
    {
        const QString root = parser.value(raster_option);
        if (!QFileInfo(root).isDir())
        {
            std::fprintf(stderr, "Raster directory does not exist\n");
            return 2;
        }
        const QJsonObject result = benchmarkRaster(root, quality, maximum,
                                                   !parser.isSet(exhaustive_option), region);
        report.insert(QStringLiteral("raster"), result);
        const quint64 raster_valid = quint64(result.value(QStringLiteral("valid_tiles")).toInteger());
        valid += raster_valid;
        if (region.has_value() && raster_valid == 0)
        {
            region_missing_data = true;
            std::fprintf(stderr, "No cached raster tiles around Bitkine; visit the area "
                                 "in the GUI using a permitted imagery provider.\n");
        }
    }
    if (parser.isSet(terrain_option))
    {
        const QString root = parser.value(terrain_option);
        if (!QFileInfo(root).isDir())
        {
            std::fprintf(stderr, "Terrain directory does not exist\n");
            return 2;
        }
        const QJsonObject result = benchmarkTerrain(root, maximum, region);
        report.insert(QStringLiteral("terrain"), result);
        const quint64 terrain_valid = quint64(result.value(QStringLiteral("valid_tiles")).toInteger());
        valid += terrain_valid;
        if (region.has_value() && terrain_valid == 0)
        {
            region_missing_data = true;
            std::fprintf(stderr, "No cached terrain tiles around Bitkine; render the "
                                 "area in 3D before benchmarking terrain.\n");
        }
    }
    std::fputs(QJsonDocument(report).toJson(QJsonDocument::Indented).constData(), stdout);
    return valid > 0 && !region_missing_data ? 0 : 1;
}
