#include <aowis/map/raster_transfer.h>

#include <QByteArray>

#include <png.h>

#include <cstdio>
#include <vector>

namespace
{
bool require(bool result, const char *message)
{
    if (!result) std::fprintf(stderr, "%s\n", message);
    return result;
}

QByteArray pngTile(int width, int height, bool transparent,
                   quint32 initial_state = 0x1a2b3c4d, bool hard_edges = false)
{
    const int channels = transparent ? 4 : 3;
    std::vector<unsigned char> pixels(size_t(width) * height * channels);
    quint32 state = initial_state;
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            state = state * 1664525u + 1013904223u;
            const int noise = int((state >> 24) & 31);
            const size_t offset = (size_t(y) * width + x) * channels;
            if (hard_edges)
            {
                // This RGB PNG compresses well losslessly, but sharp alternating
                // black/white edges make its JPEG candidate larger.
                const unsigned char value = ((x + y + noise / 8) % 2) ? 255 : 0;
                pixels[offset] = value;
                pixels[offset + 1] = value;
                pixels[offset + 2] = value;
            }
            else
            {
                pixels[offset] = static_cast<unsigned char>((x * 2 + noise) % 256);
                pixels[offset + 1] = static_cast<unsigned char>((y * 2 + noise) % 256);
                pixels[offset + 2] = static_cast<unsigned char>((x + y + noise) % 256);
            }
            if (transparent)
                pixels[offset + 3] = static_cast<unsigned char>((x == 0 && y == 0) ? 0 : 255);
        }
    }
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    image.width = png_uint_32(width);
    image.height = png_uint_32(height);
    image.format = transparent ? PNG_FORMAT_RGBA : PNG_FORMAT_RGB;
    png_alloc_size_t output_size = 0;
    if (!png_image_write_to_memory(&image, nullptr, &output_size, 0, pixels.data(), 0, nullptr))
        return {};
    QByteArray result(int(output_size), Qt::Uninitialized);
    if (!png_image_write_to_memory(&image, result.data(), &output_size, 0,
                                   pixels.data(), 0, nullptr)) return {};
    result.resize(int(output_size));
    return result;
}
}

int main()
{
    using Aowis::Map::RasterTransferOptimizer;
    RasterTransferOptimizer optimizer;
    const QByteArray original = pngTile(256, 256, false);
    if (!require(original.size() > 12 * 1024, "Test PNG unexpectedly too small")) return 1;
    const RasterTransferOptimizer::Selection selected = optimizer.select(
        original, QByteArrayLiteral("source-1"), 12, 75, false);
    if (!require(selected.jpeg && selected.bytes.startsWith("\xff\xd8\xff") &&
                 selected.bytes.size() <= original.size() * 9 / 10 &&
                 selected.bytes.size() + 2048 <= original.size(),
                 "Opaque PNG was not converted into a useful JPEG")) return 1;
    const RasterTransferOptimizer::Selection reused = optimizer.select(
        original, QByteArrayLiteral("source-1"), 12, 75, false);
    if (!require(reused.jpeg && reused.from_cache && reused.bytes == selected.bytes,
                 "JPEG variant was not reused")) return 1;
    if (!require(!optimizer.lookup(original, 90).has_value(),
                 "JPEG quality change reused a stale variant")) return 1;
    const RasterTransferOptimizer::Selection different_quality = optimizer.select(
        original, QByteArrayLiteral("source-1"), 12, 90, false);
    if (!require(different_quality.bytes != selected.bytes,
                 "Different JPEG quality yielded identical cached variant")) return 1;

    const QByteArray alpha = pngTile(256, 256, true);
    if (!require(alpha.size() > 12 * 1024, "Transparent PNG unexpectedly too small")) return 1;
    const RasterTransferOptimizer::Selection transparent = optimizer.select(
        alpha, QByteArrayLiteral("source-1"), 12, 75, false);
    if (!require(!transparent.jpeg && transparent.bytes == alpha,
                 "Lossy transfer flattened PNG transparency")) return 1;
    const RasterTransferOptimizer::Selection already_jpeg = optimizer.select(
        selected.bytes, QByteArrayLiteral("source-1"), 12, 75, false);
    if (!require(!already_jpeg.jpeg && already_jpeg.bytes == selected.bytes,
                 "JPEG was transcoded a second time")) return 1;
    const QByteArray small(1000, 'x');
    if (!require(optimizer.select(small, QByteArrayLiteral("source"), 10, 75).bytes == small,
                 "Tiny tiles should not be evaluated")) return 1;

    // Bitkine's real raster sample showed 43.9% exhaustive JPEG savings but
    // only 25.2% from the old adaptive predictor. A bucket that consistently
    // wins must never skip an arbitrary fraction of eligible tiles.
    RasterTransferOptimizer good_bucket;
    for (int index = 0; index < 24; ++index)
    {
        const QByteArray tile = pngTile(128, 128, false, 0x1a2b3c4du + quint32(index));
        if (!require(tile.size() > 12 * 1024 && tile.size() < 32 * 1024,
                     "Adaptive winning-tile fixture left the 12-32 KiB bucket")) return 1;
        const RasterTransferOptimizer::Selection choice = good_bucket.select(
            tile, QByteArrayLiteral("bitkine-provider"), 14, 75, true);
        if (!require(choice.jpeg && choice.bytes.size() + 2048 <= tile.size(),
                     "Adaptive selector skipped a proven profitable JPEG")) return 1;
    }
    const RasterTransferOptimizer::Statistics winning_stats = good_bucket.statistics();
    if (!require(winning_stats.tiles_evaluated == 24 &&
                 winning_stats.skipped_predictor == 0,
                 "Winning bucket should evaluate every eligible tile")) return 1;

    // Conversely, a bucket with eight consecutive real losses backs off to
    // one probe per 32 visits. A successful probe must immediately recover
    // aggressive encoding, even when the previous tiles showed hard edges.
    RasterTransferOptimizer losing_bucket;
    for (int index = 0; index < 31; ++index)
    {
        const QByteArray tile = pngTile(256, 256, false,
                                        0x6b123400u + quint32(index), true);
        if (!require(tile.size() > 12 * 1024 && tile.size() < 32 * 1024,
                     "Adaptive losing-tile fixture left the 12-32 KiB bucket")) return 1;
        const RasterTransferOptimizer::Selection choice = losing_bucket.select(
            tile, QByteArrayLiteral("mixed-provider"), 14, 75, true);
        if (!require(!choice.jpeg && choice.bytes == tile,
                     "High-contrast PNG should remain lossless")) return 1;
    }
    const RasterTransferOptimizer::Statistics losing_stats = losing_bucket.statistics();
    if (!require(losing_stats.tiles_evaluated == 8 &&
                 losing_stats.skipped_predictor == 23,
                 "Losing bucket did not back off after eight misses")) return 1;
    const QByteArray probe = pngTile(128, 128, false, 0x789abcdeu);
    if (!require(losing_bucket.select(probe, QByteArrayLiteral("mixed-provider"),
                                      14, 75, true).jpeg,
                 "Periodic sample did not detect a profitable JPEG")) return 1;
    const QByteArray recovered = pngTile(128, 128, false, 0x789abcdfu);
    if (!require(losing_bucket.select(recovered, QByteArrayLiteral("mixed-provider"),
                                      14, 75, true).jpeg,
                 "Profitable probe did not restore aggressive encoding")) return 1;

    optimizer.recordTransfer(original.size(), selected.bytes.size());
    const RasterTransferOptimizer::Statistics stats = optimizer.statistics();
    if (!require(stats.cache_hits > 0 && stats.tiles_evaluated >= 2 &&
                 stats.original_bytes == quint64(original.size()) &&
                 stats.transmitted_bytes == quint64(selected.bytes.size()),
                 "Raster transfer statistics are incorrect")) return 1;
    return 0;
}
