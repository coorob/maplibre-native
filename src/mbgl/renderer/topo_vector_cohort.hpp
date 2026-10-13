#pragma once

#include <mbgl/tile/tile_id.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mbgl::topo_vector {

inline bool isOfficialSourceID(const std::string_view sourceID) noexcept {
    return sourceID == "topo" || sourceID == "norway-topo-vector";
}

inline bool useSharedDrapeCohort(const bool hasOfficialTopo, const bool hasSatelliteBasemap) noexcept {
    // During a style handoff both source families can be present for a frame.
    // Keep the fine raster topology until the outgoing satellite source has
    // left the render tree; a z10 canvas is intentionally specific to the
    // official vector basemap.
    return hasOfficialTopo && !hasSatelliteBasemap;
}

// Collapse the disjoint terrain-leaf cover toward the first detailed official
// Topo zoom. maxParentGap can retain a finer target for close-view colour
// density while still sharing a canvas between siblings. Targets below the
// detail floor retain their natural zoom for low-camera overview rendering.
inline std::vector<OverscaledTileID> cohortForTerrainLeaves(
    const std::unordered_set<OverscaledTileID>& terrainLeaves,
    const uint8_t detailZoom,
    const uint8_t maxParentGap = std::numeric_limits<uint8_t>::max()) {
    std::set<OverscaledTileID> unique;
    for (const auto& leaf : terrainLeaves) {
        if (leaf.canonical.z <= detailZoom) {
            unique.insert(leaf);
            continue;
        }
        const uint8_t gapFloor = leaf.canonical.z > maxParentGap
                                     ? static_cast<uint8_t>(leaf.canonical.z - maxParentGap)
                                     : uint8_t{0};
        unique.insert(leaf.scaledTo(std::max(detailZoom, gapFloor)));
    }
    return {unique.begin(), unique.end()};
}

// Keep the full official detail cohort around the camera, but collapse whole
// sibling groups to a coarser overview parent in the far pitched frustum. A
// group is atomic: if one visible child is selected for detail, every visible
// sibling remains at detail zoom, so the returned cover never contains an
// overlapping parent and child. This preserves trails, streams, indexed
// contours and land-cover detail where the user is looking while avoiding a
// full-screen fan-out of dense z10 vector buckets on Android/OpenGL.
inline std::vector<OverscaledTileID> distanceLimitedCohortForTerrainLeaves(
    const std::unordered_set<OverscaledTileID>& terrainLeaves,
    const uint8_t detailZoom,
    const uint8_t overviewZoom,
    const std::size_t detailTileBudget,
    const double centerMercatorX,
    const double centerMercatorY) {
    const auto detailed = cohortForTerrainLeaves(terrainLeaves, detailZoom);
    if (overviewZoom >= detailZoom || detailTileBudget >= detailed.size()) {
        return detailed;
    }

    std::set<OverscaledTileID> result;
    std::map<OverscaledTileID, std::vector<OverscaledTileID>> detailGroups;
    for (const auto& tileID : detailed) {
        if (tileID.canonical.z < detailZoom) {
            result.insert(tileID);
            continue;
        }
        detailGroups[tileID.scaledTo(overviewZoom)].push_back(tileID);
    }

    struct RankedGroup {
        double distanceSquared;
        OverscaledTileID parent;
        std::vector<OverscaledTileID> children;
    };
    std::vector<RankedGroup> rankedGroups;
    rankedGroups.reserve(detailGroups.size());
    for (auto& [parent, children] : detailGroups) {
        double nearestDistanceSquared = std::numeric_limits<double>::max();
        for (const auto& child : children) {
            const double scale = std::exp2(static_cast<double>(child.canonical.z));
            const double x = (static_cast<double>(child.canonical.x) + 0.5) / scale + child.wrap;
            const double y = (static_cast<double>(child.canonical.y) + 0.5) / scale;
            const double dx = x - centerMercatorX;
            const double dy = y - centerMercatorY;
            nearestDistanceSquared = std::min(nearestDistanceSquared, dx * dx + dy * dy);
        }
        rankedGroups.push_back({nearestDistanceSquared, parent, std::move(children)});
    }
    std::sort(rankedGroups.begin(), rankedGroups.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.distanceSquared != rhs.distanceSquared) {
            return lhs.distanceSquared < rhs.distanceSquared;
        }
        return lhs.parent < rhs.parent;
    });

    std::size_t detailedTilesKept = 0;
    for (const auto& group : rankedGroups) {
        const bool keepDetailed = detailTileBudget > 0 &&
                                  (detailedTilesKept == 0 ||
                                   detailedTilesKept + group.children.size() <= detailTileBudget);
        if (keepDetailed) {
            result.insert(group.children.begin(), group.children.end());
            detailedTilesKept += group.children.size();
        } else {
            result.insert(group.parent);
        }
    }

    return {result.begin(), result.end()};
}

} // namespace mbgl::topo_vector
