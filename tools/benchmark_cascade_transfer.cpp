#include <aowis/map/raster_transfer.h>
#include <aowis/map/terrain_tile.h>
#include <aowis/map/terrain_transfer.h>

#include <QByteArray>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <png.h>

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

QJsonObject benchmarkRaster(const QString &root, int quality, quint64 limit, bool adaptive)
{
    Totals totals;
    Aowis::Map::RasterTransferOptimizer optimizer;
    visitFiles(root, limit, isRasterFile, [&](const QString &path)
    {
        ++totals.examined;
        QByteArray original;
        if (!readFile(path, &original, 16 * 1024 * 1024) || !isRasterPayload(original))
        {
            ++totals.invalid;
            return;
        }
        QDir directory = QFileInfo(path).dir();  // x directory
        directory.cdUp();                         // z directory
        bool valid_zoom = false;
        const int zoom = directory.dirName().toInt(&valid_zoom);
        directory.cdUp();                         // provider directory
        const QByteArray source = directory.dirName().toUtf8();
        const Aowis::Map::RasterTransferOptimizer::Selection selection =
            optimizer.select(original, source, valid_zoom ? zoom : 0, quality, adaptive);
        ++totals.valid;
        totals.original_bytes += quint64(original.size());
        totals.transfer_bytes += quint64(selection.bytes.size());
        if (selection.jpeg) ++totals.optimized;
    });
    const Aowis::Map::RasterTransferOptimizer::Statistics statistics = optimizer.statistics();
    totals.codec_nanoseconds = statistics.encoding_microseconds * 1000;
    QJsonObject result = jsonTotals(totals);
    result.insert(QStringLiteral("jpeg_encodes_attempted"), qint64(statistics.tiles_evaluated));
    result.insert(QStringLiteral("skipped_small"), qint64(statistics.skipped_small));
    result.insert(QStringLiteral("skipped_by_predictor"), qint64(statistics.skipped_predictor));
    result.insert(QStringLiteral("skipped_unsafe"), qint64(statistics.skipped_unsafe));
    result.insert(QStringLiteral("variant_cache_hits"), qint64(statistics.cache_hits));
    return result;
}

QJsonObject benchmarkTerrain(const QString &root, quint64 limit)
{
    Totals totals;
    visitFiles(root, limit,
               [](const QString &name) { return name.endsWith(QStringLiteral(".aowterrain")); },
               [&](const QString &path)
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
    });
    return jsonTotals(totals);
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
    return raster.value(QStringLiteral("valid_tiles")).toInteger() == 1 &&
           heights.value(QStringLiteral("valid_tiles")).toInteger() == 1 &&
           heights.value(QStringLiteral("optimized_tiles")).toInteger() == 1 &&
           heights.value(QStringLiteral("saved_percent")).toDouble() > 0.0;
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
    const QCommandLineOption selftest_option(QStringList{QStringLiteral("self-test")},
        QStringLiteral("Run the built-in codec and file discovery smoke test"));
    parser.addOptions({raster_option, terrain_option, quality_option, max_option,
                       exhaustive_option, selftest_option});
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
    QJsonObject report;
    report.insert(QStringLiteral("jpeg_quality"), quality);
    report.insert(QStringLiteral("adaptive_jpeg"), !parser.isSet(exhaustive_option));
    report.insert(QStringLiteral("max_files_per_category"), qint64(maximum));
    quint64 valid = 0;
    if (parser.isSet(raster_option))
    {
        const QString root = parser.value(raster_option);
        if (!QFileInfo(root).isDir())
        {
            std::fprintf(stderr, "Raster directory does not exist\n");
            return 2;
        }
        const QJsonObject result = benchmarkRaster(root, quality, maximum,
                                                   !parser.isSet(exhaustive_option));
        report.insert(QStringLiteral("raster"), result);
        valid += quint64(result.value(QStringLiteral("valid_tiles")).toInteger());
    }
    if (parser.isSet(terrain_option))
    {
        const QString root = parser.value(terrain_option);
        if (!QFileInfo(root).isDir())
        {
            std::fprintf(stderr, "Terrain directory does not exist\n");
            return 2;
        }
        const QJsonObject result = benchmarkTerrain(root, maximum);
        report.insert(QStringLiteral("terrain"), result);
        valid += quint64(result.value(QStringLiteral("valid_tiles")).toInteger());
    }
    std::fputs(QJsonDocument(report).toJson(QJsonDocument::Indented).constData(), stdout);
    return valid > 0 ? 0 : 1;
}
