// Offline ETOPO preparation. Only libtiff and the C++ standard library are needed.
#include <tiffio.h>
#include <aowis/map/terrain_lod_metadata.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <utility>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>
#include <fcntl.h>

namespace
{
namespace fs = std::filesystem;
constexpr int MaximumZoom = 7;
constexpr int GridCells = 64;
constexpr int RecordBytes = 8614;
constexpr int PayloadOffset = 80;
constexpr int SamplesOffset = 164;
constexpr int TileCount = 21845;
constexpr int PackBytes = 16 + TileCount * RecordBytes;
constexpr float MinimumElevation = -12000.0f;
constexpr float Scale = 0.5f;

void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

template <typename T>
const T *tag(TIFF *tiff, uint32_t number, TIFFDataType type, uint32_t &count)
{
    const TIFFField *field = TIFFFindField(tiff, number, TIFF_ANY);
    if (!field || TIFFFieldDataType(field) != type || !TIFFFieldPassCount(field))
        return nullptr;
    void *data = nullptr;
    if (TIFFFieldReadCount(field) == TIFF_VARIABLE2)
    {
        if (!TIFFGetField(tiff, number, &count, &data))
            return nullptr;
    }
    else
    {
        uint16_t short_count = 0;
        if (!TIFFGetField(tiff, number, &short_count, &data))
            return nullptr;
        count = short_count;
    }
    return static_cast<const T *>(data);
}

// Register the standard GeoTIFF/GDAL tags ourselves (no libgeotiff or GDAL).
// This avoids libtiff's unknown-tag warnings on every source open.
TIFFExtendProc previous_extender = nullptr;
void registerGeoTags(TIFF *tiff)
{
    if (previous_extender)
        previous_extender(tiff);
    static const TIFFFieldInfo fields[] = {
        {33550, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_DOUBLE, FIELD_CUSTOM, 1, 1, const_cast<char *>("ModelPixelScale")},
        {33922, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_DOUBLE, FIELD_CUSTOM, 1, 1, const_cast<char *>("ModelTiepoint")},
        {34264, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_DOUBLE, FIELD_CUSTOM, 1, 1, const_cast<char *>("ModelTransformation")},
        {34735, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_SHORT, FIELD_CUSTOM, 1, 1, const_cast<char *>("GeoKeyDirectory")},
        {34736, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_DOUBLE, FIELD_CUSTOM, 1, 1, const_cast<char *>("GeoDoubleParams")},
        {34737, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_ASCII, FIELD_CUSTOM, 1, 1, const_cast<char *>("GeoAsciiParams")},
        {42112, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_ASCII, FIELD_CUSTOM, 1, 1, const_cast<char *>("GDAL_METADATA")},
        {42113, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_ASCII, FIELD_CUSTOM, 1, 1, const_cast<char *>("GDAL_NODATA")}
    };
    TIFFMergeFieldInfo(tiff, fields, sizeof(fields) / sizeof(fields[0]));
}

class Raster
{
public:
    explicit Raster(const fs::path &path, bool exact_etopo)
        : tiff(TIFFOpen(path.c_str(), "r"), TIFFClose)
    {
        require(bool(this->tiff), "cannot open source TIFF");
        TIFF *file = this->tiff.get();
        uint16_t bits = 0, samples = 0, format = 0, planar = 0, orientation = 0;
        require(TIFFGetField(file, TIFFTAG_IMAGEWIDTH, &this->width)
                && TIFFGetField(file, TIFFTAG_IMAGELENGTH, &this->height)
                && TIFFGetFieldDefaulted(file, TIFFTAG_BITSPERSAMPLE, &bits)
                && TIFFGetFieldDefaulted(file, TIFFTAG_SAMPLESPERPIXEL, &samples)
                && TIFFGetFieldDefaulted(file, TIFFTAG_SAMPLEFORMAT, &format)
                && TIFFGetFieldDefaulted(file, TIFFTAG_PLANARCONFIG, &planar)
                && TIFFGetFieldDefaulted(file, TIFFTAG_ORIENTATION, &orientation), "missing raster layout");
        require(this->width > 0 && this->height > 0 && this->width <= 21600 && this->height <= 10800
                && this->width == 2 * this->height, "expected global 2:1 raster");
        require(!exact_etopo || (this->width == 21600 && this->height == 10800), "expected 21600 x 10800 ETOPO raster");
        require(bits == 32 && samples == 1 && format == SAMPLEFORMAT_IEEEFP
                && planar == PLANARCONFIG_CONTIG && orientation == ORIENTATION_TOPLEFT,
                "expected north-up single-band float32 TIFF");
        uint32_t scale_count = 0, tie_count = 0, key_count = 0;
        const double *scale = tag<double>(file, 33550, TIFF_DOUBLE, scale_count);
        const double *tie = tag<double>(file, 33922, TIFF_DOUBLE, tie_count);
        const uint16_t *keys = tag<uint16_t>(file, 34735, TIFF_SHORT, key_count);
        require(scale && scale_count >= 2 && tie && tie_count == 6 && keys && key_count >= 4
                && keys[0] == 1 && key_count >= 4u + 4u * keys[3], "missing or invalid GeoTIFF reference");
        uint32_t matrix_count = 0;
        require(!tag<double>(file, 34264, TIFF_DOUBLE, matrix_count), "rotated GeoTIFF not supported");
        uint16_t raster_type = 1, geographic = 0, vertical = 0, model = 0, angular = 9102;
        for (uint32_t i = 0; i < keys[3]; ++i)
        {
            const uint16_t *key = keys + 4 + 4 * i;
            if (key[1] != 0 || key[2] != 1)
                continue;
            switch (key[0])
            {
            case 1024: model = key[3]; break;
            case 1025: raster_type = key[3]; break;
            case 2048: geographic = key[3]; break;
            case 2054: angular = key[3]; break;
            case 4096: vertical = key[3]; break;
            default: break;
            }
        }
        require(model == 2 && geographic == 4326 && angular == 9102
                && (vertical == 0 || vertical == 3855), "expected WGS84 geographic / EGM2008 reference");
        require(raster_type == 1 || raster_type == 2, "unsupported raster type");
        const double edge_offset = raster_type == 2 ? -0.5 : 0.0;
        const double west = tie[3] + (edge_offset - tie[0]) * scale[0];
        const double north = tie[4] - (edge_offset - tie[1]) * scale[1];
        require(std::abs(west + 180.0) < 1e-5 && std::abs(north - 90.0) < 1e-5
                && std::abs(scale[0] * this->width - 360.0) < 1e-5
                && std::abs(scale[1] * this->height - 180.0) < 1e-5,
                "expected full globe extent");
        // ETOPO uses finite metres; reject both tagged nodata and impossible values.
        uint32_t nodata_count = 0;
        const char *nodata = tag<char>(file, 42113, TIFF_ASCII, nodata_count);
        if (nodata && nodata_count)
            this->no_data = std::stof(std::string(nodata, nodata_count));
    }

    template <typename Visitor>
    void rows(const Visitor &visitor)
    {
        TIFF *file = this->tiff.get();
        if (!TIFFIsTiled(file))
        {
            require(TIFFScanlineSize(file) == tmsize_t(this->width * sizeof(float)), "invalid strip row size");
            std::vector<float> row(this->width);
            for (uint32_t y = 0; y < this->height; ++y)
            {
                require(TIFFReadScanline(file, row.data(), y, 0) == 1, "incomplete TIFF scanline");
                this->validateRow(row.data());
                visitor(y, row.data());
            }
            return;
        }
        uint32_t tw = 0, th = 0;
        require(TIFFGetField(file, TIFFTAG_TILEWIDTH, &tw) && TIFFGetField(file, TIFFTAG_TILELENGTH, &th)
                && tw > 0 && th > 0 && tw <= 32768 && th <= 1024
                && TIFFTileSize(file) == tmsize_t(uint64_t(tw) * th * sizeof(float)), "invalid TIFF tile layout");
        std::vector<float> tile(size_t(tw) * th);
        std::vector<float> band(size_t(this->width) * th);
        for (uint32_t y = 0; y < this->height; y += th)
        {
            const uint32_t rows = std::min(th, this->height - y);
            for (uint32_t x = 0; x < this->width; x += tw)
            {
                require(TIFFReadTile(file, tile.data(), x, y, 0, 0) == TIFFTileSize(file), "incomplete TIFF tile");
                const uint32_t cols = std::min(tw, this->width - x);
                for (uint32_t j = 0; j < rows; ++j)
                    std::copy_n(tile.data() + size_t(j) * tw, cols, band.data() + size_t(j) * this->width + x);
            }
            for (uint32_t j = 0; j < rows; ++j)
            {
                const float *row = band.data() + size_t(j) * this->width;
                this->validateRow(row);
                visitor(y + j, row);
            }
        }
    }

    uint32_t width = 0, height = 0;
private:
    void validateRow(const float *row) const
    {
        for (uint32_t x = 0; x < this->width; ++x)
            require(std::isfinite(row[x]) && row[x] != this->no_data && row[x] >= -12000.0f
                    && row[x] <= 10000.0f, "source contains nodata or out-of-range elevation");
    }
    std::unique_ptr<TIFF, decltype(&TIFFClose)> tiff;
    float no_data = std::numeric_limits<float>::quiet_NaN();
};

// Integrate source pixel footprints in geographic coordinates. Longitude is
// linear in Mercator; inverse-project the row edges for latitude overlap.
// This is an area-weighted resampler, not point sampling or peak inflation.
std::vector<float> project(Raster &source, int n, bool clamp_negative)
{
    std::vector<float> cells(size_t(n) * n, 0.0f);
    std::vector<double> y_edges(n + 1), x_edges(n + 1), prefix(source.width + 1), horizontal(n);
    for (int i = 0; i <= n; ++i)
    {
        const double t = double(i) / n;
        const double latitude = std::atan(std::sinh(std::numbers::pi * (1 - 2 * t))) * 180 / std::numbers::pi;
        y_edges[i] = (90 - latitude) * source.height / 180;
        x_edges[i] = t * source.width;
    }
    int target_y = 0;
    source.rows([&](uint32_t y, const float *row)
    {
        if (double(y + 1) <= y_edges.front() || double(y) >= y_edges.back())
            return;
        prefix[0] = 0;
        for (uint32_t x = 0; x < source.width; ++x)
            prefix[x + 1] = prefix[x] + (clamp_negative ? std::max(0.0f, row[x]) : row[x]);
        double previous = 0.0;
        for (int x = 0; x < n; ++x)
        {
            const double edge = x_edges[x + 1];
            const uint32_t whole = std::min(uint32_t(edge), source.width);
            const double integral = prefix[whole] + (whole < source.width ? (edge - whole) * (prefix[whole + 1] - prefix[whole]) : 0.0);
            horizontal[x] = (integral - previous) / (edge - x_edges[x]);
            previous = integral;
        }
        while (target_y < n && y_edges[target_y] < y + 1.0)
        {
            const double overlap = std::max(0.0, std::min(y_edges[target_y + 1], y + 1.0) - std::max(y_edges[target_y], double(y)));
            const double fraction = overlap / (y_edges[target_y + 1] - y_edges[target_y]);
            for (int x = 0; x < n; ++x)
                cells[size_t(target_y) * n + x] += float(horizontal[x] * fraction);
            if (y_edges[target_y + 1] > y + 1.0)
                break;
            ++target_y;
        }
    });
    require(target_y == n, "incomplete projected coverage");
    return cells;
}

using Record = std::array<unsigned char, RecordBytes>;
void put(Record &record, int offset, uint32_t value, int bytes = 4)
{
    for (int i = 0; i < bytes; ++i)
        record[offset + i] = static_cast<unsigned char>(value >> (8 * i));
}
uint32_t get(const unsigned char *data, int bytes = 4)
{
    uint32_t result = 0;
    for (int i = 0; i < bytes; ++i)
        result |= uint32_t(data[i]) << (8 * i);
    return result;
}
constexpr std::array<uint32_t, 256> crcTable()
{
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i)
    {
        uint32_t c = i;
        for (int bit = 0; bit < 8; ++bit)
            c = (c >> 1) ^ ((c & 1) ? 0xedb88320u : 0);
        table[i] = c;
    }
    return table;
}
uint32_t crc(const Record &record)
{
    constexpr std::array<uint32_t, 256> table = crcTable();
    uint32_t result = 0xffffffffu;
    for (int i = PayloadOffset; i < RecordBytes; ++i)
        result = (result >> 8) ^ table[(result ^ record[i]) & 255];
    return ~result;
}
// Store the same quantized vertex codes as the delivered mesh. A global
// grid shares edge vertices, including the antimeridian.
uint16_t vertexCode(const std::vector<float> &cells, int n, int x, int y)
{
    const size_t top = size_t(std::max(0, y - 1)) * n;
    const size_t bottom = size_t(std::min(y, n - 1)) * n;
    const int left = (x + n - 1) % n, right = x % n;
    const double h = (double(cells[top + left]) + cells[top + right]
                    + cells[bottom + left] + cells[bottom + right]) / 4;
    const double code = std::floor((h - MinimumElevation) / Scale + 0.5);
    require(std::isfinite(code) && code >= 0 && code <= 65534, "invalid encoded elevation");
    return uint16_t(code);
}

struct Residual
{
    float minimum = std::numeric_limits<float>::infinity();
    float maximum = -std::numeric_limits<float>::infinity();
    double under = 0, over = 0, squares = 0;
    uint64_t count = 0;

    void observe(float reference, double surface)
    {
        this->minimum = std::min(this->minimum, reference);
        this->maximum = std::max(this->maximum, reference);
        const double difference = double(reference) - surface;
        this->under = std::max(this->under, difference);
        this->over = std::max(this->over, -difference);
        this->squares += difference * difference;
        ++this->count;
    }

    Aowis::TerrainWire::MetadataBuffer metadata() const
    {
        using namespace Aowis::TerrainWire;
        if (!this->count)
            return {};
        const TerrainSourceStatistics source{this->minimum, this->maximum, this->count, 0, 0};
        const TerrainGeometricError error{this->under, this->over,
            std::sqrt(this->squares / double(this->count)), this->count, 0, 0, 0};
        const std::optional<MetadataBuffer> encoded = encodeMetadata(source, error);
        require(encoded.has_value(), "invalid overview metadata");
        return *encoded;
    }
};

struct AxisSample
{
    int cell;
    double fraction;
};

AxisSample axisSample(double position, int n)
{
    const double p = std::clamp(position, 0.0, double(n));
    const int cell = std::min(int(p), n - 1);
    return {cell, p - cell};
}

// NE-SW diagonal: identical to Render's surface index topology. The
// codes are interpolated before conversion to metres (an affine transform).
double triangleHeight(uint16_t nw, uint16_t ne, uint16_t sw, uint16_t se,
                      double u, double v)
{
    const double code = u + v <= 1.0
        ? double(nw) + u * (double(ne) - nw) + v * (double(sw) - nw)
        : double(se) + (1.0 - u) * (double(sw) - se) + (1.0 - v) * (double(ne) - se);
    return MinimumElevation + Scale * code;
}

struct ErrorLevel
{
    int n = 0;
    std::vector<uint16_t> vertices;
    std::vector<AxisSample> columns;
    std::vector<Residual> tiles;

    void initialize(const std::vector<float> &cells, int z, uint32_t source_width)
    {
        this->n = GridCells << z;
        const int stride = this->n + 1;
        this->vertices.resize(size_t(stride) * stride);
        for (int y = 0; y <= this->n; ++y)
            for (int x = 0; x <= this->n; ++x)
                this->vertices[size_t(y) * stride + x] = vertexCode(cells, this->n, x, y);
        this->columns.resize(source_width);
        for (uint32_t x = 0; x < source_width; ++x)
            this->columns[x] = axisSample((double(x) + 0.5) * this->n / source_width, this->n);
        const int side = 1 << z;
        this->tiles.resize(size_t(side) * side);
    }

    void observeRow(double mercator_y, const float *row, bool clamp_negative)
    {
        const AxisSample sy = axisSample(mercator_y * this->n, this->n);
        const size_t stride = size_t(this->n) + 1;
        const uint16_t *top = this->vertices.data() + size_t(sy.cell) * stride;
        const uint16_t *bottom = top + stride;
        Residual *tile_row = this->tiles.data() + size_t(sy.cell / GridCells) * (this->n / GridCells);
        for (size_t x = 0; x < this->columns.size(); ++x)
        {
            const AxisSample &sx = this->columns[x];
            const int i = sx.cell;
            const float reference = clamp_negative ? std::max(0.0f, row[x]) : row[x];
            tile_row[i / GridCells].observe(reference,
                triangleHeight(top[i], top[i + 1], bottom[i], bottom[i + 1], sx.fraction, sy.fraction));
        }
    }
};

void measureErrors(Raster &source, std::array<ErrorLevel, MaximumZoom + 1> &levels, bool clamp_negative)
{
    // One additional streaming pass over the source, not one per zoom. Pixel
    // centres outside Web Mercator coverage are deliberately excluded.
    source.rows([&](uint32_t y, const float *row)
    {
        const double latitude = (90.0 - (double(y) + 0.5) * 180.0 / source.height)
                              * std::numbers::pi / 180.0;
        const double mercator_y = (1.0 - std::asinh(std::tan(latitude)) / std::numbers::pi) * 0.5;
        if (mercator_y < 0.0 || mercator_y >= 1.0)
            return;
        for (ErrorLevel &level : levels)
            level.observeRow(mercator_y, row, clamp_negative);
    });
}

Record encode(const std::vector<float> &cells, int z, int tx, int ty, float resolution,
              const ErrorLevel *level = nullptr)
{
    Record record{};
    const int n = GridCells << z;
    std::copy_n("AOWTRN\0", 8, record.data());
    put(record, 8, 2, 2); put(record, 10, 80, 2);
    record[16] = 1; record[17] = 1; record[18] = 3;
    put(record, 20, 65, 2); put(record, 22, 65, 2); record[24] = static_cast<unsigned char>(z);
    put(record, 28, tx); put(record, 32, ty);
    put(record, 36, std::bit_cast<uint32_t>(MinimumElevation));
    put(record, 40, std::bit_cast<uint32_t>(Scale));
    put(record, 44, std::bit_cast<uint32_t>(resolution));
    put(record, 48, 16, 2); put(record, 52, 4225); put(record, 56, RecordBytes - PayloadOffset);
    std::copy_n("copernicus-glo30", 16, record.data() + 64);
    for (int j = 0; j <= 64; ++j)
    {
        const int y = ty * 64 + j;
        for (int i = 0; i <= 64; ++i)
        {
            const int x = tx * 64 + i;
            const uint16_t code = level ? level->vertices[size_t(y) * (n + 1) + x]
                                        : vertexCode(cells, n, x, y);
            put(record, SamplesOffset + 2 * (j * 65 + i), code, 2);
        }
    }
    if (level)
    {
        const Aowis::TerrainWire::MetadataBuffer metadata = level->tiles[size_t(ty) * (1 << z) + tx].metadata();
        std::copy(metadata.begin(), metadata.end(), record.begin() + PayloadOffset);
    }
    put(record, 60, crc(record));
    return record;
}
int64_t offset(int z, int x, int y)
{
    const int side = 1 << z;
    return 16 + int64_t((side * side - 1) / 3 + y * side + x) * RecordBytes;
}
void validatePack(const fs::path &path)
{
    require(fs::file_size(path) == PackBytes, "invalid pack length");
    std::ifstream input(path, std::ios::binary);
    std::array<unsigned char, 16> header{};
    input.read(reinterpret_cast<char *>(header.data()), header.size());
    require(bool(input) && std::equal(header.begin(), header.begin() + 8, "AOWCOAR1")
            && get(header.data() + 8) == MaximumZoom && get(header.data() + 12) == RecordBytes, "invalid pack header");
    for (int z = 0; z <= MaximumZoom; ++z)
        for (int y = 0; y < (1 << z); ++y)
            for (int x = 0; x < (1 << z); ++x)
            {
                Record record{};
                input.read(reinterpret_cast<char *>(record.data()), record.size());
                require(bool(input) && std::equal(record.begin(), record.begin() + 8, "AOWTRN\0")
                    && get(record.data() + 8, 2) == 2 && get(record.data() + 10, 2) == 80
                    && record[16] == 1 && record[17] == 1 && record[18] == 3
                    && get(record.data() + 20, 2) == 65 && get(record.data() + 22, 2) == 65
                    && record[24] == z && get(record.data() + 28) == uint32_t(x) && get(record.data() + 32) == uint32_t(y)
                    && get(record.data() + 52) == 4225 && get(record.data() + 56) == RecordBytes - PayloadOffset
                    && std::equal(record.begin() + 64, record.begin() + 80, "copernicus-glo30")
                    && get(record.data() + 60) == crc(record), "invalid overview record");
                std::optional<Aowis::TerrainWire::TerrainSourceStatistics> statistics;
                std::optional<Aowis::TerrainWire::TerrainGeometricError> error;
                require(Aowis::TerrainWire::decodeMetadata(record.data() + PayloadOffset,
                    RecordBytes - PayloadOffset, statistics, error)
                    && statistics.has_value() == error.has_value()
                    && (!statistics || statistics->valid_pixel_count == error->compared_pixel_count),
                    "invalid overview metadata");
            }
}
void writePack(Raster &source, std::vector<float> cells, const fs::path &output, float resolution, bool clamp_negative)
{
    // Caller creates a unique temporary file in the destination directory.
    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    require(bool(out), "cannot create pack");
    Record header{};
    std::copy_n("AOWCOAR1", 8, header.data());
    put(header, 8, MaximumZoom); put(header, 12, RecordBytes);
    out.write(reinterpret_cast<const char *>(header.data()), 16);
    std::array<ErrorLevel, MaximumZoom + 1> levels;
    for (int z = MaximumZoom; z >= 0; --z)
    {
        const int n = GridCells << z;
        levels[z].initialize(cells, z, source.width);
        if (z)
        {
            const int m = n / 2;
            // In-place reduction: the destination never overlaps an unread source.
            for (int y = 0; y < m; ++y)
                for (int x = 0; x < m; ++x)
                {
                    const size_t i = size_t(y * 2) * n + x * 2;
                    cells[size_t(y) * m + x] = (cells[i] + cells[i + 1] + cells[i + n] + cells[i + n + 1]) * 0.25f;
                }
            cells.resize(size_t(m) * m);
        }
    }
    std::vector<float>().swap(cells);
    std::cout << "Measuring ETOPO geometric error at zooms 0-7" << std::endl;
    measureErrors(source, levels, clamp_negative);
    for (int z = MaximumZoom; z >= 0; --z)
    {
        const int side = 1 << z;
        out.seekp(offset(z, 0, 0));
        for (int y = 0; y < side; ++y)
            for (int x = 0; x < side; ++x)
            {
                const Record record = encode(cells, z, x, y, resolution, &levels[z]);
                out.write(reinterpret_cast<const char *>(record.data()), record.size());
            }
        require(bool(out), "pack write failed");
        std::cout << "Zoom " << z << ": " << side * side << " tiles" << std::endl;
    }
    out.close();
    require(bool(out), "pack close failed");
}

void build(const fs::path &source_path, const fs::path &output, float resolution, bool clamp_negative)
{
    require(std::isfinite(resolution) && resolution > 0, "invalid source resolution");
    require(!fs::exists(output) || !fs::equivalent(source_path, output), "source and output must differ");
    Raster source(source_path, false);
    std::vector<float> cells = project(source, GridCells << MaximumZoom, clamp_negative);
    if (!output.parent_path().empty())
        fs::create_directories(output.parent_path());
    std::string pattern = output.string() + ".XXXXXX";
    const int fd = mkstemp(pattern.data());
    require(fd >= 0, "cannot allocate temporary pack");
    close(fd);
    try
    {
        writePack(source, std::move(cells), pattern, resolution, clamp_negative);
        validatePack(pattern);
        fs::permissions(pattern, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read);
        const int pack_fd = open(pattern.c_str(), O_RDONLY);
        require(pack_fd >= 0, "cannot open completed pack");
        const int synced = fsync(pack_fd);
        const int closed = close(pack_fd);
        require(synced == 0 && closed == 0, "cannot sync completed pack");
        fs::rename(pattern, output);
    }
    catch (...)
    {
        fs::remove(pattern);
        throw;
    }
}

// Small native fixtures cover the GeoTIFF decoder and weighted projection
// without requiring a global dataset or Python in the test environment.
void testRaster(const fs::path &path, bool tiled)
{
    {
        std::unique_ptr<TIFF, decltype(&TIFFClose)> file(TIFFOpen(path.c_str(), "w"), TIFFClose);
        require(bool(file), "cannot create fixture");
        TIFF *tiff = file.get();
        TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, 32u);
        TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, 16u);
        TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, 32);
        TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, 1);
        TIFFSetField(tiff, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_IEEEFP);
        TIFFSetField(tiff, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
        const double scale[] = {11.25, 11.25, 0};
        const double tie[] = {0, 0, 0, -180, 90, 0};
        const uint16_t keys[] = {1, 1, 0, 4, 1024, 0, 1, 2, 1025, 0, 1, 1, 2048, 0, 1, 4326, 4096, 0, 1, 3855};
        require(TIFFSetField(tiff, 33550, 3u, scale) && TIFFSetField(tiff, 33922, 6u, tie)
                && TIFFSetField(tiff, 34735, 20u, keys), "cannot write georeference");
        if (tiled)
        {
            TIFFSetField(tiff, TIFFTAG_TILEWIDTH, 16u);
            TIFFSetField(tiff, TIFFTAG_TILELENGTH, 16u);
            std::vector<float> tile(256, -100);
            require(TIFFWriteTile(tiff, tile.data(), 0, 0, 0, 0) > 0, "fixture write failed");
            std::fill(tile.begin(), tile.end(), 200.0f);
            require(TIFFWriteTile(tiff, tile.data(), 16, 0, 0, 0) > 0, "fixture write failed");
        }
        else
        {
            TIFFSetField(tiff, TIFFTAG_ROWSPERSTRIP, 4u);
            std::vector<float> row(32, 200);
            std::fill_n(row.begin(), 16, -100.0f);
            for (uint32_t y = 0; y < 16; ++y)
                require(TIFFWriteScanline(tiff, row.data(), y, 0) == 1, "fixture write failed");
        }
    }
    Raster source(path, false);
    const std::vector<float> cells = project(source, 64, true);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            require(std::abs(cells[y * 64 + x] - (x < 32 ? 0 : 200)) < 0.001, "wrong projected value");
}

void selfTest()
{
    constexpr int n = 128;
    std::vector<float> cells(n * n);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            cells[y * n + x] = float(x * 3 + y);
    const Record a = encode(cells, 1, 0, 0, 1855), b = encode(cells, 1, 1, 0, 1855), c = encode(cells, 1, 0, 1, 1855);
    for (int i = 0; i <= 64; ++i)
    {
        require(get(a.data() + SamplesOffset + 2 * (i * 65 + 64), 2) == get(b.data() + SamplesOffset + 2 * i * 65, 2), "horizontal seam");
        require(get(b.data() + SamplesOffset + 2 * (i * 65 + 64), 2) == get(a.data() + SamplesOffset + 2 * i * 65, 2), "antimeridian seam");
        require(get(a.data() + SamplesOffset + 2 * (64 * 65 + i), 2) == get(c.data() + SamplesOffset + 2 * i, 2), "vertical seam");
    }
    require(get(a.data() + 60) == crc(a), "CRC mismatch");
    require(triangleHeight(24000, 24000, 24000, 26000, 0.25, 0.25) == 0.0,
            "wrong NE-SW triangle");
    require(triangleHeight(24000, 24000, 24000, 26000, 0.75, 0.75) == 500.0,
            "wrong SE triangle");
    Residual residual;
    residual.observe(4500.0f, 1000.0);
    residual.observe(0.0f, 1000.0);
    std::optional<Aowis::TerrainWire::TerrainSourceStatistics> statistics;
    std::optional<Aowis::TerrainWire::TerrainGeometricError> error;
    const Aowis::TerrainWire::MetadataBuffer metadata = residual.metadata();
    require(Aowis::TerrainWire::decodeMetadata(metadata.data(), metadata.size(), statistics, error)
        && statistics && error && statistics->minimum_elevation_m == 0.0f
        && statistics->maximum_elevation_m == 4500.0f && error->compared_pixel_count == 2
        && error->maximum_underestimation_m == 3500.0 && error->maximum_overestimation_m == 1000.0
        && std::abs(error->rms_geometric_error_m - std::sqrt(6625000.0)) < 1e-9,
        "wrong source residual statistics");
    ErrorLevel level;
    level.initialize(cells, 1, 2);
    const float samples[] = {4500.0f, 0.0f};
    level.observeRow(0.25, samples, true);
    const Record measured = encode(cells, 1, 0, 0, 1855, &level);
    require(Aowis::TerrainWire::decodeMetadata(measured.data() + PayloadOffset,
        RecordBytes - PayloadOffset, statistics, error) && statistics && error
        && statistics->maximum_elevation_m == 4500.0f && error->compared_pixel_count == 1
        && error->maximum_underestimation_m > 4000.0 && get(measured.data() + 60) == crc(measured),
        "metadata record did not preserve summit");
    std::string pattern = (fs::temp_directory_path() / "aowis-etopo-test.XXXXXX").string();
    require(mkdtemp(pattern.data()) != nullptr, "cannot create fixture directory");
    try
    {
        testRaster(fs::path(pattern) / "strip.tif", false);
        testRaster(fs::path(pattern) / "tile.tif", true);
        fs::remove_all(pattern);
    }
    catch (...)
    {
        fs::remove_all(pattern);
        throw;
    }
}
}

int main(int argc, char **argv)
{
    previous_extender = TIFFSetTagExtender(registerGeoTags);
    try
    {
        const std::string command = argc > 1 ? argv[1] : "";
        if (command == "validate-source" && argc == 3)
        {
            Raster source(argv[2], true);
            source.rows([](uint32_t, const float *) {});
        }
        else if (command == "validate-pack" && argc == 3)
            validatePack(argv[2]);
        else if (command == "build" && (argc == 5 || argc == 6))
        {
            require(argc == 5 || std::string(argv[5]) == "--clamp-negative", "unknown build option");
            build(argv[2], argv[3], std::stof(argv[4]), argc == 6);
        }
        else if (command == "self-test" && argc == 2)
            selfTest();
        else
            throw std::runtime_error("usage: aowis-map-etopo validate-source TIFF | validate-pack PACK | build TIFF PACK RESOLUTION_M [--clamp-negative] | self-test");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "ETOPO: " << error.what() << '\n';
        return 1;
    }
}
