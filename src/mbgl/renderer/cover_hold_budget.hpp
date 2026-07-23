#pragma once

#include <mbgl/tile/tile_id.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace mbgl::cover_hold {

// Production RasterDEM covers are capped before they reach TilePyramid (112
// tiles on compact viewports and up to 384 on tablets). Retaining at most one
// additional cover gives terrain a complete loading fallback without allowing
// a long pan to accumulate every cover generation. A zero active-cover cap is
// reserved for controlled A/Bs; keep a stable tablet-sized safety bound there.
constexpr std::size_t rasterDEMFallbackBudget = 384;

constexpr std::size_t rasterDEMCoverHoldBudget(const std::size_t activeCoverCap) noexcept {
    return activeCoverCap > 0 ? activeCoverCap : rasterDEMFallbackBudget;
}

// DIAG: once iOS reports memory pressure, keep half of the normal custom
// RasterDEM continuity allowance. The active/renderable cover and ordinary
// fade holds are outside this budget. Rounding up preserves a useful fallback
// even for controlled tiny-cover tests.
constexpr std::size_t rasterDEMPressureCoverHoldBudget(const std::size_t normalBudget) noexcept {
    return normalBudget <= 1 ? normalBudget : normalBudget / 2 + normalBudget % 2;
}

constexpr std::size_t recentIdealBudget(const std::size_t coverHoldBudget) noexcept {
    return coverHoldBudget > std::numeric_limits<std::size_t>::max() / 2 ? std::numeric_limits<std::size_t>::max()
                                                                         : coverHoldBudget * 2;
}

inline bool overlaps(const UnwrappedTileID& lhs, const UnwrappedTileID& rhs) noexcept {
    return lhs == rhs || lhs.isChildOf(rhs) || rhs.isChildOf(lhs);
}

// Lower values are newer. Unsigned subtraction intentionally remains valid
// when the monotonically increasing update counter wraps.
inline std::optional<uint32_t> newestOverlapAge(const UnwrappedTileID& candidate,
                                                const std::map<UnwrappedTileID, uint32_t>& recentIdealTiles,
                                                const uint32_t currentUpdate) noexcept {
    std::optional<uint32_t> newest;
    for (const auto& [idealID, lastUpdate] : recentIdealTiles) {
        if (!overlaps(candidate, idealID)) continue;
        const uint32_t age = currentUpdate - lastUpdate;
        if (!newest || age < *newest) newest = age;
    }
    return newest;
}

struct Candidate {
    UnwrappedTileID id;
    uint16_t holdAge;
    uint32_t overlapAge;
};

inline bool candidatePriority(const Candidate& lhs, const Candidate& rhs) noexcept {
    // Prefer the immediately previous cover, then the area seen most recently.
    // Shallower tiles win exact ties because each preserves more fallback area.
    if (lhs.holdAge != rhs.holdAge) return lhs.holdAge < rhs.holdAge;
    if (lhs.overlapAge != rhs.overlapAge) return lhs.overlapAge < rhs.overlapAge;
    if (lhs.id.canonical.z != rhs.id.canonical.z) return lhs.id.canonical.z < rhs.id.canonical.z;
    return lhs.id < rhs.id;
}

inline std::vector<Candidate> selectCandidates(std::vector<Candidate> candidates, const std::size_t budget) {
    std::sort(candidates.begin(), candidates.end(), candidatePriority);
    if (candidates.size() > budget) {
        candidates.erase(candidates.begin() + static_cast<std::ptrdiff_t>(budget), candidates.end());
    }
    return candidates;
}

// RasterDEM is continuous at every source zoom. Once an exact tile from the
// current ideal cover is renderable, it can safely replace retained children
// from the previous, higher-zoom cover. Sparse vector sources cannot make the
// same promise because a coarse parent can be feature-empty.
template <typename RenderedTiles>
inline bool hasRenderableIdealAncestor(const UnwrappedTileID& previous,
                                       const std::set<UnwrappedTileID>& currentIdealTiles,
                                       const RenderedTiles& renderedTiles) {
    for (uint8_t ancestorZ = previous.canonical.z; ancestorZ > 0;) {
        --ancestorZ;
        const UnwrappedTileID ancestor{previous.wrap, previous.canonical.scaledTo(ancestorZ)};
        if (currentIdealTiles.find(ancestor) != currentIdealTiles.end() &&
            renderedTiles.find(ancestor) != renderedTiles.end()) {
            return true;
        }
    }
    return false;
}

// Keep current and immediately preceding ideal-cover IDs. The map is only an
// overlap index; evicting older entries does not touch active, fallback, or
// fading tiles directly.
inline std::size_t boundRecentIdealTiles(std::map<UnwrappedTileID, uint32_t>& recentIdealTiles,
                                         const uint32_t currentUpdate,
                                         const std::size_t budget) {
    if (recentIdealTiles.size() <= budget) return 0;

    std::vector<std::pair<UnwrappedTileID, uint32_t>> ordered(recentIdealTiles.begin(), recentIdealTiles.end());
    std::sort(ordered.begin(), ordered.end(), [currentUpdate](const auto& lhs, const auto& rhs) {
        const uint32_t lhsAge = currentUpdate - lhs.second;
        const uint32_t rhsAge = currentUpdate - rhs.second;
        if (lhsAge != rhsAge) return lhsAge < rhsAge;
        return lhs.first < rhs.first;
    });

    const std::size_t evicted = ordered.size() - budget;
    for (auto it = ordered.begin() + static_cast<std::ptrdiff_t>(budget); it != ordered.end(); ++it) {
        recentIdealTiles.erase(it->first);
    }
    return evicted;
}

} // namespace mbgl::cover_hold
