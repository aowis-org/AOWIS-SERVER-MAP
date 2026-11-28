#ifndef AOWIS_MAP_GLOBAL_TERRAIN_OVERVIEW_H
#define AOWIS_MAP_GLOBAL_TERRAIN_OVERVIEW_H

#include <aowis/map/terrain_data.h>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QtEndian>
#include <QStringList>

namespace Aowis::Map::GlobalTerrainOverview
{
// An immutable, locally provisioned visual overview. This is deliberately
// separate from both the provider-native and normalized/evictable stores.
inline constexpr int MaximumZoom = 7;
inline constexpr qint64 HeaderBytes = 16;
inline constexpr qint64 TileCount = ((qint64(1) << (2 * (MaximumZoom + 1))) - 1) / 3;
inline constexpr qint64 RecordBytes = 64 + 16 + TerrainTileMetadataBytes
                                      + TerrainTileSampleCount * 2;
inline constexpr qint64 PackBytes = HeaderBytes + TileCount * RecordBytes;
inline const QString Dataset = QStringLiteral("copernicus-glo30");

inline QString path(const QString &directory)
{
    return QDir(directory).filePath(QStringLiteral("global-overview-v1.aowpack"));
}

inline bool supports(const QString &dataset, const TerrainTileAddress &address)
{
    return dataset == Dataset && address.zoom <= MaximumZoom && isValidTerrainTileAddress(address);
}

inline bool validHeader(QFile &file)
{
    if (!file.open(QIODevice::ReadOnly) || file.size() != PackBytes)
        return false;
    const QByteArray header = file.read(HeaderBytes);
    return header.size() == HeaderBytes && header.startsWith("AOWCOAR1")
        && qFromLittleEndian<quint32>(header.constData() + 8) == quint32(MaximumZoom)
        && qFromLittleEndian<quint32>(header.constData() + 12) == quint32(RecordBytes);
}

// Prefer an administrator-provisioned overview, then the immutable data
// shipped beside the executable or in the Debian package's share directory.
inline bool openOverview(QFile &file, const QString &directory)
{
    QStringList directories{directory};
    if (QCoreApplication::instance())
    {
        const QDir application_directory(QCoreApplication::applicationDirPath());
        directories.append(application_directory.filePath(QStringLiteral("terrain-data")));
        directories.append(application_directory.filePath(QStringLiteral("../share/aowis-server-map/terrain")));
    }
    for (const QString &candidate : directories)
    {
        file.close();
        file.setFileName(path(candidate));
        if (validHeader(file))
            return true;
    }
    file.close();
    return false;
}

inline bool available(const QString &directory)
{
    QFile file;
    return openOverview(file, directory);
}

inline qint64 offset(const TerrainTileAddress &address)
{
    const qint64 side = qint64(1) << address.zoom;
    const qint64 preceding = (side * side - 1) / 3;
    return HeaderBytes + (preceding + qint64(address.y) * side + address.x) * RecordBytes;
}

inline std::optional<TerrainTileLookupResult> read(const QString &directory,
                                                   const QString &dataset,
                                                   const TerrainTileAddress &address)
{
    if (!supports(dataset, address))
        return std::nullopt;
    QFile file;
    if (!openOverview(file, directory))
        return std::nullopt;
    TerrainTileLookupResult result;
    result.status = TerrainTileLookupStatus::CorruptTile;
    if (!file.seek(offset(address)))
    {
        result.error_message = QStringLiteral("Could not seek global terrain overview record");
        return result;
    }
    const QByteArray data = file.read(RecordBytes);
    QString error;
    const std::optional<TerrainTile> tile = decodeTerrainTile(data, &error);
    if (!tile || tile->dataset != dataset || tile->address.zoom != address.zoom
        || tile->address.x != address.x || tile->address.y != address.y
        || tile->vertical_datum != TerrainVerticalDatum::Egm2008)
    {
        result.error_message = QStringLiteral("Invalid global terrain overview record: %1").arg(error);
        return result;
    }
    result.status = TerrainTileLookupStatus::Ready;
    result.origin = TerrainDataOrigin::Cache;
    result.data = data;
    result.source_statistics = tile->source_statistics;
    result.geometric_error = tile->geometric_error;
    return result;
}
}
#endif
