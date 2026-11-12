#include <aowis/map/terrain_tile.h>
#include <aowis/map/terrain_transfer.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QByteArray>

#include <zstd.h>

#include <cstdio>
#include <cstring>

namespace
{
bool require(bool condition, const char *message)
{
    if (!condition) std::fprintf(stderr, "%s\n", message);
    return condition;
}
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    using namespace Aowis::Map;
    TerrainTile tile;
    tile.dataset = QStringLiteral("copernicus-glo30");
    tile.address = {12, 2034, 1355};
    tile.vertical_datum = TerrainVerticalDatum::Egm2008;
    tile.nominal_resolution_m = 30.0;
    tile.elevations_m.fill(123.0f, TerrainTileSampleCount);
    const QByteArray canonical = encodeTerrainTile(tile);
    if (!require(!canonical.isEmpty(), "Failed to make canonical terrain tile")) return 1;

    QElapsedTimer timer;
    timer.start();
    const std::optional<QByteArray> compressed = compressTerrainTileForTransfer(canonical);
    const qint64 encode_nanoseconds = timer.nsecsElapsed();
    if (!require(compressed.has_value() && compressed->size() < canonical.size(),
                 "Flat terrain should compress well")) return 1;
    const std::optional<QByteArray> restored = decompressTerrainTileFromTransfer(*compressed);
    if (!require(restored.has_value() && *restored == canonical,
                 "zstd terrain roundtrip changed canonical tile bytes")) return 1;
    if (!require(decodeTerrainTile(*restored).has_value(), "Decompressed tile failed validation"))
        return 1;

    if (!require(!decompressTerrainTileFromTransfer(compressed->left(compressed->size() - 1)).has_value(),
                 "Truncated zstd frame accepted")) return 1;
    if (!require(!decompressTerrainTileFromTransfer(*compressed + QByteArray("trailer")).has_value(),
                 "Trailing data accepted")) return 1;
    QByteArray tampered = *compressed;
    tampered[tampered.size() / 2] = char(uchar(tampered.at(tampered.size() / 2)) ^ 0xff);
    const std::optional<QByteArray> corrupt = decompressTerrainTileFromTransfer(tampered);
    if (!require(!corrupt.has_value() || !decodeTerrainTile(corrupt.value()).has_value(),
                 "Corrupt zstd terrain passed canonical validation")) return 1;

    // A deceptively small frame must never allocate an arbitrary amount of RAM.
    const QByteArray huge(MaximumCanonicalTerrainTileBytes + 1, 'X');
    QByteArray huge_buffer(qsizetype(ZSTD_compressBound(size_t(huge.size()))), Qt::Uninitialized);
    const size_t huge_size = ZSTD_compress(huge_buffer.data(), size_t(huge_buffer.size()),
                                          huge.constData(), size_t(huge.size()), 1);
    if (!require(!ZSTD_isError(huge_size), "Failed to synthesize oversized zstd frame")) return 1;
    huge_buffer.resize(qsizetype(huge_size));
    if (!require(!decompressTerrainTileFromTransfer(huge_buffer).has_value(),
                 "Oversized uncompressed zstd frame accepted")) return 1;
    if (!require(!decompressTerrainTileFromTransfer(
                    QByteArray(maximumCompressedTerrainTileBytes() + 1, 'Q')).has_value(),
                 "Oversized compressed transfer accepted")) return 1;
    if (!require(!compressTerrainTileForTransfer(huge).has_value(),
                 "Oversized canonical input was compressed")) return 1;
    std::printf("Terrain zstd: %lld -> %lld bytes, encode %lld ns\n",
                static_cast<long long>(canonical.size()),
                static_cast<long long>(compressed->size()),
                static_cast<long long>(encode_nanoseconds));
    return 0;
}
