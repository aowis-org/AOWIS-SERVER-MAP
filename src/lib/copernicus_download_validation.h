#pragma once

#include <QtGlobal>
#include <QByteArray>
#include <optional>
#include <limits>

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
// These rules are deliberately stricter than general-purpose HTTP parsing.
// Resumed source data must belong to the same entity and start at the exact
// existing byte offset. No partial data is published by this helper.
struct CopernicusContentRange
{
    qint64 first = 0;
    qint64 last = 0;
    qint64 total = 0;
};

inline std::optional<qint64> parseCopernicusPositiveDecimal(const QByteArray &bytes)
{
    if (bytes.isEmpty())
        return std::nullopt;
    qint64 value = 0;
    for (char digit : bytes)
    {
        if (digit < '0' || digit > '9')
            return std::nullopt;
        const int next_digit = digit - '0';
        if (value > (std::numeric_limits<qint64>::max() - next_digit) / 10)
            return std::nullopt;
        value = value * 10 + next_digit;
    }
    return value;
}

inline std::optional<CopernicusContentRange> parseCopernicusContentRange(
    const QByteArray &header)
{
    // Reject whitespace variants, unknown totals, multipart ranges, signs,
    // and integer overflows rather than guessing which bytes can be appended.
    if (!header.startsWith("bytes "))
        return std::nullopt;
    const QByteArray range = header.mid(6);
    const int dash = range.indexOf('-');
    const int slash = range.indexOf('/');
    if (dash <= 0 || slash <= dash + 1 || slash == range.size() - 1 ||
        range.indexOf('-', dash + 1) >= 0 || range.indexOf('/', slash + 1) >= 0)
        return std::nullopt;
    const std::optional<qint64> first = parseCopernicusPositiveDecimal(range.left(dash));
    const std::optional<qint64> last =
        parseCopernicusPositiveDecimal(range.mid(dash + 1, slash - dash - 1));
    const std::optional<qint64> total =
        parseCopernicusPositiveDecimal(range.mid(slash + 1));
    if (!first || !last || !total || *total == 0 || *first > *last || *last >= *total)
        return std::nullopt;
    return CopernicusContentRange{*first, *last, *total};
}

enum class CopernicusResumeAction
{
    RestartWithFullEntity,
    AppendVerifiedRange,
    RejectResponse
};

inline CopernicusResumeAction classifyCopernicusResumeResponse(
    int http_status, qint64 existing_bytes, qint64 received_bytes,
    const QByteArray &content_range, const QByteArray &stored_strong_etag,
    const QByteArray &response_etag, qint64 maximum_source_bytes)
{
    if (http_status == 200 &&
        completeCopernicusResponse(http_status, received_bytes, -1) &&
        received_bytes <= maximum_source_bytes)
        return CopernicusResumeAction::RestartWithFullEntity;

    if (http_status != 206 || existing_bytes <= 0 || received_bytes <= 0 ||
        maximum_source_bytes <= 0 || stored_strong_etag.isEmpty() ||
        stored_strong_etag.startsWith("W/") ||
        response_etag != stored_strong_etag)
        return CopernicusResumeAction::RejectResponse;

    const std::optional<CopernicusContentRange> range =
        parseCopernicusContentRange(content_range);
    if (!range || range->first != existing_bytes || range->total > maximum_source_bytes ||
        range->last - range->first + 1 != received_bytes ||
        range->last + 1 != range->total)
        return CopernicusResumeAction::RejectResponse;
    return CopernicusResumeAction::AppendVerifiedRange;
}

}
