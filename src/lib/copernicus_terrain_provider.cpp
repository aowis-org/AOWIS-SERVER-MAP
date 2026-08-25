#include "copernicus_terrain_provider.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLockFile>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QSet>
#include <QUrl>

#include <tiffio.h>

#include <algorithm>
#include <cmath>
#include <cstring>
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
constexpr int RemoteTransferTimeoutMs = 120000;
constexpr int ProviderSourceLockTimeoutMs = 180000;
constexpr int ProviderSourceStaleLockMs = 10 * 60 * 1000;
constexpr qint64 MaximumSourceDownloadBytes = 256LL * 1024LL * 1024LL;

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
    ReadError
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
                                    bool allow_remote_fetch)
{
    SourceLoadResult result;
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
    source_lock.setStaleLockTime(ProviderSourceStaleLockMs);
    if (!source_lock.tryLock(ProviderSourceLockTimeoutMs))
    {
        result.status = SourceLoadStatus::ReadError;
        result.error_message =
            QStringLiteral("Timed out waiting for Copernicus provider cache fill: %1")
                .arg(result.path);
        return result;
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

    QSaveFile output(result.path);
    if (!output.open(QIODevice::WriteOnly))
    {
        result.status = SourceLoadStatus::ReadError;
        result.error_message = QStringLiteral("Failed to create Copernicus provider cache file %1: %2")
                                   .arg(result.path, output.errorString());
        return result;
    }

    QNetworkAccessManager network_manager;
    QNetworkRequest request(sourceTileUrl(address));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(RemoteTransferTimeoutMs);
    request.setRawHeader("User-Agent", "AOWIS-SERVER-MAP terrain-provider");

    QNetworkReply *reply = network_manager.get(request);
    QEventLoop event_loop;
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
    event_loop.exec();

    if (!write_failed)
    {
        const QByteArray remaining_data = reply->readAll();
        downloaded_bytes += remaining_data.size();
        if (downloaded_bytes > MaximumSourceDownloadBytes ||
            output.write(remaining_data) != remaining_data.size())
        {
            write_failed = true;
        }
    }

    const int http_status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError network_error = reply->error();
    const QString network_error_text = reply->errorString();
    const QUrl request_url = reply->url();
    reply->deleteLater();

    if (write_failed)
    {
        output.cancelWriting();
        result.status = SourceLoadStatus::ReadError;
        result.error_message = downloaded_bytes > MaximumSourceDownloadBytes
            ? QStringLiteral("Copernicus source tile exceeded the %1 MiB safety limit: %2")
                  .arg(MaximumSourceDownloadBytes / (1024LL * 1024LL))
                  .arg(request_url.toString())
            : QStringLiteral("Failed to write Copernicus provider cache file: %1")
                  .arg(result.path);
        return result;
    }

    if (http_status == 404 || network_error == QNetworkReply::ContentNotFoundError)
    {
        output.cancelWriting();
        result.status = SourceLoadStatus::Unavailable;
        result.source_confirmed_unavailable = true;
        result.error_message = QStringLiteral("Copernicus GLO-30 source tile is unavailable: %1")
                                   .arg(request_url.toString());
        return result;
    }

    if (network_error != QNetworkReply::NoError || http_status < 200 || http_status >= 300)
    {
        output.cancelWriting();
        result.status = SourceLoadStatus::NetworkError;
        result.error_message = QStringLiteral("Failed to download Copernicus GLO-30 source tile %1: HTTP %2, %3")
                                   .arg(request_url.toString())
                                   .arg(http_status)
                                   .arg(network_error_text);
        return result;
    }

    if (!output.commit())
    {
        result.status = SourceLoadStatus::ReadError;
        result.error_message = QStringLiteral("Failed to commit Copernicus provider cache file %1: %2")
                                   .arg(result.path, output.errorString());
        return result;
    }

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

        if (!this->decoded_blocks.contains(cache_key))
        {
            const tmsize_t tile_bytes = TIFFTileSize(this->tiff);
            if (tile_bytes <= 0 || tile_bytes > std::numeric_limits<int>::max())
            {
                if (error_message != nullptr)
                    *error_message = QStringLiteral("Copernicus GeoTIFF tile buffer size is invalid: %1")
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
            this->decoded_blocks.insert(cache_key, decoded);
        }

        const QByteArray &decoded = this->decoded_blocks[cache_key];
        const int local_column = column - int(tile_x);
        const int local_row = row - int(tile_y);
        const qsizetype byte_offset =
            (qsizetype(local_row) * qsizetype(this->tile_width) + qsizetype(local_column)) *
            qsizetype(sizeof(float));
        return floatAt(decoded, byte_offset, error_message);
    }

    std::optional<float> strippedPixel(int column, int row, QString *error_message)
    {
        const tstrip_t strip_index = TIFFComputeStrip(this->tiff, uint32_t(row), 0);
        const quint32 cache_key = quint32(strip_index);

        if (!this->decoded_blocks.contains(cache_key))
        {
            const tmsize_t strip_bytes = TIFFStripSize(this->tiff);
            if (strip_bytes <= 0 || strip_bytes > std::numeric_limits<int>::max())
            {
                if (error_message != nullptr)
                    *error_message = QStringLiteral("Copernicus GeoTIFF strip buffer size is invalid: %1")
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
            this->decoded_blocks.insert(cache_key, decoded);
        }

        const QByteArray &decoded = this->decoded_blocks[cache_key];
        const int local_row = row % int(this->rows_per_strip);
        const qsizetype byte_offset =
            (qsizetype(local_row) * qsizetype(this->width) + qsizetype(column)) *
            qsizetype(sizeof(float));
        return floatAt(decoded, byte_offset, error_message);
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
    QHash<quint32, QByteArray> decoded_blocks;
};

class CopernicusRasterSet
{
public:
    CopernicusRasterSet(const QString &provider_cache_directory, bool allow_remote_fetch,
                        QHash<QString, QDateTime> *unavailable_source_until,
                        QMutex *unavailable_source_mutex)
        : provider_cache_directory(provider_cache_directory),
          allow_remote_fetch(allow_remote_fetch),
          unavailable_source_until(unavailable_source_until),
          unavailable_source_mutex(unavailable_source_mutex)
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
                               this->allow_remote_fetch);
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

QString CopernicusTerrainProvider::providerId() const
{
    return QStringLiteral("copernicus");
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

    CopernicusRasterSet raster_set(provider_cache_directory, allow_remote_fetch,
                                    &this->unavailable_source_until,
                                    &this->unavailable_source_mutex);
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

} // namespace Aowis::Map
