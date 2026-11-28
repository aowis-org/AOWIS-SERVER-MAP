#include "../src/lib/terrain_geometric_error_accumulator.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <functional>

using namespace Aowis::Map::Detail;

int main()
{
    const float grid[] = {0, 10, 20, 100};
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float missing[] = {0, 10, 20, nan};
    const std::function<bool(double, double)> near =
        [](double a, double b) -> bool { return std::abs(a - b) < 1e-9; };
    // The opposite diagonal would give 50 at the centre; bilinear 32.5.
    if (!near(triangularGridElevation(grid, 1, 0.5, 0.5), 15) ||
        !near(triangularGridElevation(grid, 1, 0.75, 0.75), 57.5) ||
        !near(triangularGridElevation(grid, 1, 1, 1), 100) ||
        !near(triangularGridElevation(missing, 1, 0.25, 0.25), 7.5) ||
        !std::isnan(triangularGridElevation(missing, 1, 0.75, 0.75)) ||
        !std::isnan(triangularGridElevation(grid, 1, -0.1, 0.5)))
    {
        std::cerr << "incorrect triangle topology or missing data handling\n";
        return 1;
    }
    const float flat[] = {100, 100, 100, 100};
    TerrainGeometricErrorAccumulator flat_error;
    flat_error.observe(100, triangularGridElevation(flat, 1, 0.3, 0.7));
    if (!near(flat_error.result().rms_geometric_error_m, 0))
        return 1;
    TerrainGeometricErrorAccumulator accumulator;
    accumulator.observe(4500, 1000);
    accumulator.observe(-200, 1000);
    accumulator.observe(1000, 1000);
    accumulator.observe(nan, 1000);
    accumulator.observe(1000, nan);
    const Aowis::Map::TerrainGeometricError error = accumulator.result();
    if (error.compared_pixel_count != 3 || error.no_data_pixel_count != 1 ||
        error.missing_surface_pixel_count != 1 ||
        !near(error.maximum_underestimation_m, 3500) ||
        !near(error.maximum_overestimation_m, 1200) ||
        !near(error.rms_geometric_error_m, std::sqrt((3500.0 * 3500 + 1200.0 * 1200) / 3)))
        return 1;
    return 0;
}
