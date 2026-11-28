#ifndef AOWIS_TERRAIN_LOD_METADATA_H
#define AOWIS_TERRAIN_LOD_METADATA_H

#include <array>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>

namespace Aowis::TerrainWire
{
// Observed native raster extrema (including normalization halo), not certified bounds.
struct TerrainSourceStatistics
{
    float minimum_elevation_m = 0;
    float maximum_elevation_m = 0;
    std::uint64_t valid_pixel_count = 0;
    std::uint64_t no_data_pixel_count = 0;
    std::uint32_t missing_source_cells = 0;
};

// Vertical residuals at native DEM centres against the unquantized triangular grid.
// Not a certified continuous, ECEF or screen-space error bound.
struct TerrainGeometricError
{
    double maximum_underestimation_m = 0;
    double maximum_overestimation_m = 0;
    double rms_geometric_error_m = 0;
    std::uint64_t compared_pixel_count = 0;
    std::uint64_t no_data_pixel_count = 0;
    std::uint64_t missing_surface_pixel_count = 0;
    std::uint32_t missing_source_cells = 0;
};

inline constexpr int MetadataBytes = 84;
using MetadataBuffer = std::array<unsigned char, MetadataBytes>;
static_assert(sizeof(float) == 4 && sizeof(double) == 8 &&
              std::numeric_limits<float>::is_iec559 && std::numeric_limits<double>::is_iec559);

inline bool valid(const TerrainSourceStatistics &s)
{
    return s.valid_pixel_count != 0 && std::isfinite(s.minimum_elevation_m) &&
           std::isfinite(s.maximum_elevation_m) && s.minimum_elevation_m <= s.maximum_elevation_m;
}
inline bool valid(const TerrainGeometricError &e)
{
    return e.compared_pixel_count != 0 && std::isfinite(e.maximum_underestimation_m) &&
           std::isfinite(e.maximum_overestimation_m) && std::isfinite(e.rms_geometric_error_m) &&
           e.maximum_underestimation_m >= 0 && e.maximum_overestimation_m >= 0 &&
           e.rms_geometric_error_m >= 0;
}
namespace Detail
{
inline void put(MetadataBuffer &b, int offset, std::uint64_t value, int size)
{
    for (int i = 0; i < size; ++i) b[offset + i] = static_cast<unsigned char>(value >> (8 * i));
}
inline std::uint64_t get(const unsigned char *b, int offset, int size)
{
    std::uint64_t value = 0;
    for (int i = 0; i < size; ++i) value |= std::uint64_t(b[offset + i]) << (8 * i);
    return value;
}
inline void putFloat(MetadataBuffer &b, int offset, float value)
{
    std::uint32_t bits; std::memcpy(&bits, &value, 4); put(b, offset, bits, 4);
}
inline void putDouble(MetadataBuffer &b, int offset, double value)
{
    std::uint64_t bits; std::memcpy(&bits, &value, 8); put(b, offset, bits, 8);
}
inline float getFloat(const unsigned char *b, int offset)
{
    const std::uint32_t bits = std::uint32_t(get(b, offset, 4)); float value;
    std::memcpy(&value, &bits, 4); return value;
}
inline double getDouble(const unsigned char *b, int offset)
{
    const std::uint64_t bits = get(b, offset, 8); double value;
    std::memcpy(&value, &bits, 8); return value;
}
inline bool zero(const unsigned char *b, int first, int end)
{
    for (int i = first; i < end; ++i) if (b[i] != 0) return false;
    return true;
}
}

// Fixed, CRC-covered prefix to the height codes. Missing blocks are zero-filled.
inline std::optional<MetadataBuffer> encodeMetadata(
    const std::optional<TerrainSourceStatistics> &source,
    const std::optional<TerrainGeometricError> &error)
{
    using namespace Detail;
    MetadataBuffer b{};
    if (source)
    {
        if (!valid(*source)) return std::nullopt;
        b[0] |= 1;
        putFloat(b, 4, source->minimum_elevation_m); putFloat(b, 8, source->maximum_elevation_m);
        put(b, 12, source->valid_pixel_count, 8); put(b, 20, source->no_data_pixel_count, 8);
        put(b, 28, source->missing_source_cells, 4);
    }
    if (error)
    {
        if (!valid(*error)) return std::nullopt;
        b[0] |= 2;
        putDouble(b, 32, error->maximum_underestimation_m);
        putDouble(b, 40, error->maximum_overestimation_m);
        putDouble(b, 48, error->rms_geometric_error_m);
        put(b, 56, error->compared_pixel_count, 8); put(b, 64, error->no_data_pixel_count, 8);
        put(b, 72, error->missing_surface_pixel_count, 8); put(b, 80, error->missing_source_cells, 4);
    }
    return b;
}
inline bool decodeMetadata(const unsigned char *b, std::size_t size,
    std::optional<TerrainSourceStatistics> &source, std::optional<TerrainGeometricError> &error)
{
    using namespace Detail;
    if (!b || size < MetadataBytes) return false;
    const std::uint64_t flags = get(b, 0, 4);
    if (flags & ~std::uint64_t(3)) return false;
    std::optional<TerrainSourceStatistics> s;
    std::optional<TerrainGeometricError> e;
    if (flags & 1)
    {
        s = TerrainSourceStatistics{getFloat(b, 4), getFloat(b, 8), get(b, 12, 8),
            get(b, 20, 8), std::uint32_t(get(b, 28, 4))};
        if (!valid(*s)) return false;
    }
    else if (!zero(b, 4, 32)) return false;
    if (flags & 2)
    {
        e = TerrainGeometricError{getDouble(b, 32), getDouble(b, 40), getDouble(b, 48),
            get(b, 56, 8), get(b, 64, 8), get(b, 72, 8), std::uint32_t(get(b, 80, 4))};
        if (!valid(*e)) return false;
    }
    else if (!zero(b, 32, MetadataBytes)) return false;
    source = s; error = e;
    return true;
}
} // namespace Aowis::TerrainWire
#endif
