#pragma once

#include <aowis/map/cache_storage.h>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QString>
#include <algorithm>
#include <functional>

namespace Aowis::Map::Internal
{
// Assemble only verified input segments into an atomically published source.
// The caller validates HTTP ranges and source identity before using this helper.
inline bool publishCopernicusSource(const QString &final_path,
                                   const QString &incoming_path,
                                   const QString &prefix_path = QString())
{
    const QFileInfo incoming(incoming_path);
    const QFileInfo prefix(prefix_path);
    if (!incoming.isFile() || incoming.size() < 0 ||
        (!prefix_path.isEmpty() && (!prefix.isFile() || prefix.size() < 0)))
        return false;
    const std::uint64_t expected_incoming = std::uint64_t(incoming.size());
    const std::uint64_t expected_prefix = prefix_path.isEmpty() ? 0 : std::uint64_t(prefix.size());
    // qint64 file sizes make their uint64_t sum representable. Both inputs
    // already occupy disk space; the atomic publication needs an additional copy.
    const CacheDiskWriteReservation reservation = reserveCacheDiskWrite(
        std::filesystem::path(QFileInfo(final_path).absolutePath().toStdU16String()),
        expected_incoming + expected_prefix);
    if (!reservation) return false;
    QSaveFile output(final_path);
    if (!output.open(QIODevice::WriteOnly))
        return false;
    const std::function<bool(const QString &, std::uint64_t)> append_file =
        [&output](const QString &path, std::uint64_t expected) -> bool
    {
        QFile input(path);
        if (!input.open(QIODevice::ReadOnly) || input.size() < 0 || std::uint64_t(input.size()) != expected)
            return false;
        while (expected > 0)
        {
            const QByteArray chunk = input.read(qint64(std::min<std::uint64_t>(expected, 256 * 1024)));
            if (chunk.isEmpty() || output.write(chunk) != chunk.size())
                return false;
            expected -= std::uint64_t(chunk.size());
        }
        return input.atEnd();
    };
    if ((!prefix_path.isEmpty() && !append_file(prefix_path, expected_prefix)) ||
        !append_file(incoming_path, expected_incoming))
    {
        output.cancelWriting();
        return false;
    }
    return output.commit();
}
}
