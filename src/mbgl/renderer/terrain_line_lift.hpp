#pragma once

#include <mbgl/tile/geometry_tile_data.hpp>
#include <mbgl/util/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string_view>
#include <vector>

namespace mbgl {
namespace terrain_line_lift {

// Line layers whose id ends with this suffix are drawn as 3D geometry on the
// terrain surface after the terrain mesh, at screen resolution, instead of
// being baked into the per-DEM-tile drape canvases. A canvas texel covers
// several screen pixels near a pitched camera, which turns thin strokes into
// soft, stair-stepped bands; lifted lines keep camera-pixel antialiasing.
inline constexpr std::string_view layerIDSuffix = "__terrain-lift";

// KLATTRA_DISABLE_LIFTED_LINES=1 restores the drape path for every layer.
inline bool disabled() {
    static const bool value = std::getenv("KLATTRA_DISABLE_LIFTED_LINES") != nullptr;
    return value;
}

inline bool isLiftedLayerID(std::string_view id) {
    return !disabled() && id.size() > layerIDSuffix.size() && id.ends_with(layerIDSuffix);
}

// Property name that selects the lifted line shader variant: shader groups
// turn it into the define HAS_UNIFORM_u_terrain_lift.
inline constexpr std::string_view shaderDefineName = "a_terrain_lift";

namespace detail {
inline double envNumber(const char* name, double fallback, double lo, double hi) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    char* end = nullptr;
    const double parsed = std::strtod(v, &end);
    return end != v && std::isfinite(parsed) ? std::clamp(parsed, lo, hi) : fallback;
}
} // namespace detail

// Fraction of the view distance by which lifted geometry is pulled toward the
// camera for the terrain depth test (0.004 = 4 m at 1 km). Covers the depth
// buffer's precision and the few pixels a ribbon spans across a slope.
inline double depthPullFraction() {
    static const double value = detail::envNumber("KLATTRA_LIFTED_LINE_DEPTH_PULL", 0.004, 0.0, 0.2);
    return value;
}

// Extra height above the sampled surface, in render metres.
inline double liftMeters() {
    static const double value = detail::envNumber("KLATTRA_LIFTED_LINE_LIFT_M", 0.5, 0.0, 200.0);
    return value;
}

// Terrain mesh cell size in tile units: 128 cells per DEM tile, DEM data stops
// at z12, so deeper source tiles span proportionally more units per cell. Lower
// source zooms coincide with the coarser meshes drawn at those camera zooms.
inline double densifyStep(double canonicalZoom) {
    constexpr double cellUnitsAtDEMZoom = static_cast<double>(util::EXTENT) / 128.0;
    return cellUnitsAtDEMZoom * std::exp2(std::max(0.0, canonicalZoom - 12.0));
}

// Splits every segment where it crosses the terrain mesh grid of spacing
// `step` (tile units): the vertical and horizontal cell edges and the cells'
// anti-diagonal (the mesh triangulates each cell along x + y = const). Inside
// one mesh triangle the surface is a plane, so a straight piece between two
// such crossings lies exactly on the rendered terrain. The crossings depend
// only on world position, so neighbouring tiles that both carry the same
// stretch of line (tile buffers) produce the same points and the same heights.
// Points are rounded to the integer tile grid; the polyline generator treats
// the resulting near-straight joins as miters, so they add no join triangles.
inline GeometryCoordinates densify(const GeometryCoordinates& coordinates, double step) {
    if (coordinates.size() < 2 || !(step > 0.0)) return coordinates;
    GeometryCoordinates result;
    result.reserve(coordinates.size() * 2);
    result.push_back(coordinates.front());
    std::vector<double> ts;
    const auto addCrossings = [&](double a, double b) {
        // Parameters t in (0, 1) where a + (b - a) t hits a multiple of step.
        if (a == b) return;
        const double lo = std::min(a, b) / step;
        const double hi = std::max(a, b) / step;
        for (double k = std::floor(lo) + 1.0; k < hi; k += 1.0) {
            const double t = (k * step - a) / (b - a);
            if (t > 0.0 && t < 1.0) ts.push_back(t);
        }
    };
    for (std::size_t i = 1; i < coordinates.size(); ++i) {
        const auto& p0 = coordinates[i - 1];
        const auto& p1 = coordinates[i];
        ts.clear();
        addCrossings(p0.x, p1.x);
        addCrossings(p0.y, p1.y);
        addCrossings(static_cast<double>(p0.x) + p0.y, static_cast<double>(p1.x) + p1.y);
        std::sort(ts.begin(), ts.end());
        const double dx = static_cast<double>(p1.x) - p0.x;
        const double dy = static_cast<double>(p1.y) - p0.y;
        for (const double t : ts) {
            const GeometryCoordinate p{static_cast<int16_t>(std::lround(p0.x + dx * t)),
                                       static_cast<int16_t>(std::lround(p0.y + dy * t))};
            if (p != result.back()) result.push_back(p);
        }
        if (p1 != result.back()) result.push_back(p1);
    }
    return result;
}

} // namespace terrain_line_lift
} // namespace mbgl
