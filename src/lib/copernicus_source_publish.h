#pragma once

#include <QFile>
#include <QSaveFile>
#include <QString>

namespace Aowis::Map::Internal
{
// Assemble only verified input segments into an atomically published source.
// The caller validates HTTP ranges and source identity before using this helper.
inline bool publishCopernicusSource(const QString &final_path,
                                   const QString &incoming_path,
                                   const QString &prefix_path = QString())
{
    QSaveFile output(final_path);
    if (!output.open(QIODevice::WriteOnly))
        return false;
    const auto append_file = [&](const QString &path) -> bool
    {
        QFile input(path);
        if (!input.open(QIODevice::ReadOnly))
            return false;
        while (!input.atEnd())
        {
            const QByteArray chunk = input.read(256 * 1024);
            if (chunk.isEmpty() || output.write(chunk) != chunk.size())
                return false;
        }
        return true;
    };
    if ((!prefix_path.isEmpty() && !append_file(prefix_path)) ||
        !append_file(incoming_path))
    {
        output.cancelWriting();
        return false;
    }
    return output.commit();
}
}
