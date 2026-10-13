#pragma once

#include <mbgl/renderer/buckets/raster_bucket.hpp>
#include <mbgl/tile/tile_id.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace mbgl::raster_drape {

struct Cohort {
    std::optional<OverscaledTileID> floor;
    std::array<std::optional<OverscaledTileID>, 4> children;

    bool hasCompleteChildren() const noexcept {
        for (const auto& child : children) {
            if (!child) return false;
        }
        return true;
    }
};

enum class IncompleteDEMAction : uint8_t {
    Publish,
    HoldPrevious,
    DropTerrain,
};

inline IncompleteDEMAction selectIncompleteDEMAction(const std::size_t candidateBindings,
                                                     const std::size_t emptyDEMBindings,
                                                     const bool previousCoverDrawableBacked,
                                                     const bool previousCoverHasOnlyRealDEM) noexcept {
    if (candidateBindings == 0 || emptyDEMBindings == 0) return IncompleteDEMAction::Publish;
    if (previousCoverDrawableBacked && previousCoverHasOnlyRealDEM) {
        return IncompleteDEMAction::HoldPrevious;
    }
    return IncompleteDEMAction::DropTerrain;
}

inline bool bucketHasTextureSource(const RasterBucket& bucket) noexcept {
    return bucket.texture2d || (bucket.image && bucket.image->valid());
}

inline bool isAuthoritativeTerrainRaster(const std::string& layerID, const std::string& sourceID) noexcept {
    return layerID == "satellite-terrain-layer" && sourceID == "satellite-terrain-raster";
}

inline bool hasAuthoritativePublicationRequirement(const bool requiresRaster,
                                                   const std::size_t declaredSourceRequirements) noexcept {
    return requiresRaster || declaredSourceRequirements > 0;
}

inline bool publicationGenerationNeedsRetry(const uint64_t pendingGeneration,
                                            const uint64_t publishedGeneration) noexcept {
    return pendingGeneration > publishedGeneration;
}

inline bool publicationNeedsFrameRetry(const uint64_t pendingGeneration,
                                       const uint64_t publishedGeneration,
                                       const bool requiresRaster,
                                       const std::size_t declaredSourceRequirements) noexcept {
    return publicationGenerationNeedsRetry(pendingGeneration, publishedGeneration) &&
           hasAuthoritativePublicationRequirement(requiresRaster, declaredSourceRequirements);
}

// A rendered frame can retry an unattempted drape generation, but it cannot
// make missing source coverage or an absent raster floor become ready by
// itself. Those states need a source/style update, which already schedules a
// normal frame. Keeping the distinction here
// prevents a held target from turning a static map into an unbounded repaint
// loop (especially costly on Android emulators, where every update rebuilds
// the Goldfish-safe main pass).
inline bool publicationNeedsAutomaticFrameRetry(const uint64_t pendingGeneration,
                                                const uint64_t publishedGeneration,
                                                const bool requiresRaster,
                                                const std::size_t declaredSourceRequirements,
                                                const uint64_t attemptedGeneration,
                                                const bool attemptBlockedUntilExternalUpdate) noexcept {
    if (!publicationNeedsFrameRetry(
            pendingGeneration, publishedGeneration, requiresRaster, declaredSourceRequirements)) {
        return false;
    }
    return attemptedGeneration != pendingGeneration || !attemptBlockedUntilExternalUpdate;
}

inline bool unbakedTargetNeedsAutomaticFrame(const uint64_t completedRenders,
                                             const uint64_t minimumCompletedRenders,
                                             const uint64_t pendingGeneration,
                                             const uint64_t attemptedGeneration,
                                             const bool attemptBlockedUntilExternalUpdate) noexcept {
    if (completedRenders >= minimumCompletedRenders) {
        return false;
    }
    return attemptedGeneration != pendingGeneration || !attemptBlockedUntilExternalUpdate;
}

inline bool textureIdentityMatches(const void* currentTexture, const void* publishedTexture) noexcept {
    return currentTexture && currentTexture == publishedTexture;
}

inline bool containsExactTarget(const OverscaledTileID& target,
                                const std::vector<OverscaledTileID>& readyTargets) noexcept {
    return std::find(readyTargets.begin(), readyTargets.end(), target) != readyTargets.end();
}

inline bool tileCoversTarget(const OverscaledTileID& cover, const OverscaledTileID& target) noexcept {
    return cover.wrap == target.wrap && cover.canonical.z <= target.canonical.z &&
           (cover.canonical == target.canonical || target.canonical.isChildOf(cover.canonical));
}

inline bool tileIsStrictChildOf(const OverscaledTileID& child, const OverscaledTileID& parent) noexcept {
    return child.wrap == parent.wrap && child.canonical.z > parent.canonical.z &&
           child.canonical.isChildOf(parent.canonical);
}

inline Cohort selectCohort(const OverscaledTileID& target, const std::vector<OverscaledTileID>& usableTileIDs) {
    Cohort result;
    const auto expectedChildren = target.canonical.children();

    for (const auto& tileID : usableTileIDs) {
        if (tileCoversTarget(tileID, target)) {
            if (!result.floor || tileID.canonical.z > result.floor->canonical.z) {
                result.floor = tileID;
            }
            continue;
        }
        if (tileID.wrap != target.wrap || tileID.canonical.z != target.canonical.z + 1u) {
            continue;
        }
        for (std::size_t i = 0; i < expectedChildren.size(); ++i) {
            if (tileID.canonical == expectedChildren[i]) {
                result.children[i] = tileID;
                break;
            }
        }
    }
    return result;
}

inline bool coversTargetAtAnyResolution(const OverscaledTileID& target, const std::vector<OverscaledTileID>& tileIDs) {
    std::set<CanonicalTileID> covered;
    for (const auto& tileID : tileIDs) {
        if (tileID.wrap != target.wrap) continue;
        if (tileCoversTarget(tileID, target)) return true;
        if (tileID.canonical.isChildOf(target.canonical)) covered.insert(tileID.canonical);
    }

    // Collapse four covered siblings into their parent until either the
    // target itself is proven or no more progress is possible. This is exact
    // like recursive subdivision, but work is bounded by the supplied tile
    // count rather than 4^(deepest unrelated zoom).
    while (!covered.empty()) {
        std::map<CanonicalTileID, uint8_t> childMasks;
        for (const auto& tileID : covered) {
            if (tileID.z <= target.canonical.z) continue;
            const auto parent = tileID.scaledTo(tileID.z - 1u);
            const uint8_t quadrant = static_cast<uint8_t>((tileID.x & 1u) | ((tileID.y & 1u) << 1u));
            childMasks[parent] |= static_cast<uint8_t>(1u << quadrant);
        }

        bool collapsed = false;
        for (const auto& [parent, mask] : childMasks) {
            if (mask != 0x0fu) continue;
            for (const auto& child : parent.children()) {
                covered.erase(child);
            }
            covered.insert(parent);
            collapsed = true;
        }
        if (covered.find(target.canonical) != covered.end()) return true;
        if (!collapsed) return false;
    }
    return false;
}

} // namespace mbgl::raster_drape
