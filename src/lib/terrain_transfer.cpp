#include <aowis/map/terrain_transfer.h>

#include <zstd.h>

#include <limits>

namespace Aowis::Map
{
qsizetype maximumCompressedTerrainTileBytes()
{
    return qsizetype(ZSTD_compressBound(size_t(MaximumCanonicalTerrainTileBytes)));
}

std::optional<QByteArray> compressTerrainTileForTransfer(const QByteArray &canonical)
{
    if (canonical.isEmpty() || canonical.size() > MaximumCanonicalTerrainTileBytes)
        return std::nullopt;
    const size_t capacity = ZSTD_compressBound(size_t(canonical.size()));
    if (capacity > size_t(std::numeric_limits<int>::max()))
        return std::nullopt;
    QByteArray compressed(qsizetype(capacity), Qt::Uninitialized);
    const size_t written = ZSTD_compress(compressed.data(), capacity,
                                         canonical.constData(), size_t(canonical.size()), 3);
    if (ZSTD_isError(written) || written >= size_t(canonical.size()))
        return std::nullopt;
    compressed.resize(qsizetype(written));
    return compressed;
}

namespace
{
std::optional<QByteArray> transferFailure(const QString &message, QString *error_message)
{
    if (error_message != nullptr) *error_message = message;
    return std::nullopt;
}
}

std::optional<QByteArray> decompressTerrainTileFromTransfer(
    const QByteArray &compressed, QString *error_message)
{
    if (compressed.isEmpty() || compressed.size() > maximumCompressedTerrainTileBytes())
        return transferFailure(QStringLiteral("Compressed terrain exceeds transfer size limit"), error_message);

    const unsigned long long frame_size = ZSTD_getFrameContentSize(
        compressed.constData(), size_t(compressed.size()));
    if (frame_size == ZSTD_CONTENTSIZE_UNKNOWN || frame_size == ZSTD_CONTENTSIZE_ERROR ||
        frame_size == 0 || frame_size > quint64(MaximumCanonicalTerrainTileBytes))
        return transferFailure(QStringLiteral("Invalid or oversized terrain zstd frame"), error_message);
    // Reject concatenated/trailing frames and padding before allocating.
    const size_t frame_bytes = ZSTD_findFrameCompressedSize(
        compressed.constData(), size_t(compressed.size()));
    if (ZSTD_isError(frame_bytes) || frame_bytes != size_t(compressed.size()))
        return transferFailure(QStringLiteral("Terrain zstd transfer is truncated or has trailing bytes"), error_message);

    QByteArray canonical(qsizetype(frame_size), Qt::Uninitialized);
    const size_t decoded = ZSTD_decompress(canonical.data(), size_t(canonical.size()),
                                           compressed.constData(), size_t(compressed.size()));
    if (ZSTD_isError(decoded) || decoded != size_t(canonical.size()))
        return transferFailure(QStringLiteral("Terrain zstd decompression failed"), error_message);
    return canonical;
}
}
