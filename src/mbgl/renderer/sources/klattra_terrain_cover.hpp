#pragma once

#include <mbgl/util/size.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdlib>

namespace mbgl {

// The accepted phone renderer was calibrated at 112 terrain/raster cover
// tiles. A fixed ceiling is too small for tablets: TransformState::getSize()
// is the map view's logical size, so a full-size iPad exposes substantially
// more pitched frustum than an iPhone at the same camera stop.
//
// Keep every compact viewport on the proven phone budget. Scale only once the
// short edge exceeds a large phone (500 points), reaching a bounded 384-tile
// tablet budget at a 768-point edge. A 1032x1376 iPad viewport at 58 degrees
// pitch requested 322 tiles, so the old 256 ceiling still clipped its far
// field. Using the short edge keeps the budget stable across orientation and
// avoids treating a wide phone as a tablet.
constexpr std::size_t klattraDefaultTerrainRenderTileCap(const Size& viewport) noexcept {
    constexpr std::size_t compactCap = 112;
    constexpr std::size_t expandedCap = 384;
    constexpr uint32_t compactShortEdge = 500;
    constexpr uint32_t expandedShortEdge = 768;

    const uint32_t shortEdge = std::min(viewport.width, viewport.height);
    if (shortEdge <= compactShortEdge) return compactCap;
    if (shortEdge >= expandedShortEdge) return expandedCap;

    constexpr uint32_t edgeSpan = expandedShortEdge - compactShortEdge;
    constexpr std::size_t capSpan = expandedCap - compactCap;
    const uint32_t edgeOffset = shortEdge - compactShortEdge;
    return compactCap + ((static_cast<std::size_t>(edgeOffset) * capSpan + edgeSpan / 2) / edgeSpan);
}

inline std::size_t klattraTerrainRenderTileCap(const Size& viewport) noexcept {
    const std::size_t fallback = klattraDefaultTerrainRenderTileCap(viewport);
    const char* value = std::getenv("KLATTRA_TERRAIN_MAX_RENDER_TILES");
    if (!value || !*value) return fallback;

    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value) return fallback;
    return static_cast<std::size_t>(std::clamp<unsigned long>(parsed, 0, 512));
}

} // namespace mbgl
