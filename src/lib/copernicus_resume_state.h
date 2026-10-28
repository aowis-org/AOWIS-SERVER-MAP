#pragma once

#include "copernicus_download_validation.h"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QSaveFile>
#include <QString>

#include <optional>

namespace Aowis::Map::Internal
{
// A partial source is never a valid GeoTIFF cache entry. Keep its bytes and
// metadata under separate names; the final cache path is published elsewhere.
struct CopernicusPartialState
{
    qint64 downloaded_bytes = 0;
    qint64 total_bytes = 0;
    QByteArray strong_etag;
};

inline bool validCopernicusStrongEtag(const QByteArray &etag)
{
    return etag.size() >= 2 && etag.size() <= 256 && etag.front() == '"' &&
           etag.back() == '"' && !etag.contains('\n') && !etag.contains('\r') &&
           !etag.contains('\0');
}

inline bool validCopernicusPartialState(const CopernicusPartialState &state,
                                       qint64 maximum_bytes)
{
    return maximum_bytes > 0 && state.downloaded_bytes > 0 &&
           state.downloaded_bytes < state.total_bytes &&
           state.total_bytes <= maximum_bytes &&
           validCopernicusStrongEtag(state.strong_etag);
}

// Metadata is written atomically. A crash between the partial and metadata
// updates leaves an invalid pair that the reader rejects, never an accepted
// incomplete final tile.
inline bool saveCopernicusPartialState(const QString &metadata_path,
                                      const CopernicusPartialState &state,
                                      qint64 maximum_bytes)
{
    if (!validCopernicusPartialState(state, maximum_bytes))
        return false;
    QSaveFile output(metadata_path);
    if (!output.open(QIODevice::WriteOnly))
        return false;
    const QByteArray data = "AOWIS-COPERNICUS-PARTIAL-1\n" +
        QByteArray::number(state.downloaded_bytes) + '\n' +
        QByteArray::number(state.total_bytes) + '\n' +
        state.strong_etag.toHex() + '\n';
    if (output.write(data) != data.size())
    {
        output.cancelWriting();
        return false;
    }
    return output.commit();
}

inline std::optional<CopernicusPartialState> loadCopernicusPartialState(
    const QString &partial_path, const QString &metadata_path, qint64 maximum_bytes)
{
    const QFileInfo info(partial_path);
    if (!info.isFile() || !info.isReadable() || info.size() <= 0 ||
        info.size() >= maximum_bytes)
        return std::nullopt;
    QFile metadata(metadata_path);
    if (!metadata.open(QIODevice::ReadOnly) || metadata.size() > 1024)
        return std::nullopt;
    const QList<QByteArray> lines = metadata.readAll().split('\n');
    if (lines.size() != 5 || lines[0] != "AOWIS-COPERNICUS-PARTIAL-1" ||
        !lines[4].isEmpty())
        return std::nullopt;
    const std::optional<qint64> downloaded = parseCopernicusPositiveDecimal(lines[1]);
    const std::optional<qint64> total = parseCopernicusPositiveDecimal(lines[2]);
    if (!downloaded || !total || lines[3].size() % 2 != 0)
        return std::nullopt;
    const QByteArray etag = QByteArray::fromHex(lines[3]);
    if (etag.toHex() != lines[3])
        return std::nullopt;
    const CopernicusPartialState state{*downloaded, *total, etag};
    if (!validCopernicusPartialState(state, maximum_bytes) ||
        info.size() != state.downloaded_bytes)
        return std::nullopt;
    return state;
}
}
