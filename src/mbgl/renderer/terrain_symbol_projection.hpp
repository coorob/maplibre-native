#pragma once

#include <mbgl/renderer/terrain_perf_diagnostic.hpp>
#include <mbgl/tile/tile_id.hpp>
#include <mbgl/util/constants.hpp>
#include <mbgl/util/mat4.hpp>
#include <mbgl/util/projection.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace mbgl {

// The symbol shader and Camera consume Z in metres. Keep the anchor conversion
// shared by CPU placement and the per-vertex stream, including world copies.
inline LatLng terrainSymbolAnchorLatLng(const UnwrappedTileID& tile, const Point<float>& anchor) {
    const double scale = std::exp2(static_cast<double>(tile.canonical.z));
    return Projection::unproject(
        Point<double>{(static_cast<double>(tile.canonical.x) + tile.wrap * scale + anchor.x / util::EXTENT) *
                          util::tileSize_D,
                      (static_cast<double>(tile.canonical.y) + anchor.y / util::EXTENT) * util::tileSize_D},
        scale);
}

inline float terrainSymbolRenderHeight(std::optional<float> elevation, float exaggeration, float origin) {
    if (!elevation || !std::isfinite(*elevation) || !std::isfinite(exaggeration) || !std::isfinite(origin)) {
        return 0.0f; // No DEM: retain the renderer's zero-height plane.
    }
    const float result = (*elevation - origin) * exaggeration;
    return std::isfinite(result) ? result : 0.0f;
}

inline mat4 terrainSymbolMatrix(const mat4& matrix, float elevation) {
    mat4 result;
    matrix::translate(result, matrix, 0.0, 0.0, elevation);
    return result;
}

using SymbolElevationFunction = std::function<float(const Point<float>&)>;

// PlacedSymbol::vertexStartIndex is segment-local. Dynamic vertices are emitted
// in placed-symbol/glyph order across all segments; use that same global order.
template <class Symbols, class Elevation>
std::vector<float> terrainSymbolVertexElevations(const Symbols& symbols, size_t vertexCount, const Elevation& elevation) {
    std::vector<float> heights(vertexCount, 0.0f);
    size_t cursor = 0;
    for (const auto& symbol : symbols) {
        if (symbol.glyphOffsets.size() > (vertexCount - cursor) / 4) return std::vector<float>(vertexCount, 0.0f);
        const size_t count = symbol.glyphOffsets.size() * 4;
        std::fill_n(heights.begin() + cursor, count, elevation(symbol.anchorPoint));
        cursor += count;
    }
    return cursor == vertexCount ? heights : std::vector<float>(vertexCount, 0.0f);
}

// A bucket-local cache shares samples between CPU line/variable placement and
// text/icon glyph vertices. It never grows beyond the anchors in this bucket.
class TerrainSymbolElevationSampler {
public:
    using Sample = std::function<std::optional<float>(const LatLng&)>;
    TerrainSymbolElevationSampler(UnwrappedTileID tile_, float exaggeration_, float origin_, Sample sample_)
        : tile(tile_), exaggeration(exaggeration_), origin(origin_), sample(std::move(sample_)) {}

    float operator()(const Point<float>& anchor) {
        if (!std::isfinite(anchor.x) || !std::isfinite(anchor.y)) return 0.0f;
        const auto key = std::make_pair(anchor.x, anchor.y);
        const auto [it, inserted] = heights.try_emplace(key);
        terrain_perf::count(inserted ? terrain_perf::Counter::CacheMiss : terrain_perf::Counter::CacheHit);
        if (inserted) it->second = sample(terrainSymbolAnchorLatLng(tile, anchor));
        return terrainSymbolRenderHeight(it->second, exaggeration, origin);
    }

    // Camera-origin/exaggeration changes affect render Z, not the DEM sample.
    void setTransform(float exaggeration_, float origin_) {
        exaggeration = exaggeration_;
        origin = origin_;
    }

private:
    UnwrappedTileID tile;
    float exaggeration;
    float origin;
    Sample sample;
    std::map<std::pair<float, float>, std::optional<float>> heights;
};

} // namespace mbgl
