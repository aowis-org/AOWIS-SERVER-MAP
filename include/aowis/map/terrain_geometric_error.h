#ifndef AOWIS_MAP_TERRAIN_GEOMETRIC_ERROR_H
#define AOWIS_MAP_TERRAIN_GEOMETRIC_ERROR_H

#include <cstdint>

namespace Aowis::Map
{

// Observed vertical residuals at native DEM pixel centres inside the XYZ
// tile, relative to its unquantized normalized triangulated heightfield.
// Not a certified continuous, ECEF, or screen-space error bound. Missing
// coverage and no-data are reported, never treated as zero error.
struct TerrainGeometricError
{
    double maximum_underestimation_m = 0.0; // max(source - mesh, 0)
    double maximum_overestimation_m = 0.0;  // max(mesh - source, 0)
    double rms_geometric_error_m = 0.0;    // unweighted over compared centres
    std::uint64_t compared_pixel_count = 0;
    std::uint64_t no_data_pixel_count = 0;
    std::uint64_t missing_surface_pixel_count = 0;
    std::uint32_t missing_source_cells = 0;
};

} // namespace Aowis::Map
#endif
