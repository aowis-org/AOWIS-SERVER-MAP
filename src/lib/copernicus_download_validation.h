#pragma once

#include <QtGlobal>

namespace Aowis::Map::Internal
{
// This downloader issues a full GET, never a Range request. Accepting 206 or
// an incomplete entity would allow an invalid partial COG into the native cache.
inline bool completeCopernicusResponse(int http_status, qint64 downloaded_bytes,
                                        qint64 declared_content_length)
{
    return http_status == 200 && downloaded_bytes > 0 &&
           (declared_content_length < 0 ||
            declared_content_length == downloaded_bytes);
}
}
