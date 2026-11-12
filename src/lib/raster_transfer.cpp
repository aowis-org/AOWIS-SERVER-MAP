#include <aowis/map/raster_transfer.h>

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QMutexLocker>

#include <png.h>
#include <turbojpeg.h>

#include <limits>
#include <vector>

namespace Aowis::Map
{
namespace
{
constexpr qsizetype MinimumOriginalBytes = 12 * 1024;
constexpr qsizetype MinimumSavedBytes = 2 * 1024;
constexpr qsizetype MaximumVariantCacheBytes = 16 * 1024 * 1024;
constexpr qsizetype MaximumVariantEntries = 4096;
constexpr unsigned MaximumImageSide = 1024;

bool encodeOpaquePngToJpeg(const QByteArray &original, int quality, QByteArray *output)
{
    if (original.size() < 24 || !original.startsWith("\x89PNG\r\n\x1a\n")) return false;

    png_image picture{};
    picture.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&picture, original.constData(),
                                          size_t(original.size())))
        return false;

    // libpng's simplified API checks PNG integrity. Reject giant images before
    // allocating RGBA or RGB buffers for a malicious upstream response.
    if (picture.width == 0 || picture.height == 0 ||
        picture.width > MaximumImageSide || picture.height > MaximumImageSide)
    {
        png_image_free(&picture);
        return false;
    }
    picture.format = PNG_FORMAT_RGBA;
    const size_t pixel_count = size_t(picture.width) * picture.height;
    std::vector<unsigned char> rgba(pixel_count * 4);
    if (!png_image_finish_read(&picture, nullptr, rgba.data(), 0, nullptr))
    {
        png_image_free(&picture);
        return false;
    }
    png_image_free(&picture);

    // Alpha is a hard opt-out, including palette tRNS and partially transparent
    // pixels. It must never be flattened against a guessed background.
    std::vector<unsigned char> rgb(pixel_count * 3);
    for (size_t pixel = 0; pixel < pixel_count; ++pixel)
    {
        if (rgba[pixel * 4 + 3] != 255) return false;
        rgb[pixel * 3] = rgba[pixel * 4];
        rgb[pixel * 3 + 1] = rgba[pixel * 4 + 1];
        rgb[pixel * 3 + 2] = rgba[pixel * 4 + 2];
    }

    tjhandle encoder = tjInitCompress();
    if (encoder == nullptr) return false;
    unsigned char *encoded = nullptr;
    unsigned long encoded_size = 0;
    const int result = tjCompress2(encoder, rgb.data(), int(picture.width), 0,
                                   int(picture.height), TJPF_RGB, &encoded,
                                   &encoded_size, TJSAMP_420, quality, TJFLAG_FASTDCT);
    if (result == 0 && encoded != nullptr &&
        encoded_size <= static_cast<unsigned long>(std::numeric_limits<int>::max()))
        *output = QByteArray(reinterpret_cast<const char *>(encoded), int(encoded_size));
    tjFree(encoded);
    tjDestroy(encoder);
    return result == 0 && !output->isEmpty();
}
}

QByteArray RasterTransferOptimizer::cacheKey(const QByteArray &original, int quality)
{
    QByteArray key = QCryptographicHash::hash(original, QCryptographicHash::Sha256);
    key.append(char(quality));
    return key;
}

QString RasterTransferOptimizer::bucketKey(const QByteArray &source, int zoom,
                                          int size, int quality)
{
    const int zoom_band = zoom < 8 ? 0 : zoom < 12 ? 1 : zoom < 16 ? 2 : 3;
    const int size_band = size < 32768 ? 0 : size < 65536 ? 1 :
                          size < 98304 ? 2 : 3;
    return QString::fromLatin1(source) + QLatin1Char('/') +
           QString::number(zoom_band) + QLatin1Char('/') +
           QString::number(size_band) + QLatin1Char('/') + QString::number(quality);
}

void RasterTransferOptimizer::cacheLocked(const QByteArray &key, const Selection &selection)
{
    // Only the variant is stored: original bytes remain in MapTiles' cache.
    const qsizetype cost = selection.jpeg ? selection.bytes.size() : 0;
    if (cost > MaximumVariantCacheBytes) return;
    while (!this->insertion_order.isEmpty() &&
           (this->variants.size() >= MaximumVariantEntries ||
            cost > MaximumVariantCacheBytes - this->cached_byte_count))
    {
        const QByteArray oldest = this->insertion_order.takeFirst();
        const Cached removed = this->variants.take(oldest);
        if (removed.jpeg) this->cached_byte_count -= removed.bytes.size();
    }
    this->variants.insert(key, Cached{selection.jpeg ? selection.bytes : QByteArray(), selection.jpeg});
    this->insertion_order.append(key);
    this->cached_byte_count += cost;
}

std::optional<RasterTransferOptimizer::Selection>
RasterTransferOptimizer::lookup(const QByteArray &original, int quality)
{
    if (quality < 1 || quality > 100 || original.size() < MinimumOriginalBytes)
        return std::nullopt;
    const QByteArray key = cacheKey(original, quality);
    QMutexLocker locker(&this->mutex);
    const QHash<QByteArray, Cached>::const_iterator found = this->variants.constFind(key);
    if (found == this->variants.constEnd()) return std::nullopt;
    ++this->counters.cache_hits;
    return Selection{found->jpeg ? found->bytes : original, found->jpeg, true};
}

RasterTransferOptimizer::Selection RasterTransferOptimizer::select(
    const QByteArray &original, const QByteArray &source_fingerprint,
    int zoom, int quality, bool adaptive)
{
    Selection unchanged{original, false, false};
    {
        QMutexLocker locker(&this->mutex);
        ++this->counters.tiles_considered;
    }
    if (quality < 1 || quality > 100 || original.size() < MinimumOriginalBytes)
    {
        QMutexLocker locker(&this->mutex);
        ++this->counters.skipped_small;
        return unchanged;
    }
    if (!original.startsWith("\x89PNG\r\n\x1a\n"))
    {
        QMutexLocker locker(&this->mutex);
        ++this->counters.skipped_unsafe;
        return unchanged;
    }

    const QByteArray key = cacheKey(original, quality);
    const QString bucket_name = bucketKey(source_fingerprint, zoom, int(original.size()), quality);
    {
        QMutexLocker locker(&this->mutex);
        const QHash<QByteArray, Cached>::const_iterator cached = this->variants.constFind(key);
        if (cached != this->variants.constEnd())
        {
            ++this->counters.cache_hits;
            return Selection{cached->jpeg ? cached->bytes : original, cached->jpeg, true};
        }
        // Never encode an identical tile twice concurrently. If it is already
        // being evaluated, send canonical bytes without waiting.
        if (this->in_progress.contains(key))
            return unchanged;

        if (this->buckets.size() >= 8192 && !this->buckets.contains(bucket_name))
            this->buckets.clear();
        Bucket &bucket = this->buckets[bucket_name];
        ++bucket.visits;
        // Bandwidth is the scarce resource in poor-internet mode. The old
        // policy skipped 25% of tiles even in buckets where JPEG nearly always
        // won; the Bitkine benchmark showed that those skipped tiles cost real
        // WAN bytes. Test the first eight eligible tiles, then keep converting
        // unless the *last eight attempts* all failed the size/quality rule.
        // A losing bucket gets a periodic probe (1/32 visits) so changes in
        // geographic content can immediately re-enable JPEG conversion.
        // The server's two-worker admission limit still bounds CPU use.
        if (adaptive && original.size() < 98304 &&
            bucket.recent_count == 8 && bucket.recent_results == 0 &&
            bucket.visits % 32 != 0)
        {
            ++this->counters.skipped_predictor;
            return unchanged;
        }
        this->in_progress.insert(key);
    }

    QElapsedTimer timer;
    timer.start();
    QByteArray candidate;
    const bool encoded = encodeOpaquePngToJpeg(original, quality, &candidate);
    const quint64 elapsed = quint64(timer.nsecsElapsed() / 1000);
    const bool wins = encoded && candidate.size() + MinimumSavedBytes <= original.size() &&
                      qint64(candidate.size()) * 10 <= qint64(original.size()) * 9;
    Selection selection = wins ? Selection{candidate, true, false} : unchanged;
    {
        QMutexLocker locker(&this->mutex);
        this->in_progress.remove(key);
        Bucket &bucket = this->buckets[bucket_name];
        ++bucket.evaluations;
        bucket.recent_results = quint8((unsigned(bucket.recent_results) << 1) |
                                      (wins ? 1u : 0u));
        if (bucket.recent_count < 8) ++bucket.recent_count;
        if (wins)
        {
            ++bucket.wins;
            bucket.total_saved += quint64(original.size() - candidate.size());
            ++this->counters.jpeg_selected;
        }
        ++this->counters.tiles_evaluated;
        this->counters.encoding_microseconds += elapsed;
        if (!encoded) ++this->counters.skipped_unsafe;
        this->cacheLocked(key, selection);
    }
    return selection;
}

void RasterTransferOptimizer::recordTransfer(qsizetype original_size, qsizetype sent_size)
{
    QMutexLocker locker(&this->mutex);
    this->counters.original_bytes += quint64(qMax(qsizetype(0), original_size));
    this->counters.transmitted_bytes += quint64(qMax(qsizetype(0), sent_size));
}

RasterTransferOptimizer::Statistics RasterTransferOptimizer::statistics() const
{
    QMutexLocker locker(&this->mutex);
    return this->counters;
}
}
