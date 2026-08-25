#include <aowis/map/terrain_tile.h>

#include <QDir>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace Aowis::Map
{
namespace
{
constexpr int FixedHeaderBytes = 64;
constexpr quint32 SupportedFlags = 0;
constexpr quint8 WebMercatorXyzProjection = 1;
constexpr quint8 UInt16LinearEncoding = 1;
constexpr int PayloadBytes = TerrainTileSampleCount * int(sizeof(quint16));
constexpr char TerrainTileMagic[8] = {'A', 'O', 'W', 'T', 'R', 'N', '\0', '\0'};

void setError(QString *error_message, const QString &message)
{
    if (error_message != nullptr)
        *error_message = message;
}

void appendUInt16(QByteArray *data, quint16 value)
{
    const quint16 little_endian = qToLittleEndian(value);
    data->append(reinterpret_cast<const char *>(&little_endian), int(sizeof(little_endian)));
}

void appendUInt32(QByteArray *data, quint32 value)
{
    const quint32 little_endian = qToLittleEndian(value);
    data->append(reinterpret_cast<const char *>(&little_endian), int(sizeof(little_endian)));
}

void appendFloat32(QByteArray *data, float value)
{
    static_assert(sizeof(float) == sizeof(quint32));
    quint32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    appendUInt32(data, bits);
}

quint16 readUInt16(const QByteArray &data, qsizetype offset)
{
    return qFromLittleEndian<quint16>(
        reinterpret_cast<const uchar *>(data.constData() + offset));
}

quint32 readUInt32(const QByteArray &data, qsizetype offset)
{
    return qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar *>(data.constData() + offset));
}

float readFloat32(const QByteArray &data, qsizetype offset)
{
    const quint32 bits = readUInt32(data, offset);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

quint32 crc32(const QByteArray &data)
{
    quint32 crc = 0xffffffffu;
    for (char byte : data)
    {
        crc ^= quint32(uchar(byte));
        for (int bit = 0; bit < 8; ++bit)
        {
            const quint32 mask = quint32(0) - (crc & 1u);
            crc = (crc >> 1u) ^ (0xedb88320u & mask);
        }
    }
    return ~crc;
}

quint8 verticalDatumCode(TerrainVerticalDatum datum)
{
    switch (datum)
    {
        case TerrainVerticalDatum::Wgs84Ellipsoid:
            return 1;
        case TerrainVerticalDatum::Egm96:
            return 2;
        case TerrainVerticalDatum::Egm2008:
            return 3;
        case TerrainVerticalDatum::Local:
            return 4;
        case TerrainVerticalDatum::Unknown:
        default:
            return 0;
    }
}

std::optional<TerrainVerticalDatum> verticalDatumFromCode(quint8 code)
{
    switch (code)
    {
        case 0:
            return TerrainVerticalDatum::Unknown;
        case 1:
            return TerrainVerticalDatum::Wgs84Ellipsoid;
        case 2:
            return TerrainVerticalDatum::Egm96;
        case 3:
            return TerrainVerticalDatum::Egm2008;
        case 4:
            return TerrainVerticalDatum::Local;
        default:
            return std::nullopt;
    }
}

bool finiteFloat(float value)
{
    return std::isfinite(double(value));
}
}

bool isValidTerrainDatasetId(const QString &dataset)
{
    if (dataset.isEmpty() || dataset != dataset.trimmed() || dataset != dataset.toLower())
        return false;

    const QByteArray utf8 = dataset.toUtf8();
    if (utf8.isEmpty() || utf8.size() > TerrainTileMaximumDatasetIdBytes)
        return false;

    for (char value : utf8)
    {
        const bool lower_letter = value >= 'a' && value <= 'z';
        const bool digit = value >= '0' && value <= '9';
        const bool punctuation = value == '-' || value == '_' || value == '.';
        if (!lower_letter && !digit && !punctuation)
            return false;
    }

    const char first = utf8.at(0);
    return (first >= 'a' && first <= 'z') || (first >= '0' && first <= '9');
}

bool isValidTerrainTileAddress(const TerrainTileAddress &address)
{
    if (address.zoom < 0 || address.zoom > TerrainTileMaximumZoom)
        return false;

    const quint32 tile_count = quint32(1) << address.zoom;
    return address.x < tile_count && address.y < tile_count;
}

bool isValidTerrainTile(const TerrainTile &tile, QString *error_message)
{
    if (error_message != nullptr)
        error_message->clear();

    if (!isValidTerrainTileAddress(tile.address))
    {
        setError(error_message, QStringLiteral("Invalid terrain tile XYZ address"));
        return false;
    }
    if (!isValidTerrainDatasetId(tile.dataset))
    {
        setError(error_message, QStringLiteral("Invalid terrain dataset identifier"));
        return false;
    }
    if (!std::isfinite(tile.nominal_resolution_m) || tile.nominal_resolution_m < 0.0 ||
        tile.nominal_resolution_m > double(std::numeric_limits<float>::max()))
    {
        setError(error_message, QStringLiteral("Invalid terrain nominal resolution"));
        return false;
    }
    if (!isValidTerrainVerticalDatum(tile.vertical_datum))
    {
        setError(error_message, QStringLiteral("Invalid terrain vertical datum"));
        return false;
    }
    if (tile.elevations_m.size() != TerrainTileSampleCount)
    {
        setError(error_message,
                 QStringLiteral("Terrain tile must contain exactly %1 elevation samples")
                     .arg(TerrainTileSampleCount));
        return false;
    }

    bool has_finite_sample = false;
    for (float elevation_m : tile.elevations_m)
    {
        if (finiteFloat(elevation_m))
        {
            has_finite_sample = true;
            continue;
        }
        if (!std::isnan(double(elevation_m)))
        {
            setError(error_message,
                     QStringLiteral("Terrain tile samples must be finite or NaN no-data values"));
            return false;
        }
    }

    if (!has_finite_sample)
    {
        setError(error_message, QStringLiteral("Terrain tile contains no finite elevation samples"));
        return false;
    }

    return true;
}

QString terrainTileFileExtension()
{
    return QStringLiteral("aowterrain");
}

QString terrainTileMimeType()
{
    return QStringLiteral("application/vnd.aowis.terrain");
}

QString normalizedTerrainTileRelativePath(const QString &dataset,
                                          const TerrainTileAddress &address)
{
    if (!isValidTerrainDatasetId(dataset) || !isValidTerrainTileAddress(address))
        return QString();

    return QDir::cleanPath(
        QStringLiteral("v%1/%2/%3/%4/%5.%6")
            .arg(TerrainTileFormatVersion)
            .arg(dataset)
            .arg(address.zoom)
            .arg(address.x)
            .arg(address.y)
            .arg(terrainTileFileExtension()));
}

QByteArray encodeTerrainTile(const TerrainTile &tile, QString *error_message)
{
    if (error_message != nullptr)
        error_message->clear();

    if (!isValidTerrainTile(tile, error_message))
        return QByteArray();

    float minimum_elevation_m = std::numeric_limits<float>::infinity();
    float maximum_elevation_m = -std::numeric_limits<float>::infinity();
    for (float elevation_m : tile.elevations_m)
    {
        if (!finiteFloat(elevation_m))
            continue;
        minimum_elevation_m = std::min(minimum_elevation_m, elevation_m);
        maximum_elevation_m = std::max(maximum_elevation_m, elevation_m);
    }

    const double elevation_range_m =
        double(maximum_elevation_m) - double(minimum_elevation_m);
    const float elevation_scale_m = elevation_range_m > 0.0
        ? float(elevation_range_m / double(TerrainTileMaximumElevationCode))
        : 0.0f;
    if (!finiteFloat(elevation_scale_m) || elevation_scale_m < 0.0f)
    {
        setError(error_message, QStringLiteral("Terrain tile elevation range cannot be encoded"));
        return QByteArray();
    }

    QByteArray payload;
    payload.reserve(PayloadBytes);
    for (float elevation_m : tile.elevations_m)
    {
        quint16 code = TerrainTileNoDataCode;
        if (finiteFloat(elevation_m))
        {
            if (elevation_scale_m == 0.0f)
            {
                code = 0;
            }
            else
            {
                const double normalized =
                    (double(elevation_m) - double(minimum_elevation_m)) /
                    double(elevation_scale_m);
                const qint64 rounded = qRound64(normalized);
                code = quint16(std::clamp<qint64>(
                    rounded, 0, qint64(TerrainTileMaximumElevationCode)));
            }
        }
        appendUInt16(&payload, code);
    }

    const QByteArray dataset_utf8 = tile.dataset.toUtf8();
    const int header_bytes = FixedHeaderBytes + dataset_utf8.size();
    if (header_bytes > int(std::numeric_limits<quint16>::max()))
    {
        setError(error_message, QStringLiteral("Terrain tile header is too large"));
        return QByteArray();
    }

    QByteArray result;
    result.reserve(header_bytes + payload.size());
    result.append(TerrainTileMagic, int(sizeof(TerrainTileMagic)));
    appendUInt16(&result, TerrainTileFormatVersion);
    appendUInt16(&result, quint16(header_bytes));
    appendUInt32(&result, SupportedFlags);
    result.append(char(WebMercatorXyzProjection));
    result.append(char(UInt16LinearEncoding));
    result.append(char(verticalDatumCode(tile.vertical_datum)));
    result.append(char(0));
    appendUInt16(&result, TerrainTileGridSize);
    appendUInt16(&result, TerrainTileGridSize);
    result.append(char(tile.address.zoom));
    result.append(QByteArray(3, char(0)));
    appendUInt32(&result, tile.address.x);
    appendUInt32(&result, tile.address.y);
    appendFloat32(&result, minimum_elevation_m);
    appendFloat32(&result, elevation_scale_m);
    appendFloat32(&result, float(tile.nominal_resolution_m));
    appendUInt16(&result, quint16(dataset_utf8.size()));
    appendUInt16(&result, 0);
    appendUInt32(&result, TerrainTileSampleCount);
    appendUInt32(&result, PayloadBytes);
    appendUInt32(&result, crc32(payload));
    result.append(dataset_utf8);
    result.append(payload);

    return result;
}

std::optional<TerrainTile> decodeTerrainTile(const QByteArray &data, QString *error_message)
{
    if (error_message != nullptr)
        error_message->clear();

    if (data.size() < FixedHeaderBytes)
    {
        setError(error_message, QStringLiteral("Terrain tile is shorter than the fixed header"));
        return std::nullopt;
    }
    if (std::memcmp(data.constData(), TerrainTileMagic, sizeof(TerrainTileMagic)) != 0)
    {
        setError(error_message, QStringLiteral("Terrain tile magic does not match"));
        return std::nullopt;
    }

    const quint16 version = readUInt16(data, 8);
    const quint16 header_bytes = readUInt16(data, 10);
    const quint32 flags = readUInt32(data, 12);
    const quint8 projection = quint8(uchar(data.at(16)));
    const quint8 encoding = quint8(uchar(data.at(17)));
    const quint8 datum_code = quint8(uchar(data.at(18)));
    const quint8 reserved_19 = quint8(uchar(data.at(19)));
    const quint16 grid_width = readUInt16(data, 20);
    const quint16 grid_height = readUInt16(data, 22);
    const int zoom = int(uchar(data.at(24)));
    const bool reserved_25_27_zero =
        data.at(25) == char(0) && data.at(26) == char(0) && data.at(27) == char(0);
    const quint32 tile_x = readUInt32(data, 28);
    const quint32 tile_y = readUInt32(data, 32);
    const float minimum_elevation_m = readFloat32(data, 36);
    const float elevation_scale_m = readFloat32(data, 40);
    const float nominal_resolution_m = readFloat32(data, 44);
    const quint16 dataset_bytes = readUInt16(data, 48);
    const quint16 reserved_50 = readUInt16(data, 50);
    const quint32 sample_count = readUInt32(data, 52);
    const quint32 payload_bytes = readUInt32(data, 56);
    const quint32 expected_crc32 = readUInt32(data, 60);

    if (version != TerrainTileFormatVersion)
    {
        setError(error_message,
                 QStringLiteral("Unsupported terrain tile version: %1").arg(version));
        return std::nullopt;
    }
    if (flags != SupportedFlags)
    {
        setError(error_message, QStringLiteral("Terrain tile uses unsupported flags"));
        return std::nullopt;
    }
    if (reserved_19 != 0 || !reserved_25_27_zero || reserved_50 != 0)
    {
        setError(error_message, QStringLiteral("Terrain tile reserved header fields are not zero"));
        return std::nullopt;
    }
    if (projection != WebMercatorXyzProjection || encoding != UInt16LinearEncoding)
    {
        setError(error_message,
                 QStringLiteral("Terrain tile uses an unsupported projection or sample encoding"));
        return std::nullopt;
    }
    if (grid_width != TerrainTileGridSize || grid_height != TerrainTileGridSize ||
        sample_count != TerrainTileSampleCount || payload_bytes != PayloadBytes)
    {
        setError(error_message, QStringLiteral("Terrain tile grid or payload dimensions are invalid"));
        return std::nullopt;
    }
    if (header_bytes != FixedHeaderBytes + dataset_bytes || header_bytes > data.size())
    {
        setError(error_message, QStringLiteral("Terrain tile header length is invalid"));
        return std::nullopt;
    }
    if (qsizetype(header_bytes) + qsizetype(payload_bytes) != data.size())
    {
        setError(error_message, QStringLiteral("Terrain tile file length does not match its header"));
        return std::nullopt;
    }
    if (!finiteFloat(minimum_elevation_m) || !finiteFloat(elevation_scale_m) ||
        elevation_scale_m < 0.0f || !finiteFloat(nominal_resolution_m) ||
        nominal_resolution_m < 0.0f)
    {
        setError(error_message, QStringLiteral("Terrain tile elevation metadata is invalid"));
        return std::nullopt;
    }

    const std::optional<TerrainVerticalDatum> vertical_datum = verticalDatumFromCode(datum_code);
    if (!vertical_datum.has_value())
    {
        setError(error_message, QStringLiteral("Terrain tile vertical datum code is invalid"));
        return std::nullopt;
    }

    TerrainTile tile;
    tile.address.zoom = zoom;
    tile.address.x = tile_x;
    tile.address.y = tile_y;
    tile.dataset = QString::fromUtf8(data.constData() + FixedHeaderBytes, dataset_bytes);
    tile.nominal_resolution_m = double(nominal_resolution_m);
    tile.vertical_datum = vertical_datum.value();

    if (!isValidTerrainTileAddress(tile.address) || !isValidTerrainDatasetId(tile.dataset) ||
        tile.dataset.toUtf8().size() != dataset_bytes)
    {
        setError(error_message, QStringLiteral("Terrain tile address or dataset metadata is invalid"));
        return std::nullopt;
    }

    const QByteArray payload = data.sliced(header_bytes, payload_bytes);
    if (crc32(payload) != expected_crc32)
    {
        setError(error_message, QStringLiteral("Terrain tile payload CRC32 check failed"));
        return std::nullopt;
    }

    tile.elevations_m.resize(TerrainTileSampleCount);
    bool has_finite_sample = false;
    for (int index = 0; index < TerrainTileSampleCount; ++index)
    {
        const quint16 code = readUInt16(payload, qsizetype(index) * qsizetype(sizeof(quint16)));
        if (code == TerrainTileNoDataCode)
        {
            tile.elevations_m[index] = std::numeric_limits<float>::quiet_NaN();
            continue;
        }

        tile.elevations_m[index] = minimum_elevation_m + float(code) * elevation_scale_m;
        has_finite_sample = true;
    }

    if (!has_finite_sample)
    {
        setError(error_message, QStringLiteral("Terrain tile payload contains only no-data samples"));
        return std::nullopt;
    }

    return tile;
}

} // namespace Aowis::Map
