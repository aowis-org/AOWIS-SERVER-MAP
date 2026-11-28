#ifndef AOWIS_MAP_TERRAIN_GEOMETRIC_ERROR_ACCUMULATOR_H
#define AOWIS_MAP_TERRAIN_GEOMETRIC_ERROR_ACCUMULATOR_H

#include <aowis/map/terrain_geometric_error.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Aowis::Map::Detail
{

// x/y are fractional grid indices, not degrees. Match the NW/ SW/ NE,
// NE/ SW/ SE triangles in Render's mapGlobeTileIndices(). No hole filling,
// skirts, exaggeration, curvature or quantization in this metric.
inline double triangularGridElevation(const float *grid, int cells, double x, double y)
{
    if (grid == nullptr || cells < 1 || !std::isfinite(x) || !std::isfinite(y) ||
        x < 0.0 || y < 0.0 || x > cells || y > cells)
        return std::numeric_limits<double>::quiet_NaN();
    const int column = std::min(int(x), cells - 1);
    const int row = std::min(int(y), cells - 1);
    const double u = x - column;
    const double v = y - row;
    const float *north = grid + row * (cells + 1) + column;
    const float *south = north + cells + 1;
    if (u + v <= 1.0)
        return (1.0 - u - v) * north[0] + u * north[1] + v * south[0];
    return (1.0 - v) * north[1] + (1.0 - u) * south[0] + (u + v - 1.0) * south[1];
}

class TerrainGeometricErrorAccumulator
{
public:
    void observe(float source, double surface)
    {
        if (!std::isfinite(source))
        {
            ++error.no_data_pixel_count;
            return;
        }
        if (!std::isfinite(surface))
        {
            ++error.missing_surface_pixel_count;
            return;
        }
        const double residual = double(source) - surface;
        error.maximum_underestimation_m = std::max(error.maximum_underestimation_m, residual);
        error.maximum_overestimation_m = std::max(error.maximum_overestimation_m, -residual);
        squared_error_sum += residual * residual;
        ++error.compared_pixel_count;
    }

    TerrainGeometricError result() const
    {
        TerrainGeometricError value = this->error;
        if (value.compared_pixel_count != 0)
            value.rms_geometric_error_m = std::sqrt(squared_error_sum / double(value.compared_pixel_count));
        return value;
    }

private:
    TerrainGeometricError error;
    double squared_error_sum = 0.0;
};

} // namespace Aowis::Map::Detail
#endif
