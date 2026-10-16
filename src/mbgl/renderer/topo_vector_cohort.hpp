#pragma once

#include <mbgl/renderer/raster_drape_cohort.hpp>
#include <mbgl/tile/tile_id.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mbgl::topo_vector {

inline bool isOfficialSourceID(const std::string_view sourceID) noexcept {
    return sourceID == "topo" || sourceID == "norway-topo-vector";
}

// Publication coverage is separate from the Swedish/Norwegian shared-cohort
// policy. Finland's complete vector base can cover a drape target without
// opting its regional NLS overlays into that source-cover policy.
inline bool isAuthoritativeFillSourceID(const std::string_view sourceID, const bool androidOpenGL) noexcept {
    return isOfficialSourceID(sourceID) || (androidOpenGL && sourceID == "finland-openmaptiles");
}

inline bool useSharedDrapeCohort(const bool hasOfficialTopo, const bool hasSatelliteBasemap) noexcept {
    // During a style handoff both source families can be present for a frame.
    // Keep the fine raster topology until the outgoing satellite source has
    // left the render tree; a z10 canvas is intentionally specific to the
    // official vector basemap.
    return hasOfficialTopo && !hasSatelliteBasemap;
}

// The basemap families a render tree draws, accumulated from its (layer,
// source) pairs. Satellite comes from the imagery layers themselves: the
// native aerial blend keeps the Topo style's `topo` source for its labels, so
// counting only the Esri raster draped every 1.0.12 satellite flight through
// the shared z10/z9 Topo canvases on Android (Galaxy S8, 2026-10-16).
struct DrapeBasemaps {
    bool officialTopo = false;
    bool satelliteImagery = false;

    void addLayer(const std::string_view layerID, const std::string_view sourceID) noexcept {
        officialTopo = officialTopo || isOfficialSourceID(sourceID);
        satelliteImagery = satelliteImagery || raster_drape::isSatelliteBasemapRaster(layerID, sourceID);
    }

    bool useSharedCohort() const noexcept { return useSharedDrapeCohort(officialTopo, satelliteImagery); }
};

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

// Close Finnish terrain may reuse a real ancestor DEM while giving the colour
// surface its own finer tile grid. Keep this policy outside the official Topo
// cohort: only a rendered Finnish source, on Android/OpenGL, may opt in.
inline bool useFinnishCloseSurface(const bool androidOpenGL,
                                   const bool hasRenderedFinnishBase,
                                   const bool hasRenderedOfficialTopo,
                                   const bool sharedTopoCohort,
                                   const uint8_t cameraDetailZoom,
                                   const uint8_t demMaxZoom,
                                   const uint32_t nearTextureSize,
                                   const uint32_t nominalTileSize) {
    if (!androidOpenGL || !hasRenderedFinnishBase || hasRenderedOfficialTopo || sharedTopoCohort ||
        nominalTileSize == 0 || nearTextureSize < nominalTileSize) {
        return false;
    }
    const auto textureZoomHeadroom = static_cast<uint8_t>(
        std::floor(std::log2(static_cast<double>(nearTextureSize) / nominalTileSize)));
    return cameraDetailZoom > demMaxZoom + textureZoomHeadroom;
}

inline bool surfaceTilesOverlap(const OverscaledTileID& a, const OverscaledTileID& b) {
    return a.wrap == b.wrap &&
           (a.canonical == b.canonical || a.canonical.isChildOf(b.canonical) ||
            b.canonical.isChildOf(a.canonical));
}

// Only the Android/OpenGL Finnish caller uses this funded refinement bound.
// Its at-most48 extra mesh instances share the existing terrain grid; every
// split still exchanges a complete safe far quartet, preserving canvas count.
inline constexpr std::size_t finnishCloseSurfaceSplitBudget = 16;

struct FinlandSurfaceCover {
    std::unordered_set<OverscaledTileID> meshLeaves;
    std::unordered_set<OverscaledTileID> drapeLeaves;
    std::size_t splitCount = 0;
};

// Each near split is funded by one complete, original max-DEM-zoom quartet
// outside ALL camera-detail footprints. Far geometry stays fine and samples
// its planned parent canvas. Exact canvas count stays constant; mesh count
// increases by at most 3 * maxSplits. No safe quartet means no refinement.
inline FinlandSurfaceCover refineFinnishSurfaceCover(
    const std::unordered_set<OverscaledTileID>& baseLeaves,
    const std::vector<OverscaledTileID>& detailTiles,
    const uint8_t demMaxZoom,
    const std::size_t maxSplits,
    const double centerMercatorX,
    const double centerMercatorY) {
    FinlandSurfaceCover result{baseLeaves, baseLeaves, 0};
    if (demMaxZoom == 0 || maxSplits == 0 || detailTiles.empty()) return result;

    const auto distanceSquared = [&](const OverscaledTileID& id) {
        const double scale = std::exp2(id.canonical.z);
        const double dx = (id.canonical.x + 0.5) / scale + id.wrap - centerMercatorX;
        const double dy = (id.canonical.y + 0.5) / scale - centerMercatorY;
        return dx * dx + dy * dy;
    };
    std::set<OverscaledTileID> farParentSet;
    for (const auto& leaf : baseLeaves) {
        if (leaf.canonical.z != demMaxZoom) continue;
        const auto parent = leaf.scaledTo(static_cast<uint8_t>(demMaxZoom - 1));
        bool complete = true;
        for (const auto& child : parent.canonical.children()) {
            complete = complete && baseLeaves.count(OverscaledTileID(demMaxZoom, parent.wrap, child)) != 0;
        }
        const bool touchesDetail = std::any_of(detailTiles.begin(), detailTiles.end(), [&](const auto& detail) {
            return surfaceTilesOverlap(parent, detail);
        });
        if (complete && !touchesDetail) farParentSet.insert(parent);
    }
    std::vector<OverscaledTileID> farParents(farParentSet.begin(), farParentSet.end());
    std::sort(farParents.begin(), farParents.end(), [&](const auto& a, const auto& b) {
        const auto da = distanceSquared(a), db = distanceSquared(b);
        return da != db ? da > db : a < b;
    });
    const auto limit = std::min(maxSplits, farParents.size());
    while (result.splitCount < limit) {
        // Spend each next split on the coarsest visible leaf. Refining one
        // nearest request to its final zoom first can exhaust the budget
        // while another on-screen footprint still samples its DEM-size canvas.
        std::optional<OverscaledTileID> ancestor;
        for (const auto& leaf : result.meshLeaves) {
            const bool needsDetail = std::any_of(detailTiles.begin(), detailTiles.end(), [&](const auto& detail) {
                return leaf.canonical.z < detail.canonical.z && surfaceTilesOverlap(leaf, detail);
            });
            if (!needsDetail) continue;
            if (!ancestor || leaf.canonical.z < ancestor->canonical.z ||
                (leaf.canonical.z == ancestor->canonical.z &&
                 (distanceSquared(leaf) < distanceSquared(*ancestor) ||
                  (distanceSquared(leaf) == distanceSquared(*ancestor) && leaf < *ancestor)))) {
                ancestor = leaf;
            }
        }
        if (!ancestor) break;
        const auto farParent = farParents[result.splitCount];
        for (const auto& child : farParent.canonical.children()) {
            result.drapeLeaves.erase(OverscaledTileID(demMaxZoom, farParent.wrap, child));
        }
        result.drapeLeaves.insert(farParent);
        result.meshLeaves.erase(*ancestor);
        result.drapeLeaves.erase(*ancestor);
        const uint8_t childZoom = static_cast<uint8_t>(ancestor->canonical.z + 1);
        for (const auto& child : ancestor->canonical.children()) {
            const OverscaledTileID id(childZoom, ancestor->wrap, child);
            result.meshLeaves.insert(id);
            result.drapeLeaves.insert(id);
        }
        ++result.splitCount;
    }
    return result;
}

struct FinnishSurfaceDetail {
    std::size_t requestedFootprints = 0;
    std::size_t coveredFootprints = 0;
    std::size_t resolvedFootprints = 0;
    uint8_t minimumLeafZoom = 0;
    uint8_t maximumLeafZoom = 0;
    std::size_t remainingSplits = 0;
};

// Inspect the candidate surface independently of texture readiness. Geometric
// coverage does not prove requested colour detail: a ready z12 canvas covers
// a z16 footprint, but still needs four ancestor splits along that path.
// The input leaf cover is disjoint; binary tile areas sum exactly here.
inline FinnishSurfaceDetail inspectFinnishSurfaceDetail(
    const std::unordered_set<OverscaledTileID>& meshLeaves,
    const std::vector<OverscaledTileID>& detailTiles) {
    FinnishSurfaceDetail detail;
    std::set<OverscaledTileID> requests;
    for (const auto& request : detailTiles) {
        // A footprint is canonical geography plus wrap; source layout zoom
        // does not create a second visible area with the same bounds.
        requests.emplace(request.canonical.z, request.wrap, request.canonical);
    }
    std::set<OverscaledTileID> pendingSplits;
    bool hasVisibleLeaf = false;
    detail.requestedFootprints = requests.size();
    for (const auto& request : requests) {
        double coveredArea = 0.0;
        bool needsFinerLeaf = false;
        for (const auto& leaf : meshLeaves) {
            if (!surfaceTilesOverlap(leaf, request)) continue;
            if (!hasVisibleLeaf) {
                detail.minimumLeafZoom = leaf.canonical.z;
                detail.maximumLeafZoom = leaf.canonical.z;
                hasVisibleLeaf = true;
            } else {
                detail.minimumLeafZoom = std::min(detail.minimumLeafZoom, leaf.canonical.z);
                detail.maximumLeafZoom = std::max(detail.maximumLeafZoom, leaf.canonical.z);
            }
            if (leaf.canonical.z <= request.canonical.z) {
                coveredArea = 1.0;
                needsFinerLeaf = leaf.canonical.z < request.canonical.z;
                for (uint8_t z = leaf.canonical.z; z < request.canonical.z; ++z) {
                    pendingSplits.insert(request.scaledTo(z));
                }
                break; // a disjoint cover cannot also contain its descendants
            }
            coveredArea += std::ldexp(1.0, 2 * (static_cast<int>(request.canonical.z) - leaf.canonical.z));
        }
        if (coveredArea >= 1.0) {
            ++detail.coveredFootprints;
            if (!needsFinerLeaf) ++detail.resolvedFootprints;
        }
    }
    detail.remainingSplits = pendingSplits.size();
    return detail;
}

struct SurfaceDiagnosticState {
    std::size_t bindings = 0;
    std::size_t ready = 0;
    std::size_t candidateBindings = 0;
    std::size_t candidateReady = 0;
    std::size_t unbound = 0;
    std::size_t candidateUnbound = 0;
    std::size_t emptyDEM = 0;
    std::size_t candidateEmptyDEM = 0;
    std::size_t meshMissing = 0;
    std::size_t meshExtra = 0;
    std::size_t meshDuplicates = 0;
    std::size_t meshOverlap = 0;
    std::size_t retiredTargets = 0;
    std::size_t cleanupRequests = 0;
    bool atomicHold = false;
    bool atomicDrop = false;
    bool pending = false;
};

// Quiescent geometry is not the same as merely empty retired ownership. Keep
// the current and candidate counts explicit so a held/empty cover cannot pass.
inline bool isSurfaceDiagnosticQuiescent(const SurfaceDiagnosticState& state) {
    return state.bindings > 0 && state.ready == state.bindings &&
           state.candidateBindings == state.bindings && state.candidateReady == state.candidateBindings &&
           state.unbound == 0 && state.candidateUnbound == 0 &&
           state.emptyDEM == 0 && state.candidateEmptyDEM == 0 &&
           state.meshMissing == 0 && state.meshExtra == 0 && state.meshDuplicates == 0 && state.meshOverlap == 0 &&
           state.retiredTargets == 0 && state.cleanupRequests == 0 &&
           !state.atomicHold && !state.atomicDrop && !state.pending;
}

// Do not lose the final ownership snapshot when cleanup finishes within the
// one-second diagnostic interval and the renderer then becomes idle. This
// only requests an emission; it neither schedules work nor declares readiness.
inline bool shouldEmitFinalRetiredSurfaceState(const std::size_t lastEmittedTargets,
                                                const std::size_t lastEmittedCleanup,
                                                const std::size_t currentTargets,
                                                const std::size_t currentCleanup,
                                                const bool lastEmittedQuiescent = false,
                                                const bool currentQuiescent = false) {
    const bool ownershipBecameEmpty =
        (lastEmittedTargets > 0 || lastEmittedCleanup > 0) && currentTargets == 0 && currentCleanup == 0;
    return ownershipBecameEmpty || (!lastEmittedQuiescent && currentQuiescent);
}

// Virtual surfaces may be finer than the MVT archive. Canonical source IDs
// stay within its real max zoom, while the largest requested layout zoom is
// retained once per canonical tile. Mixed virtual z15/z16 siblings must not
// request overlapping 14@15 and 14@16 source buckets.
inline std::vector<OverscaledTileID> sourceCoverForTerrainSurfaceLeaves(
    const std::unordered_set<OverscaledTileID>& surfaceLeaves,
    const uint8_t sourceMaxZoom) {
    std::map<UnwrappedTileID, uint8_t> layouts;
    for (const auto& leaf : surfaceLeaves) {
        const auto canonical = leaf.canonical.z > sourceMaxZoom
                                   ? leaf.canonical.scaledTo(sourceMaxZoom)
                                   : leaf.canonical;
        const UnwrappedTileID key(leaf.wrap, canonical);
        auto [it, inserted] = layouts.emplace(key, leaf.overscaledZ);
        if (!inserted) it->second = std::max(it->second, leaf.overscaledZ);
    }
    std::vector<OverscaledTileID> result;
    result.reserve(layouts.size());
    for (const auto& [key, layoutZoom] : layouts) {
        result.emplace_back(layoutZoom, key.wrap, key.canonical);
    }
    return result;
}

inline std::optional<OverscaledTileID> plannedDrapeForSurfaceMesh(
    const OverscaledTileID& mesh,
    const std::unordered_set<OverscaledTileID>& drapeLeaves) {
    for (int z = mesh.canonical.z; z >= 0; --z) {
        const auto candidate = mesh.scaledTo(static_cast<uint8_t>(z));
        if (drapeLeaves.count(candidate)) return candidate;
    }
    return std::nullopt;
}

struct PlannedDrapeSelection {
    bool usePlannedAncestor;
    bool needsRetiredCleanupUpdate;
};

// A retired own child is continuity only until its planned LIVE parent is
// ready. Request one follow-up while that retired texture is still published:
// cleanup runs before atomic publication and must see the successor next time.
inline PlannedDrapeSelection selectPlannedDrapeAncestor(const bool hasDifferentPlannedAncestor,
                                                        const bool plannedAncestorReady,
                                                        const bool retiredOwnStillPublished) {
    const bool usePlanned = hasDifferentPlannedAncestor && plannedAncestorReady;
    return {usePlanned, usePlanned && retiredOwnStillPublished};
}

// Compare texture identities across the actual atomic publication. An old
// virtual child can also retire when refinement returns to the base cover,
// where there is no planned-parent binding to request the cleanup frame.
template <typename TextureID>
inline bool needsRetiredSurfaceCleanup(const std::unordered_set<TextureID>& previousPublished,
                                      const std::unordered_set<TextureID>& nextPublished,
                                      const std::unordered_set<TextureID>& retired) {
    return std::any_of(retired.begin(), retired.end(), [&](const auto& texture) {
        return previousPublished.count(texture) && !nextPublished.count(texture);
    });
}

// The virtual cover can already be gone when a Finnish base tile finishes a
// later ring resize. That publication still needs one cleanup update. Compare
// actual texture identities; a predecessor sampled anywhere in the published
// cover must stay owned, including during an atomic hold.
// The renderer invokes this only inside its Android/OpenGL compile-time gate.
template <typename TextureID>
inline bool needsFinnishSurfaceCleanup(const bool hasRenderedFinnishBase,
                                      const bool hasVirtualSurface,
                                      const std::unordered_set<TextureID>& previousPublished,
                                      const std::unordered_set<TextureID>& nextPublished,
                                      const std::unordered_set<TextureID>& retired) {
    return (hasRenderedFinnishBase || hasVirtualSurface) &&
           needsRetiredSurfaceCleanup(previousPublished, nextPublished, retired);
}

} // namespace mbgl::topo_vector
