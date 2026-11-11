#ifndef AOWIS_MAP_RASTER_TRANSFER_H
#define AOWIS_MAP_RASTER_TRANSFER_H

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QSet>
#include <QString>

#include <optional>

namespace Aowis::Map
{
// Bounded, thread-safe MAP-to-MAP raster variant selector. The original tile
// cache is never altered; ordinary GUI clients never enter this code path.
class RasterTransferOptimizer
{
public:
    struct Selection
    {
        QByteArray bytes;
        bool jpeg = false;
        bool from_cache = false;
    };

    struct Statistics
    {
        quint64 tiles_considered = 0;
        quint64 tiles_evaluated = 0;
        quint64 jpeg_selected = 0;
        quint64 cache_hits = 0;
        quint64 skipped_small = 0;
        quint64 skipped_predictor = 0;
        quint64 skipped_unsafe = 0;
        quint64 original_bytes = 0;
        quint64 transmitted_bytes = 0;
        quint64 encoding_microseconds = 0;
    };

    // Fast lookup for a previously selected variant (including an original
    // decision). Called before the server's bounded encoder pool admission.
    std::optional<Selection> lookup(const QByteArray &original, int quality);
    Selection select(const QByteArray &original, const QByteArray &source_fingerprint,
                     int zoom, int quality, bool adaptive = true);
    void recordTransfer(qsizetype original_size, qsizetype sent_size);
    Statistics statistics() const;

private:
    struct Cached
    {
        QByteArray bytes;
        bool jpeg = false;
    };
    struct Bucket
    {
        quint64 visits = 0;
        quint64 evaluations = 0;
        quint64 wins = 0;
        quint64 total_saved = 0;
    };

    static QByteArray cacheKey(const QByteArray &original, int quality);
    static QString bucketKey(const QByteArray &source, int zoom, int size, int quality);
    void cacheLocked(const QByteArray &key, const Selection &selection);

    mutable QMutex mutex;
    QHash<QByteArray, Cached> variants;
    QList<QByteArray> insertion_order;
    QSet<QByteArray> in_progress;
    QHash<QString, Bucket> buckets;
    qsizetype cached_byte_count = 0;
    Statistics counters;
};
}

#endif // AOWIS_MAP_RASTER_TRANSFER_H
