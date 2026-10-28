#include "copernicus_terrain_provider.h"
#include "copernicus_download_validation.h"
#include "copernicus_resume_state.h"
#include "copernicus_source_publish.h"
#include "copernicus_decoded_block_cache.h"

#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLockFile>
#include <QMutexLocker>
#include <QMetaObject>
#include <QPointer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QSet>
#include <QTimer>
#include <QUrl>

#include <tiffio.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <numbers>
#include <vector>

namespace Aowis::Map
{
namespace
{
constexpr int CopernicusMinimumNormalizedZoom = 8;
constexpr int CopernicusMaximumNormalizedZoom = 14;
constexpr double CopernicusNominalResolutionM = 30.0;
// Timeouts are configured per provider instance to accommodate slow connections.
constexpr int ProviderSourceLockTimeoutMs = 180000;
constexpr int ProviderSourceMinimumStaleLockMs = 10 * 60 * 1000;
constexpr int ProviderSourceLockGraceMs = 60 * 1000;
constexpr qint64 MaximumSourceDownloadBytes = 256LL * 1024LL * 1024LL;
constexpr qsizetype MaximumDecodedRasterBlockCacheBytes = 32LL * 1024LL * 1024LL;

const QString CopernicusDataset = QStringLiteral("copernicus-glo30");
const QString CopernicusBaseUrl = QStringLiteral("https://copernicus-dem-30m.s3.amazonaws.com");

struct CopernicusSourceAddress
{
    int south_deg = 0;
    int west_deg = 0;
};

enum class SourceLoadStatus
{
    Ready,
    Unavailable,
    NetworkError,
    ReadError,
    Cancelled
};

struct SourceLoadResult
{
    SourceLoadStatus status = SourceLoadStatus::Unavailable;
    QString path;
    bool downloaded = false;
    bool source_confirmed_unavailable = false;
    QString error_message;
};

QString latitudeToken(int south_deg)
{
    const QChar hemisphere = south_deg >= 0 ? QChar('N') : QChar('S');
    return QStringLiteral("%1%2_00")
        .arg(hemisphere)
        .arg(std::abs(south_deg), 2, 10, QChar('0'));
}

QString longitudeToken(int west_deg)
{
    const QChar hemisphere = west_deg >= 0 ? QChar('E') : QChar('W');
    return QStringLiteral("%1%2_00")
        .arg(hemisphere)
        .arg(std::abs(west_deg), 3, 10, QChar('0'));
}

QString sourceTileBaseName(const CopernicusSourceAddress &address)
{
    return QStringLiteral("Copernicus_DSM_COG_10_%1_%2_DEM")
        .arg(latitudeToken(address.south_deg), longitudeToken(address.west_deg));
}

QString sourceTileCachePath(const QString &provider_cache_directory,
                            const CopernicusSourceAddress &address)
{
    return QDir(provider_cache_directory)
        .filePath(QStringLiteral("copernicus/glo30/%1.tif").arg(sourceTileBaseName(address)));
}

QUrl sourceTileUrl(const CopernicusSourceAddress &address)
{
    const QString base_name = sourceTileBaseName(address);
    return QUrl(QStringLiteral("%1/%2/%2.tif").arg(CopernicusBaseUrl, base_name));
}

int sourceWestForLongitude(double longitude_deg)
{
    if (longitude_deg >= 180.0)
        return 179;
    if (longitude_deg <= -180.0)
        return -180;
    return int(std::floor(longitude_deg));
}

int sourceSouthForLatitude(double latitude_deg)
{
    if (latitude_deg <= -90.0)
        return -90;
    if (latitude_deg >= 90.0)
        return 89;

    const double rounded = std::round(latitude_deg);
    if (std::abs(latitude_deg - rounded) <= 1e-12)
        return int(rounded) - 1;
    return int(std::floor(latitude_deg));
}

CopernicusSourceAddress sourceAddressForCoordinate(double latitude_deg,
                                                   double longitude_deg)
{
    CopernicusSourceAddress address;
    address.south_deg = sourceSouthForLatitude(latitude_deg);
    address.west_deg = sourceWestForLongitude(longitude_deg);
    return address;
}

QString sourceAddressKey(const CopernicusSourceAddress &address)
{
    return QStringLiteral("%1:%2").arg(address.south_deg).arg(address.west_deg);
}

bool ensureParentDirectory(const QString &path, QString *error_message)
{
    const QFileInfo file_info(path);
    QDir directory;
    if (directory.mkpath(file_info.absolutePath()))
        return true;

    if (error_message != nullptr)
        *error_message = QStringLiteral("Failed to create Copernicus provider cache directory: %1")
                             .arg(file_info.absolutePath());
    return false;
}

SourceLoadResult downloadSourceTile(const QString &provider_cache_directory,
                                    const CopernicusSourceAddress &address,
                                    bool allow_remote_fetch,
                                    QMutex *upstream_mutex,
                                    QSet<QNetworkReply *> *upstream_active_replies,
                                    quint64 *upstream_cancel_generation,
                                    quint64 request_cancel_generation,
                                    int connect_timeout_ms, int inactivity_timeout_ms,
                                    int download_deadline_ms,
                                    const QUrl &source_url = QUrl(),
                                    const std::shared_ptr<std::atomic_bool> &cancel_requested = {})
{
    SourceLoadResult result;
    const std::function<bool()> is_cancelled = [&]()
    {
        return cancel_requested && cancel_requested->load(std::memory_order_acquire);
    };
    if (is_cancelled())
    {
        result.status = SourceLoadStatus::Cancelled;
        result.error_message = QStringLiteral("Copernicus upstream download canceled");
        return result;
    }
    result.path = sourceTileCachePath(provider_cache_directory, address);

    const QFileInfo existing_info(result.path);
    if (existing_info.exists() && existing_info.isFile() && existing_info.isReadable() &&
        existing_info.size() > 0)
    {
        result.status = SourceLoadStatus::Ready;
        return result;
    }

    if (!allow_remote_fetch)
    {
        result.status = SourceLoadStatus::Unavailable;
        result.error_message = QStringLiteral(
            "Copernicus provider-native source tile is not cached and remote fetching is disabled: %1")
                                   .arg(result.path);
        return result;
    }

    if (!ensureParentDirectory(result.path, &result.error_message))
    {
        result.status = SourceLoadStatus::ReadError;
        return result;
    }

    QLockFile source_lock(result.path + QStringLiteral(".lock"));
    // A valid slow transfer can outlast the former fixed ten-minute stale-lock
    // threshold. Do not allow another process to consider its source lock stale
    // before the configured download deadline plus cleanup grace.
    const qint64 source_stale_lock_ms = std::max(
        qint64(ProviderSourceMinimumStaleLockMs),
        qint64(download_deadline_ms) + ProviderSourceLockGraceMs);
    source_lock.setStaleLockTime(int(std::min(
        source_stale_lock_ms, qint64(std::numeric_limits<int>::max()))));
    // Poll the cross-process source lock rather than blocking for up to three
    // minutes without noticing a shutdown/cancellation request. The lock still
    // protects the entire download and cache commit once acquired.
    QElapsedTimer lock_wait;
    lock_wait.start();
    bool source_locked = false;
    while (lock_wait.elapsed() < ProviderSourceLockTimeoutMs)
    {
        if (is_cancelled())
        {
            result.status = SourceLoadStatus::Cancelled;
            result.error_message = QStringLiteral("Copernicus upstream download canceled");
            return result;
        }
        if (upstream_mutex != nullptr && upstream_cancel_generation != nullptr)
        {
            QMutexLocker locker(upstream_mutex);
            if (*upstream_cancel_generation != request_cancel_generation)
            {
                result.status = SourceLoadStatus::Cancelled;
                result.error_message = QStringLiteral("Copernicus upstream download canceled");
                return result;
            }
        }

        const int remaining_ms = ProviderSourceLockTimeoutMs - int(lock_wait.elapsed());
        if (source_lock.tryLock(std::min(remaining_ms, 100)))
        {
            source_locked = true;
            break;
        }
    }
    if (!source_locked)
    {
        result.status = SourceLoadStatus::ReadError;
        result.error_message =
            QStringLiteral("Timed out waiting for Copernicus provider cache fill: %1")
                .arg(result.path);
        return result;
    }

    // Cancellation may have happened during the final lock attempt.
    if (is_cancelled())
    {
        result.status = SourceLoadStatus::Cancelled;
        result.error_message = QStringLiteral("Copernicus upstream download canceled");
        return result;
    }
    if (upstream_mutex != nullptr && upstream_cancel_generation != nullptr)
    {
        QMutexLocker locker(upstream_mutex);
        if (*upstream_cancel_generation != request_cancel_generation)
        {
            result.status = SourceLoadStatus::Cancelled;
            result.error_message = QStringLiteral("Copernicus upstream download canceled");
            return result;
        }
    }

    // Another normalized tile/process may have downloaded the same 1-degree COG while
    // this request waited for the source lock. Recheck before touching the network.
    const QFileInfo locked_existing_info(result.path);
    if (locked_existing_info.exists() && locked_existing_info.isFile() &&
        locked_existing_info.isReadable() && locked_existing_info.size() > 0)
    {
        result.status = SourceLoadStatus::Ready;
        return result;
    }

    // The incoming response is never the published cache entry. A retained
    // partial is usable only together with matching, atomically stored metadata.
    const QString partial_path = result.path + QStringLiteral(".partial");
    const QString metadata_path = result.path + QStringLiteral(".partial.meta");
    const QString incoming_path = result.path + QStringLiteral(".incoming");
    const std::optional<Internal::CopernicusPartialState> prior_state =
        Internal::loadCopernicusPartialState(partial_path, metadata_path,
                                             MaximumSourceDownloadBytes);
    if (!prior_state)
    {
        QFile::remove(partial_path);
        QFile::remove(metadata_path);
    }
    QFile::remove(incoming_path);
    QFile output(incoming_path);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        result.status = SourceLoadStatus::ReadError;
        result.error_message = QStringLiteral("Failed to stage Copernicus source %1: %2")
                                   .arg(incoming_path, output.errorString());
        return result;
    }

    if (upstream_mutex != nullptr && upstream_cancel_generation != nullptr)
    {
        QMutexLocker locker(upstream_mutex);
        if (*upstream_cancel_generation != request_cancel_generation)
        {
            result.status = SourceLoadStatus::Cancelled;
            result.error_message = QStringLiteral("Copernicus upstream download canceled");
            return result;
        }
    }

    if (is_cancelled())
    {
        output.close();
        QFile::remove(incoming_path);
        result.status = SourceLoadStatus::Cancelled;
        result.error_message = QStringLiteral("Copernicus upstream download canceled");
        return result;
    }
    QNetworkAccessManager network_manager;
    QNetworkRequest request(source_url.isEmpty() ? sourceTileUrl(address) : source_url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(inactivity_timeout_ms);
#ifdef Q_OS_WIN
    // Keep the Qt 6.7.0 Windows network path on HTTP/1.1 for the same reason
    // as raster tile fetching: avoid HTTP/2 replies that can remain unfinished
    // until the application-level timeout fires.
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
#endif
    request.setRawHeader("User-Agent", "AOWIS-SERVER-MAP terrain-provider");
    if (prior_state)
    {
        request.setRawHeader("Range", "bytes=" +
            QByteArray::number(prior_state->downloaded_bytes) + "-");
        request.setRawHeader("If-Range", prior_state->strong_etag);
    }

    QNetworkReply *reply = network_manager.get(request);
    if (upstream_mutex != nullptr && upstream_active_replies != nullptr)
    {
        QMutexLocker locker(upstream_mutex);
        upstream_active_replies->insert(reply);
        if (upstream_cancel_generation != nullptr &&
            *upstream_cancel_generation != request_cancel_generation)
        {
            reply->abort();
        }
    }
    QEventLoop event_loop;
    // The token is owned by the single-flight job, not the provider. Poll on
    // the network worker event loop, so only this request is aborted.
    QTimer orphan_poll;
    if (cancel_requested)
    {
        QObject::connect(&orphan_poll, &QTimer::timeout, &event_loop,
                         [reply, cancel_requested]()
        {
            if (cancel_requested->load(std::memory_order_acquire) && !reply->isFinished())
                reply->abort();
        });
        orphan_poll.start(25);
        if (is_cancelled())
            reply->abort();
    }
    QTimer connection_deadline;
    connection_deadline.setSingleShot(true);
    bool connection_expired = false;
    QObject::connect(&connection_deadline, &QTimer::timeout, &event_loop,
                     [&connection_expired, reply]()
    {
        connection_expired = true;
        reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::metaDataChanged, &connection_deadline,
                     &QTimer::stop);
    QObject::connect(reply, &QNetworkReply::downloadProgress, &connection_deadline,
                     &QTimer::stop);
    QTimer download_deadline;
    download_deadline.setSingleShot(true);
    bool deadline_expired = false;
    QObject::connect(&download_deadline, &QTimer::timeout, &event_loop,
                     [&deadline_expired, reply]()
    {
        deadline_expired = true;
        reply->abort();
    });
    bool write_failed = false;
    qint64 downloaded_bytes = 0;

    QObject::connect(reply, &QNetworkReply::readyRead, &event_loop, [&output, reply,
                                                                 &write_failed,
                                                                 &downloaded_bytes]()
    {
        if (write_failed)
        {
            reply->readAll();
            return;
        }

        const QByteArray data = reply->readAll();
        downloaded_bytes += data.size();
        if (downloaded_bytes > MaximumSourceDownloadBytes ||
            output.write(data) != data.size())
        {
            write_failed = true;
            reply->abort();
        }
    });
    QObject::connect(reply, &QNetworkReply::finished, &event_loop, &QEventLoop::quit);
    connection_deadline.start(connect_timeout_ms);
    download_deadline.start(download_deadline_ms);
    if (!reply->isFinished())
        event_loop.exec();
    connection_deadline.stop();
    download_deadline.stop();
    orphan_poll.stop();

    bool canceled = is_cancelled();
    if (upstream_mutex != nullptr)
    {
        QMutexLocker locker(upstream_mutex);
        if (upstream_active_replies != nullptr)
            upstream_active_replies->remove(reply);
        canceled = canceled || (upstream_cancel_generation != nullptr &&
            *upstream_cancel_generation != request_cancel_generation);
    }

    if (!write_failed)
    {
        const QByteArray trailing = reply->readAll();
        downloaded_bytes += trailing.size();
        if (downloaded_bytes > MaximumSourceDownloadBytes ||
            output.write(trailing) != trailing.size())
            write_failed = true;
    }

    const int http_status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QVariant content_length_header = reply->header(QNetworkRequest::ContentLengthHeader);
    bool valid_content_length = false;
    const qint64 content_length = content_length_header.toLongLong(&valid_content_length);
    const QByteArray etag = reply->rawHeader("ETag");
    const QByteArray content_range = reply->rawHeader("Content-Range");
    const QNetworkReply::NetworkError network_error = reply->error();
    const QString network_error_text = reply->errorString();
    const QUrl request_url = reply->url();
    reply->deleteLater();
    output.close();

    const auto discard_incoming = [&]() { QFile::remove(incoming_path); };
    const auto discard_partial = [&]()
    {
        QFile::remove(partial_path);
        QFile::remove(metadata_path);
    };
    if (canceled)
    {
        discard_incoming();
        result.status = SourceLoadStatus::Cancelled;
        result.error_message = QStringLiteral("Copernicus upstream download canceled");
        return result;
    }
    if (write_failed)
    {
        discard_incoming();
        result.status = SourceLoadStatus::ReadError;
        result.error_message = downloaded_bytes > MaximumSourceDownloadBytes
            ? QStringLiteral("Copernicus source tile exceeded the %1 MiB safety limit: %2")
                  .arg(MaximumSourceDownloadBytes / (1024LL * 1024LL))
                  .arg(request_url.toString())
            : QStringLiteral("Failed to stage Copernicus provider source: %1").arg(incoming_path);
        return result;
    }
    if (http_status == 404 || network_error == QNetworkReply::ContentNotFoundError)
    {
        discard_incoming();
        discard_partial();
        result.status = SourceLoadStatus::Unavailable;
        result.source_confirmed_unavailable = true;
        result.error_message = QStringLiteral("Copernicus GLO-30 source tile is unavailable: %1")
                                   .arg(request_url.toString());
        return result;
    }

    // Preserve a failed full response only when the server identifies an
    // immutable entity and the expected size. Interrupted resumptions leave
    // their previously validated prefix untouched.
    if (network_error != QNetworkReply::NoError || connection_expired || deadline_expired)
    {
        if (http_status == 200 && downloaded_bytes > 0 &&
            valid_content_length && content_length > downloaded_bytes &&
            content_length <= MaximumSourceDownloadBytes &&
            Internal::validCopernicusStrongEtag(etag))
        {
            discard_partial();
            if (QFile::rename(incoming_path, partial_path))
            {
                const Internal::CopernicusPartialState state{downloaded_bytes,
                                                              content_length, etag};
                if (!Internal::saveCopernicusPartialState(metadata_path, state,
                                                          MaximumSourceDownloadBytes))
                    discard_partial();
            }
            else
                discard_incoming();
        }
        else if (http_status == 206 && prior_state && downloaded_bytes > 0)
        {
            const std::optional<Internal::CopernicusContentRange> range =
                Internal::parseCopernicusContentRange(content_range);
            const qint64 remaining = prior_state->total_bytes - prior_state->downloaded_bytes;
            if (range && range->first == prior_state->downloaded_bytes &&
                range->total == prior_state->total_bytes &&
                range->last + 1 == range->total &&
                range->last - range->first + 1 == remaining &&
                etag == prior_state->strong_etag &&
                valid_content_length && content_length == remaining &&
                downloaded_bytes < remaining)
            {
                Internal::appendCopernicusPartialSuffix(
                    partial_path, metadata_path, incoming_path, *prior_state,
                    downloaded_bytes, MaximumSourceDownloadBytes);
            }
            discard_incoming();
        }
        else
            discard_incoming();
        result.status = SourceLoadStatus::NetworkError;
        result.error_message = QStringLiteral("Copernicus transfer interrupted: %1 (%2)")
                                   .arg(request_url.toString(), network_error_text);
        return result;
    }

    if (http_status != 200 && http_status != 206)
    {
        discard_incoming();
        if (http_status == 416)
            discard_partial();
        result.status = SourceLoadStatus::NetworkError;
        result.error_message = QStringLiteral("Copernicus upstream rejected request: HTTP %1, %2")
                                   .arg(http_status).arg(request_url.toString());
        return result;
    }

    bool append_prior = false;
    if (http_status == 206 && prior_state)
    {
        append_prior = Internal::classifyCopernicusResumeResponse(
            http_status, prior_state->downloaded_bytes, downloaded_bytes,
            content_range, prior_state->strong_etag, etag,
            MaximumSourceDownloadBytes) == Internal::CopernicusResumeAction::AppendVerifiedRange;
        if (append_prior && prior_state->downloaded_bytes + downloaded_bytes !=
                prior_state->total_bytes)
            append_prior = false;
    }
    const bool complete_full = Internal::completeCopernicusResponse(
        http_status, downloaded_bytes, valid_content_length ? content_length : -1);
    if ((!append_prior && !complete_full) ||
        (append_prior && valid_content_length && content_length != downloaded_bytes))
    {
        discard_incoming();
        if (http_status == 206)
            discard_partial();
        result.status = SourceLoadStatus::NetworkError;
        result.error_message = QStringLiteral("Copernicus returned incomplete or inconsistent source bytes: %1")
                                   .arg(request_url.toString());
        return result;
    }

    if (is_cancelled())
    {
        discard_incoming();
        result.status = SourceLoadStatus::Cancelled;
        result.error_message = QStringLiteral("Copernicus upstream download canceled");
        return result;
    }

    // Publish only after the entire response (and optional verified prefix)
    // has been copied into QSaveFile. A failed commit leaves the old cache
    // entry untouched. Copy in chunks to avoid allocating a whole GeoTIFF.
    if (!Internal::publishCopernicusSource(result.path, incoming_path,
                                            append_prior ? partial_path : QString()))
    {
        discard_incoming();
        result.status = SourceLoadStatus::ReadError;
        result.error_message = QStringLiteral("Failed to publish complete Copernicus source: %1")
                                   .arg(result.path);
        return result;
    }
    discard_incoming();
    discard_partial();
    result.status = SourceLoadStatus::Ready;
    result.downloaded = true;
    result.error_message.clear();
    return result;
}

class CopernicusRaster
{
public:
    explicit CopernicusRaster(const QString &path)
        : path(path)
    {
    }

    ~CopernicusRaster()
    {
        if (this->tiff != nullptr)
            TIFFClose(this->tiff);
    }

    bool open(QString *error_message)
    {
        const QByteArray encoded_path = QFile::encodeName(this->path);
        this->tiff = TIFFOpen(encoded_path.constData(), "r");
        if (this->tiff == nullptr)
        {
            if (error_message != nullptr)
                *error_message = QStringLiteral("Failed to open Copernicus GeoTIFF: %1").arg(this->path);
            return false;
        }

        uint16_t bits_per_sample = 0;
        uint16_t samples_per_pixel = 1;
        uint16_t sample_format = SAMPLEFORMAT_UINT;
        uint16_t planar_config = PLANARCONFIG_CONTIG;
        if (TIFFGetField(this->tiff, TIFFTAG_IMAGEWIDTH, &this->width) != 1 ||
            TIFFGetField(this->tiff, TIFFTAG_IMAGELENGTH, &this->height) != 1 ||
            TIFFGetFieldDefaulted(this->tiff, TIFFTAG_BITSPERSAMPLE, &bits_per_sample) != 1 ||
            TIFFGetFieldDefaulted(this->tiff, TIFFTAG_SAMPLESPERPIXEL, &samples_per_pixel) != 1 ||
            TIFFGetFieldDefaulted(this->tiff, TIFFTAG_SAMPLEFORMAT, &sample_format) != 1 ||
            TIFFGetFieldDefaulted(this->tiff, TIFFTAG_PLANARCONFIG, &planar_config) != 1)
        {
            if (error_message != nullptr)
                *error_message = QStringLiteral("Copernicus GeoTIFF is missing required raster metadata: %1")
                                     .arg(this->path);
            return false;
        }

        if (this->width == 0 || this->height == 0 || bits_per_sample != 32 ||
            samples_per_pixel != 1 || sample_format != SAMPLEFORMAT_IEEEFP ||
            planar_config != PLANARCONFIG_CONTIG)
        {
            if (error_message != nullptr)
            {
                *error_message = QStringLiteral(
                    "Unsupported Copernicus GeoTIFF layout in %1 (width=%2 height=%3 bits=%4 samples=%5 format=%6 planar=%7)")
                                     .arg(this->path)
                                     .arg(this->width)
                                     .arg(this->height)
                                     .arg(bits_per_sample)
                                     .arg(samples_per_pixel)
                                     .arg(sample_format)
                                     .arg(planar_config);
            }
            return false;
        }

        this->tiled = TIFFIsTiled(this->tiff) != 0;
        if (this->tiled)
        {
            if (TIFFGetField(this->tiff, TIFFTAG_TILEWIDTH, &this->tile_width) != 1 ||
                TIFFGetField(this->tiff, TIFFTAG_TILELENGTH, &this->tile_height) != 1 ||
                this->tile_width == 0 || this->tile_height == 0)
            {
                if (error_message != nullptr)
                    *error_message = QStringLiteral("Copernicus tiled GeoTIFF has invalid tile geometry: %1")
                                         .arg(this->path);
                return false;
            }
        }
        else
        {
            if (TIFFGetFieldDefaulted(this->tiff, TIFFTAG_ROWSPERSTRIP,
                                      &this->rows_per_strip) != 1 ||
                this->rows_per_strip == 0)
            {
                if (error_message != nullptr)
                    *error_message = QStringLiteral("Copernicus stripped GeoTIFF has invalid strip geometry: %1")
                                         .arg(this->path);
                return false;
            }
        }

        return true;
    }

    int pixelWidth() const
    {
        return int(this->width);
    }

    int pixelHeight() const
    {
        return int(this->height);
    }

    std::optional<float> pixel(int column, int row, QString *error_message)
    {
        if (column < 0 || row < 0 || column >= int(this->width) || row >= int(this->height))
            return std::nullopt;

        if (this->tiled)
            return tiledPixel(column, row, error_message);
        return strippedPixel(column, row, error_message);
    }

private:
    std::optional<float> tiledPixel(int column, int row, QString *error_message)
    {
        const uint32_t tile_x = uint32_t(column) / this->tile_width * this->tile_width;
        const uint32_t tile_y = uint32_t(row) / this->tile_height * this->tile_height;
        const ttile_t tile_index = TIFFComputeTile(this->tiff, tile_x, tile_y, 0, 0);
        const quint32 cache_key = quint32(tile_index);

        QByteArray uncached_block;
        const QByteArray *block = this->decoded_blocks.find(cache_key);
        if (block == nullptr)
        {
            const tmsize_t tile_bytes = TIFFTileSize(this->tiff);
            if (tile_bytes <= 0 || tile_bytes > MaximumDecodedRasterBlockCacheBytes)
            {
                if (error_message != nullptr)
                    *error_message = QStringLiteral("Copernicus GeoTIFF tile buffer size is invalid: %1")
                                         .arg(this->path);
                return std::nullopt;
            }

            Internal::CopernicusTemporaryDecodeReservation decode_reservation(
                Internal::sharedCopernicusTemporaryDecodeBudget(), qsizetype(tile_bytes));
            if (!decode_reservation.acquired())
            {
                if (error_message != nullptr)
                    *error_message = QStringLiteral(
                        "Copernicus temporary TIFF decode memory budget exhausted: %1")
                                         .arg(this->path);
                return std::nullopt;
            }
            QByteArray decoded(int(tile_bytes), '\0');
            if (TIFFReadEncodedTile(this->tiff, tile_index, decoded.data(), tile_bytes) < 0)
            {
                if (error_message != nullptr)
                    *error_message = QStringLiteral("Failed to decode Copernicus GeoTIFF tile block: %1")
                                         .arg(this->path);
                return std::nullopt;
            }
            if (this->decoded_blocks.insert(cache_key, decoded))
                block = this->decoded_blocks.find(cache_key);
            else
            {
                uncached_block = std::move(decoded);
                block = &uncached_block;
            }
        }

        const int local_column = column - int(tile_x);
        const int local_row = row - int(tile_y);
        const qsizetype byte_offset =
            (qsizetype(local_row) * qsizetype(this->tile_width) + qsizetype(local_column)) *
            qsizetype(sizeof(float));
        return floatAt(*block, byte_offset, error_message);
    }

    std::optional<float> strippedPixel(int column, int row, QString *error_message)
    {
        const tstrip_t strip_index = TIFFComputeStrip(this->tiff, uint32_t(row), 0);
        const quint32 cache_key = quint32(strip_index);

        QByteArray uncached_block;
        const QByteArray *block = this->decoded_blocks.find(cache_key);
        if (block == nullptr)
        {
            const tmsize_t strip_bytes = TIFFStripSize(this->tiff);
            if (strip_bytes <= 0 || strip_bytes > MaximumDecodedRasterBlockCacheBytes)
            {
                if (error_message != nullptr)
                    *error_message = QStringLiteral("Copernicus GeoTIFF strip buffer size is invalid: %1")
                                         .arg(this->path);
                return std::nullopt;
            }

            Internal::CopernicusTemporaryDecodeReservation decode_reservation(
                Internal::sharedCopernicusTemporaryDecodeBudget(), qsizetype(strip_bytes));
            if (!decode_reservation.acquired())
            {
                if (error_message != nullptr)
                    *error_message = QStringLiteral(
                        "Copernicus temporary TIFF decode memory budget exhausted: %1")
                                         .arg(this->path);
                return std::nullopt;
            }
            QByteArray decoded(int(strip_bytes), '\0');
            if (TIFFReadEncodedStrip(this->tiff, strip_index, decoded.data(), strip_bytes) < 0)
            {
                if (error_message != nullptr)
                    *error_message = QStringLiteral("Failed to decode Copernicus GeoTIFF strip: %1")
                                         .arg(this->path);
                return std::nullopt;
            }
            if (this->decoded_blocks.insert(cache_key, decoded))
                block = this->decoded_blocks.find(cache_key);
            else
            {
                uncached_block = std::move(decoded);
                block = &uncached_block;
            }
        }

        const int local_row = row % int(this->rows_per_strip);
        const qsizetype byte_offset =
            (qsizetype(local_row) * qsizetype(this->width) + qsizetype(column)) *
            qsizetype(sizeof(float));
        return floatAt(*block, byte_offset, error_message);
    }

    std::optional<float> floatAt(const QByteArray &decoded, qsizetype byte_offset,
                                 QString *error_message) const
    {
        if (byte_offset < 0 || byte_offset + qsizetype(sizeof(float)) > decoded.size())
        {
            if (error_message != nullptr)
                *error_message = QStringLiteral("Copernicus GeoTIFF decoded block is shorter than expected: %1")
                                     .arg(this->path);
            return std::nullopt;
        }

        float value = 0.0f;
        std::memcpy(&value, decoded.constData() + byte_offset, sizeof(float));
        if (!std::isfinite(value) || value <= -10000.0f || value >= 100000.0f)
            return std::nullopt;
        return value;
    }

    QString path;
    TIFF *tiff = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t tile_width = 0;
    uint32_t tile_height = 0;
    uint32_t rows_per_strip = 0;
    bool tiled = false;
    Internal::CopernicusDecodedBlockCache decoded_blocks{
        MaximumDecodedRasterBlockCacheBytes, &Internal::sharedCopernicusDecodedBlockBudget()};
};

class CopernicusRasterSet
{
public:
    CopernicusRasterSet(const QString &provider_cache_directory, bool allow_remote_fetch,
                        QHash<QString, QDateTime> *unavailable_source_until,
                        QMutex *unavailable_source_mutex, QMutex *upstream_mutex,
                        QSet<QNetworkReply *> *upstream_active_replies,
                        quint64 *upstream_cancel_generation, quint64 request_cancel_generation,
                        int connect_timeout_ms, int inactivity_timeout_ms,
                        int download_deadline_ms)
        : provider_cache_directory(provider_cache_directory),
          allow_remote_fetch(allow_remote_fetch),
          unavailable_source_until(unavailable_source_until),
          unavailable_source_mutex(unavailable_source_mutex),
          upstream_mutex(upstream_mutex),
          upstream_active_replies(upstream_active_replies),
          upstream_cancel_generation(upstream_cancel_generation),
          request_cancel_generation(request_cancel_generation),
          connect_timeout_ms(connect_timeout_ms),
          inactivity_timeout_ms(inactivity_timeout_ms),
          download_deadline_ms(download_deadline_ms)
    {
    }

    std::optional<float> sample(double latitude_deg, double longitude_deg)
    {
        if (this->fatal_status.has_value())
            return std::nullopt;

        const CopernicusSourceAddress base_address =
            sourceAddressForCoordinate(latitude_deg, longitude_deg);
        CopernicusRaster *base_raster = raster(base_address);
        if (base_raster == nullptr)
            return std::nullopt;

        const double pixel_x =
            (longitude_deg - double(base_address.west_deg)) * double(base_raster->pixelWidth());
        const double north_deg = double(base_address.south_deg + 1);
        const double pixel_y =
            (north_deg - latitude_deg) * double(base_raster->pixelHeight());

        const int column0 = std::max(0, int(std::floor(pixel_x)));
        const int row0 = std::max(0, int(std::floor(pixel_y)));
        const int column1 = column0 + 1;
        const int row1 = row0 + 1;
        const double column_fraction = std::clamp(pixel_x - double(column0), 0.0, 1.0);
        const double row_fraction = std::clamp(pixel_y - double(row0), 0.0, 1.0);
        const double weights[4] = {
            (1.0 - column_fraction) * (1.0 - row_fraction),
            column_fraction * (1.0 - row_fraction),
            (1.0 - column_fraction) * row_fraction,
            column_fraction * row_fraction
        };
        const int columns[4] = { column0, column1, column0, column1 };
        const int rows[4] = { row0, row0, row1, row1 };

        double weighted_value = 0.0;
        for (int index = 0; index < 4; ++index)
        {
            if (weights[index] <= std::numeric_limits<double>::epsilon())
                continue;

            const std::optional<float> value = sourceGridValue(
                base_address, base_raster->pixelWidth(), base_raster->pixelHeight(),
                columns[index], rows[index]);
            if (!value.has_value())
                return std::nullopt;
            weighted_value += weights[index] * double(value.value());
        }

        if (!std::isfinite(weighted_value))
            return std::nullopt;
        return float(weighted_value);
    }

    // Sample the original GeoTIFF pixel containing this coordinate without
    // bilinear interpolation. Conditioning/storage analyses must not operate
    // on elevations interpolated by the renderer's terrain normalization.
    std::optional<float> sampleNative(double latitude_deg, double longitude_deg)
    {
        if (this->fatal_status.has_value())
            return std::nullopt;
        const CopernicusSourceAddress address =
            sourceAddressForCoordinate(latitude_deg, longitude_deg);
        CopernicusRaster *source_raster = raster(address);
        if (source_raster == nullptr)
            return std::nullopt;
        const int column = std::clamp(
            int(std::floor((longitude_deg - double(address.west_deg)) *
                           double(source_raster->pixelWidth()))), 0,
            source_raster->pixelWidth() - 1);
        const int row = std::clamp(
            int(std::floor((double(address.south_deg + 1) - latitude_deg) *
                           double(source_raster->pixelHeight()))), 0,
            source_raster->pixelHeight() - 1);
        QString pixel_error;
        const std::optional<float> value = source_raster->pixel(column, row, &pixel_error);
        if (!value.has_value() && !pixel_error.isEmpty())
        {
            this->fatal_status = TerrainProviderFetchStatus::SourceReadError;
            this->fatal_error_message = pixel_error;
        }
        return value;
    }

    std::optional<TerrainProviderFetchStatus> fatalStatus() const
    {
        return this->fatal_status;
    }

    QString fatalErrorMessage() const
    {
        return this->fatal_error_message;
    }

    bool downloadedAnySource() const
    {
        return this->downloaded_any_source;
    }

private:
    CopernicusRaster *raster(const CopernicusSourceAddress &address)
    {
        const QString key = sourceAddressKey(address);
        if (this->unavailable_sources.contains(key))
            return nullptr;
        if (this->unavailable_source_until != nullptr &&
            this->unavailable_source_mutex != nullptr)
        {
            QMutexLocker unavailable_locker(this->unavailable_source_mutex);
            if (this->unavailable_source_until->contains(key))
            {
                const QDateTime retry_after = this->unavailable_source_until->value(key);
                if (QDateTime::currentDateTimeUtc() < retry_after)
                {
                    this->unavailable_sources.insert(key);
                    return nullptr;
                }
                this->unavailable_source_until->remove(key);
            }
        }
        if (this->rasters.contains(key))
            return this->rasters.value(key);

        const SourceLoadResult source_result =
            downloadSourceTile(this->provider_cache_directory, address,
                               this->allow_remote_fetch, this->upstream_mutex,
                               this->upstream_active_replies,
                               this->upstream_cancel_generation,
                               this->request_cancel_generation,
                               this->connect_timeout_ms, this->inactivity_timeout_ms,
                               this->download_deadline_ms);
        if (source_result.status == SourceLoadStatus::Unavailable)
        {
            this->unavailable_sources.insert(key);
            if (source_result.source_confirmed_unavailable &&
                this->unavailable_source_until != nullptr &&
                this->unavailable_source_mutex != nullptr)
            {
                QMutexLocker unavailable_locker(this->unavailable_source_mutex);
                this->unavailable_source_until->insert(
                    key, QDateTime::currentDateTimeUtc().addSecs(3600));
            }
            return nullptr;
        }
        if (source_result.status == SourceLoadStatus::Cancelled)
        {
            this->fatal_status = TerrainProviderFetchStatus::Cancelled;
            this->fatal_error_message = source_result.error_message;
            return nullptr;
        }
        if (source_result.status == SourceLoadStatus::NetworkError)
        {
            this->fatal_status = TerrainProviderFetchStatus::NetworkError;
            this->fatal_error_message = source_result.error_message;
            return nullptr;
        }
        if (source_result.status == SourceLoadStatus::ReadError)
        {
            this->fatal_status = TerrainProviderFetchStatus::SourceReadError;
            this->fatal_error_message = source_result.error_message;
            return nullptr;
        }

        if (source_result.downloaded)
            this->downloaded_any_source = true;

        std::unique_ptr<CopernicusRaster> source_raster =
            std::make_unique<CopernicusRaster>(source_result.path);
        QString open_error;
        if (!source_raster->open(&open_error))
        {
            this->fatal_status = TerrainProviderFetchStatus::SourceReadError;
            this->fatal_error_message = open_error;
            return nullptr;
        }

        CopernicusRaster *result = source_raster.get();
        this->owned_rasters.push_back(std::move(source_raster));
        this->rasters.insert(key, result);
        return result;
    }

    std::optional<float> sourceGridValue(const CopernicusSourceAddress &base_address,
                                         int base_width, int base_height,
                                         int column, int row)
    {
        const double longitude_deg = double(base_address.west_deg) +
                                     double(column) / double(base_width);
        const double latitude_deg = double(base_address.south_deg + 1) -
                                    double(row) / double(base_height);
        const CopernicusSourceAddress address =
            sourceAddressForCoordinate(latitude_deg, longitude_deg);
        CopernicusRaster *source_raster = raster(address);
        if (source_raster == nullptr)
            return std::nullopt;

        const double source_column =
            (longitude_deg - double(address.west_deg)) * double(source_raster->pixelWidth());
        const double source_row =
            (double(address.south_deg + 1) - latitude_deg) * double(source_raster->pixelHeight());
        const int resolved_column = std::clamp(
            int(std::llround(source_column)), 0, source_raster->pixelWidth() - 1);
        const int resolved_row = std::clamp(
            int(std::llround(source_row)), 0, source_raster->pixelHeight() - 1);

        QString pixel_error;
        const std::optional<float> value =
            source_raster->pixel(resolved_column, resolved_row, &pixel_error);
        if (!value.has_value() && !pixel_error.isEmpty())
        {
            this->fatal_status = TerrainProviderFetchStatus::SourceReadError;
            this->fatal_error_message = pixel_error;
        }
        return value;
    }

    QString provider_cache_directory;
    bool allow_remote_fetch = false;
    bool downloaded_any_source = false;
    QHash<QString, QDateTime> *unavailable_source_until = nullptr;
    QMutex *unavailable_source_mutex = nullptr;
    QMutex *upstream_mutex = nullptr;
    QSet<QNetworkReply *> *upstream_active_replies = nullptr;
    quint64 *upstream_cancel_generation = nullptr;
    quint64 request_cancel_generation = 0;
    int connect_timeout_ms = 30000;
    int inactivity_timeout_ms = 120000;
    int download_deadline_ms = 1800000;
    QHash<QString, CopernicusRaster *> rasters;
    std::vector<std::unique_ptr<CopernicusRaster>> owned_rasters;
    QSet<QString> unavailable_sources;
    std::optional<TerrainProviderFetchStatus> fatal_status;
    QString fatal_error_message;
};

void coordinateForNormalizedGridSample(const TerrainTileAddress &address,
                                       int column, int row,
                                       double *latitude_deg, double *longitude_deg)
{
    const double tile_count = std::ldexp(1.0, address.zoom);
    const double tile_x = double(address.x) +
                          double(column) / double(TerrainTileCellCount);
    const double tile_y = double(address.y) +
                          double(row) / double(TerrainTileCellCount);

    *longitude_deg = tile_x / tile_count * 360.0 - 180.0;
    const double mercator_y = std::numbers::pi * (1.0 - 2.0 * tile_y / tile_count);
    *latitude_deg = std::atan(std::sinh(mercator_y)) * 180.0 / std::numbers::pi;
}
}


namespace Internal
{
bool fetchCopernicusSourceForIntegrationTest(const QString &directory,
                                             const QUrl &source_url,
                                             QString *error_message,
                                             int connect_timeout_ms,
                                             int inactivity_timeout_ms,
                                             int download_deadline_ms,
                                             int cancel_after_ms)
{
    const CopernicusSourceAddress address{50, 8};
    QMutex upstream_mutex;
    QSet<QNetworkReply *> active_replies;
    quint64 cancel_generation = 0;
    QTimer cancellation_timer;
    cancellation_timer.setSingleShot(true);
    if (cancel_after_ms >= 0)
    {
        QObject::connect(&cancellation_timer, &QTimer::timeout,
                         &cancellation_timer, [&]()
        {
            QMutexLocker locker(&upstream_mutex);
            ++cancel_generation;
            for (QNetworkReply *raw_reply : active_replies)
            {
                QPointer<QNetworkReply> reply(raw_reply);
                QMetaObject::invokeMethod(raw_reply, [reply]()
                {
                    if (!reply.isNull() && !reply->isFinished())
                        reply->abort();
                }, Qt::QueuedConnection);
            }
        });
        cancellation_timer.start(cancel_after_ms);
    }
    const SourceLoadResult result = downloadSourceTile(directory, address, true,
        &upstream_mutex, &active_replies, &cancel_generation, 0,
        connect_timeout_ms, inactivity_timeout_ms,
        download_deadline_ms, source_url);
    cancellation_timer.stop();
    if (error_message != nullptr)
        *error_message = result.error_message;
    return result.status == SourceLoadStatus::Ready;
}
}

CopernicusTerrainProvider::CopernicusTerrainProvider(
    int connect_timeout_seconds, int inactivity_timeout_seconds,
    int download_deadline_seconds, const QUrl &integration_source_url)
    : connect_timeout_ms(std::max(1, connect_timeout_seconds) * 1000),
      inactivity_timeout_ms(std::max(1, inactivity_timeout_seconds) * 1000),
      download_deadline_ms(std::max(1, download_deadline_seconds) * 1000),
      integration_source_url(integration_source_url)
{
}

QString CopernicusTerrainProvider::providerId() const
{
    return QStringLiteral("copernicus");
}

QVector<TerrainDatasetInfo> CopernicusTerrainProvider::datasets() const
{
    TerrainDatasetInfo info;
    info.dataset = CopernicusDataset;
    info.provider = providerId();
    info.nominal_resolution_m = CopernicusNominalResolutionM;
    info.vertical_datum = TerrainVerticalDatum::Egm2008;
    // Copernicus DEM is a surface model: vegetation and buildings included.
    info.surface_model = TerrainSurfaceModel::Dsm;
    info.minimum_zoom = CopernicusMinimumNormalizedZoom;
    // Zoom 14 spaces the 65x65 grid at about the 30 m source scale
    // (docs/terrain-providers.md), so it is the native detail.
    info.maximum_zoom = CopernicusMaximumNormalizedZoom;
    return QVector<TerrainDatasetInfo>{info};
}

bool CopernicusTerrainProvider::supportsDataset(const QString &dataset) const
{
    return dataset == CopernicusDataset;
}

bool CopernicusTerrainProvider::supportsAddress(const QString &dataset,
                                                const TerrainTileAddress &address) const
{
    return supportsDataset(dataset) && isValidTerrainTileAddress(address) &&
           address.zoom >= CopernicusMinimumNormalizedZoom &&
           address.zoom <= CopernicusMaximumNormalizedZoom;
}

TerrainProviderFetchResult CopernicusTerrainProvider::acquireTileSources(
    const QString &dataset, const TerrainTileAddress &address,
    const QString &provider_cache_directory, bool allow_remote_fetch,
    const std::shared_ptr<std::atomic_bool> &cancel_requested)
{
    TerrainProviderFetchResult result;
    result.status = TerrainProviderFetchStatus::Ready;
    if (cancel_requested && cancel_requested->load(std::memory_order_acquire))
    {
        result.status = TerrainProviderFetchStatus::Cancelled;
        return result;
    }
    if (!supportsAddress(dataset, address) || !allow_remote_fetch)
        return result;

    quint64 request_generation = 0;
    {
        QMutexLocker locker(&this->upstream_mutex);
        request_generation = this->upstream_cancel_generation;
    }
    // The tile spans a monotonic geographic rectangle. A one-pixel margin
    // conservatively includes neighbouring source cells for interpolation.
    double north_deg = 0.0;
    double west_deg = 0.0;
    double south_deg = 0.0;
    double east_deg = 0.0;
    coordinateForNormalizedGridSample(address, 0, 0, &north_deg, &west_deg);
    coordinateForNormalizedGridSample(address, TerrainTileCellCount,
                                      TerrainTileCellCount, &south_deg, &east_deg);
    constexpr double pixel_margin_deg = 1.0 / 3600.0;
    QHash<QString, CopernicusSourceAddress> source_addresses;
    const int south = sourceSouthForLatitude(
        std::max(-90.0, south_deg - pixel_margin_deg));
    const int north = sourceSouthForLatitude(
        std::min(90.0, north_deg + pixel_margin_deg));
    const int west = sourceWestForLongitude(
        std::max(-180.0, west_deg - pixel_margin_deg));
    const int east = sourceWestForLongitude(
        std::min(180.0, east_deg + pixel_margin_deg));
    for (int latitude = south; latitude <= north; ++latitude)
    {
        for (int longitude = west; longitude <= east; ++longitude)
        {
            const CopernicusSourceAddress source { latitude, longitude };
            source_addresses.insert(sourceAddressKey(source), source);
        }
    }
    for (const CopernicusSourceAddress &source : std::as_const(source_addresses))
    {
        const SourceLoadResult acquired = downloadSourceTile(
            provider_cache_directory, source, true, &this->upstream_mutex,
            &this->upstream_active_replies, &this->upstream_cancel_generation,
            request_generation, this->connect_timeout_ms, this->inactivity_timeout_ms,
            this->download_deadline_ms, this->integration_source_url, cancel_requested);
        if (acquired.status == SourceLoadStatus::Cancelled ||
            acquired.status == SourceLoadStatus::NetworkError ||
            acquired.status == SourceLoadStatus::ReadError)
        {
            result.status = acquired.status == SourceLoadStatus::Cancelled
                ? TerrainProviderFetchStatus::Cancelled
                : acquired.status == SourceLoadStatus::NetworkError
                    ? TerrainProviderFetchStatus::NetworkError
                    : TerrainProviderFetchStatus::SourceReadError;
            result.error_message = acquired.error_message;
            return result;
        }
        // Missing upstream cells are allowed: the CPU stage produces NoData
        // where necessary, just as the original synchronous provider did.
    }
    return result;
}

TerrainProviderFetchResult CopernicusTerrainProvider::fetchTile(
    const QString &dataset, const TerrainTileAddress &address,
    const QString &provider_cache_directory, bool allow_remote_fetch)
{
    TerrainProviderFetchResult result;
    if (!supportsDataset(dataset))
    {
        result.status = TerrainProviderFetchStatus::UnsupportedDataset;
        result.error_message = QStringLiteral("Copernicus provider does not support dataset: %1")
                                   .arg(dataset);
        return result;
    }
    if (!supportsAddress(dataset, address))
    {
        result.status = TerrainProviderFetchStatus::UnsupportedAddress;
        result.error_message = QStringLiteral(
            "Copernicus GLO-30 normalization supports XYZ zoom levels %1 through %2")
                                   .arg(CopernicusMinimumNormalizedZoom)
                                   .arg(CopernicusMaximumNormalizedZoom);
        return result;
    }

    quint64 request_cancel_generation = 0;
    {
        QMutexLocker locker(&this->upstream_mutex);
        request_cancel_generation = this->upstream_cancel_generation;
    }

    CopernicusRasterSet raster_set(provider_cache_directory, allow_remote_fetch,
                                    &this->unavailable_source_until,
                                    &this->unavailable_source_mutex,
                                    &this->upstream_mutex,
                                    &this->upstream_active_replies,
                                    &this->upstream_cancel_generation,
                                    request_cancel_generation,
                                    this->connect_timeout_ms, this->inactivity_timeout_ms,
                                    this->download_deadline_ms);
    TerrainTile tile;
    tile.address = address;
    tile.dataset = dataset;
    tile.nominal_resolution_m = CopernicusNominalResolutionM;
    tile.vertical_datum = TerrainVerticalDatum::Egm2008;
    tile.elevations_m.resize(TerrainTileSampleCount);

    int finite_samples = 0;
    for (int row = 0; row < TerrainTileGridSize; ++row)
    {
        for (int column = 0; column < TerrainTileGridSize; ++column)
        {
            double latitude_deg = 0.0;
            double longitude_deg = 0.0;
            coordinateForNormalizedGridSample(address, column, row,
                                              &latitude_deg, &longitude_deg);
            const std::optional<float> elevation_m =
                raster_set.sample(latitude_deg, longitude_deg);

            const int index = row * TerrainTileGridSize + column;
            if (elevation_m.has_value())
            {
                tile.elevations_m[index] = elevation_m.value();
                ++finite_samples;
            }
            else
            {
                tile.elevations_m[index] = std::numeric_limits<float>::quiet_NaN();
            }

            if (raster_set.fatalStatus().has_value())
            {
                result.status = raster_set.fatalStatus().value();
                result.error_message = raster_set.fatalErrorMessage();
                return result;
            }
        }
    }

    if (finite_samples == 0)
    {
        result.status = TerrainProviderFetchStatus::SourceUnavailable;
        result.error_message = QStringLiteral(
            "Copernicus GLO-30 has no public source data for normalized tile %1/%2/%3")
                                   .arg(address.zoom)
                                   .arg(address.x)
                                   .arg(address.y);
        return result;
    }

    QString validation_error;
    if (!isValidTerrainTile(tile, &validation_error))
    {
        result.status = TerrainProviderFetchStatus::ConversionError;
        result.error_message = QStringLiteral("Copernicus normalized terrain tile is invalid: %1")
                                   .arg(validation_error);
        return result;
    }

    result.status = TerrainProviderFetchStatus::Ready;
    result.tile = tile;
    result.origin = raster_set.downloadedAnySource()
        ? TerrainDataOrigin::Remote
        : TerrainDataOrigin::Cache;
    return result;
}

TerrainProviderFetchResult CopernicusTerrainProvider::acquireGridSources(
    const TerrainAnalysisGridRequest &request,
    const QString &provider_cache_directory, bool allow_remote_fetch,
    const std::shared_ptr<std::atomic_bool> &cancel_requested)
{
    TerrainProviderFetchResult result;
    result.status = TerrainProviderFetchStatus::Ready;
    if (cancel_requested && cancel_requested->load(std::memory_order_acquire))
    {
        result.status = TerrainProviderFetchStatus::Cancelled;
        return result;
    }
    if (!allow_remote_fetch)
        return result;
    if (!supportsDataset(request.dataset))
    {
        result.status = TerrainProviderFetchStatus::UnsupportedDataset;
        result.error_message = QStringLiteral("Unknown Copernicus terrain dataset");
        return result;
    }
    const TerrainGeographicBox &box = request.bounds;
    if (request.columns < 1 || request.columns > 512 ||
        request.rows < 1 || request.rows > 512 ||
        !std::isfinite(box.west_deg) || !std::isfinite(box.east_deg) ||
        !std::isfinite(box.south_deg) || !std::isfinite(box.north_deg) ||
        box.west_deg < -180.0 || box.east_deg > 180.0 ||
        box.south_deg < -85.0 || box.north_deg > 85.0 ||
        box.west_deg >= box.east_deg || box.south_deg >= box.north_deg)
    {
        result.status = TerrainProviderFetchStatus::UnsupportedAddress;
        result.error_message = QStringLiteral("Invalid source raster window (1..512 cells per axis)");
        return result;
    }
    quint64 request_generation = 0;
    {
        QMutexLocker locker(&this->upstream_mutex);
        request_generation = this->upstream_cancel_generation;
    }
    // Match fetchAnalysisGrid's native sample centres exactly. A grid may
    // be sparse across a large box; downloading the entire box would consume
    // vastly more bandwidth than the actual computation requires.
    QHash<QString, CopernicusSourceAddress> source_addresses;
    const double longitude_step = (box.east_deg - box.west_deg) / double(request.columns);
    const double latitude_step = (box.north_deg - box.south_deg) / double(request.rows);
    for (int row = 0; row < request.rows; ++row)
    {
        const double latitude = box.north_deg - (double(row) + 0.5) * latitude_step;
        for (int column = 0; column < request.columns; ++column)
        {
            const double longitude = box.west_deg + (double(column) + 0.5) * longitude_step;
            const CopernicusSourceAddress source = sourceAddressForCoordinate(latitude, longitude);
            source_addresses.insert(sourceAddressKey(source), source);
        }
    }
    for (const CopernicusSourceAddress &source : std::as_const(source_addresses))
    {
        const SourceLoadResult acquired = downloadSourceTile(
            provider_cache_directory, source, true, &this->upstream_mutex,
            &this->upstream_active_replies, &this->upstream_cancel_generation,
            request_generation, this->connect_timeout_ms, this->inactivity_timeout_ms,
            this->download_deadline_ms, QUrl(), cancel_requested);
        if (acquired.status == SourceLoadStatus::Cancelled ||
            acquired.status == SourceLoadStatus::NetworkError ||
            acquired.status == SourceLoadStatus::ReadError)
        {
            result.status = acquired.status == SourceLoadStatus::Cancelled
                ? TerrainProviderFetchStatus::Cancelled
                : acquired.status == SourceLoadStatus::NetworkError
                    ? TerrainProviderFetchStatus::NetworkError
                    : TerrainProviderFetchStatus::SourceReadError;
            result.error_message = acquired.error_message;
            return result;
        }
        // Unavailable source cells are represented as NaN by the CPU stage.
    }
    return result;
}

TerrainAnalysisGridResult CopernicusTerrainProvider::fetchAnalysisGrid(
    const TerrainAnalysisGridRequest &request,
    const QString &provider_cache_directory, bool allow_remote_fetch)
{
    TerrainAnalysisGridResult result;
    const TerrainGeographicBox &box = request.bounds;
    if (!supportsDataset(request.dataset))
    {
        result.status = TerrainProviderFetchStatus::UnsupportedDataset;
        result.error_message = QStringLiteral("Unknown Copernicus terrain dataset");
        return result;
    }
    if (request.columns < 1 || request.columns > 512 ||
        request.rows < 1 || request.rows > 512 ||
        !std::isfinite(box.west_deg) || !std::isfinite(box.east_deg) ||
        !std::isfinite(box.south_deg) || !std::isfinite(box.north_deg) ||
        box.west_deg < -180.0 || box.east_deg > 180.0 ||
        box.south_deg < -85.0 || box.north_deg > 85.0 ||
        box.west_deg >= box.east_deg || box.south_deg >= box.north_deg)
    {
        result.status = TerrainProviderFetchStatus::UnsupportedAddress;
        result.error_message = QStringLiteral("Invalid source raster window (1..512 cells per axis)");
        return result;
    }
    quint64 request_generation = 0;
    {
        QMutexLocker locker(&this->upstream_mutex);
        request_generation = this->upstream_cancel_generation;
    }
    CopernicusRasterSet rasters(provider_cache_directory, allow_remote_fetch,
                               &this->unavailable_source_until,
                               &this->unavailable_source_mutex,
                               &this->upstream_mutex,
                               &this->upstream_active_replies,
                               &this->upstream_cancel_generation,
                               request_generation,
                               this->connect_timeout_ms, this->inactivity_timeout_ms,
                               this->download_deadline_ms);
    const double longitude_step = (box.east_deg - box.west_deg) / double(request.columns);
    const double latitude_step = (box.north_deg - box.south_deg) / double(request.rows);
    result.elevations_m.resize(request.columns * request.rows);
    std::size_t valid_cells = 0;
    for (int row = 0; row < request.rows; ++row)
    {
        const double latitude = box.north_deg - (double(row) + 0.5) * latitude_step;
        for (int column = 0; column < request.columns; ++column)
        {
            const double longitude = box.west_deg + (double(column) + 0.5) * longitude_step;
            const std::optional<float> elevation = rasters.sampleNative(latitude, longitude);
            result.elevations_m[row * request.columns + column] = elevation.value_or(
                std::numeric_limits<float>::quiet_NaN());
            if (elevation.has_value())
                ++valid_cells;
            if (rasters.fatalStatus().has_value())
            {
                result.status = rasters.fatalStatus().value();
                result.error_message = rasters.fatalErrorMessage();
                result.elevations_m.clear();
                return result;
            }
        }
    }
    if (valid_cells == 0)
    {
        result.status = TerrainProviderFetchStatus::SourceUnavailable;
        result.error_message = QStringLiteral("No Copernicus DEM coverage for the requested window");
        result.elevations_m.clear();
        return result;
    }
    result.status = TerrainProviderFetchStatus::Ready;
    result.vertical_datum = TerrainVerticalDatum::Egm2008;
    result.origin = rasters.downloadedAnySource()
        ? TerrainDataOrigin::Remote : TerrainDataOrigin::Cache;
    return result;
}

TerrainUpstreamActivity CopernicusTerrainProvider::upstreamActivity() const
{
    QMutexLocker locker(&this->upstream_mutex);
    TerrainUpstreamActivity activity;
    activity.active = this->upstream_active_replies.size();
    return activity;
}

void CopernicusTerrainProvider::cancelUpstreamDownloads()
{
    QMutexLocker locker(&this->upstream_mutex);
    ++this->upstream_cancel_generation;
    for (QNetworkReply *raw_reply : this->upstream_active_replies)
    {
        QPointer<QNetworkReply> reply(raw_reply);
        QMetaObject::invokeMethod(raw_reply, [reply]()
        {
            if (!reply.isNull() && !reply->isFinished())
                reply->abort();
        }, Qt::QueuedConnection);
    }
}

TerrainSourceDeletionResult CopernicusTerrainProvider::deleteSourceData(
    const QString &dataset, const TerrainGeographicBox &box,
    const QString &provider_cache_directory)
{
    TerrainSourceDeletionResult result;
    if (!supportsDataset(dataset))
    {
        result.ok = false;
        result.error_message = QStringLiteral("Copernicus provider does not support dataset: %1")
                                   .arg(dataset);
        return result;
    }
    if (!std::isfinite(box.west_deg) || !std::isfinite(box.east_deg) ||
        !std::isfinite(box.south_deg) || !std::isfinite(box.north_deg) ||
        box.east_deg <= box.west_deg || box.north_deg <= box.south_deg ||
        provider_cache_directory.trimmed().isEmpty())
    {
        result.ok = false;
        result.error_message = QStringLiteral("Invalid Copernicus source deletion area");
        return result;
    }

    // The 1 x 1 degree source cells with a part of positive area inside the
    // box. A box edge exactly on a cell border does not take the neighbour
    // cell: floor() of the south/west edge, ceil() - 1 of the north/east one.
    const int south_first = std::clamp(int(std::floor(box.south_deg)), -90, 89);
    const int south_last = std::clamp(int(std::ceil(box.north_deg)) - 1, -90, 89);
    int west_first = int(std::floor(box.west_deg));
    int west_last = int(std::ceil(box.east_deg)) - 1;
    if (west_last - west_first + 1 >= 360)
    {
        west_first = -180;
        west_last = 179;
    }

    for (int south_deg = south_first; south_deg <= south_last; ++south_deg)
    {
        for (int unwrapped_west_deg = west_first; unwrapped_west_deg <= west_last; ++unwrapped_west_deg)
        {
            // Unwrapped longitudes (antimeridian boxes) back into -180..179.
            int west_deg = (unwrapped_west_deg + 180) % 360;
            if (west_deg < 0)
                west_deg += 360;
            west_deg -= 180;

            CopernicusSourceAddress address;
            address.south_deg = south_deg;
            address.west_deg = west_deg;

            // A confirmed-unavailable cell (ocean, no public tile) is asked
            // for again after the deletion instead of after its hour.
            {
                QMutexLocker unavailable_locker(&this->unavailable_source_mutex);
                this->unavailable_source_until.remove(sourceAddressKey(address));
            }

            const QString path = sourceTileCachePath(provider_cache_directory, address);
            if (!QFileInfo::exists(path))
                continue;

            // downloadSourceTile() holds this lock while it checks for and
            // fills the file. Held longer than a moment means a download is
            // running: that file is new data, keep it.
            QLockFile source_lock(path + QStringLiteral(".lock"));
            // A valid slow transfer can outlast the former fixed ten-minute stale-lock
    // threshold. Do not allow another process to consider its source lock stale
    // before the configured download deadline plus cleanup grace.
    const qint64 source_stale_lock_ms = std::max(
        qint64(ProviderSourceMinimumStaleLockMs),
        qint64(download_deadline_ms) + ProviderSourceLockGraceMs);
    source_lock.setStaleLockTime(int(std::min(
        source_stale_lock_ms, qint64(std::numeric_limits<int>::max()))));
            if (!source_lock.tryLock(1000))
                continue;

            if (QFile::remove(path))
            {
                ++result.deleted_count;
                continue;
            }

            // Windows: a raster another request is normalizing from right now
            // is open and cannot be removed. Report it, carry on with the rest.
            result.ok = false;
            if (result.error_message.isEmpty())
            {
                result.error_message =
                    QStringLiteral("Failed to delete Copernicus provider cache file: %1").arg(path);
            }
        }
    }

    return result;
}

} // namespace Aowis::Map
