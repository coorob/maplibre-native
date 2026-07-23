#include <mbgl/renderer/cover_hold_budget.hpp>
#include <mbgl/test/util.hpp>

#include <map>
#include <set>
#include <vector>

using namespace mbgl;

namespace {

std::set<UnwrappedTileID> ids(const std::vector<cover_hold::Candidate>& candidates) {
    std::set<UnwrappedTileID> result;
    for (const auto& candidate : candidates) result.insert(candidate.id);
    return result;
}

} // namespace

TEST(CoverHoldBudget, MirrorsTheActiveRasterDEMCoverCap) {
    EXPECT_EQ(cover_hold::rasterDEMCoverHoldBudget(112), 112u);
    EXPECT_EQ(cover_hold::rasterDEMCoverHoldBudget(384), 384u);
    EXPECT_EQ(cover_hold::rasterDEMCoverHoldBudget(0), cover_hold::rasterDEMFallbackBudget);
    EXPECT_EQ(cover_hold::rasterDEMPressureCoverHoldBudget(112), 56u);
    EXPECT_EQ(cover_hold::rasterDEMPressureCoverHoldBudget(384), 192u);
    EXPECT_EQ(cover_hold::rasterDEMPressureCoverHoldBudget(3), 2u);
    EXPECT_EQ(cover_hold::rasterDEMPressureCoverHoldBudget(1), 1u);
    EXPECT_EQ(cover_hold::rasterDEMPressureCoverHoldBudget(0), 0u);
    EXPECT_EQ(cover_hold::recentIdealBudget(112), 224u);
    EXPECT_EQ(cover_hold::recentIdealBudget(56), 112u);
}

TEST(CoverHoldBudget, KeepsOneCoverWhenBoundingNonRasterDEMFallbacksUnderPressure) {
    EXPECT_EQ(cover_hold::nonRasterDEMPressureCoverHoldBudget(16, 7), 16u);
    EXPECT_EQ(cover_hold::nonRasterDEMPressureCoverHoldBudget(16, 16), 16u);
    EXPECT_EQ(cover_hold::nonRasterDEMPressureCoverHoldBudget(16, 24), 24u);
    EXPECT_EQ(cover_hold::nonRasterDEMPressureCoverHoldBudget(0, 24), 0u);
}

TEST(CoverHoldBudget, KeepsTheYoungestCompleteCoverInsteadOfOlderHistory) {
    std::vector<cover_hold::Candidate> candidates;
    for (uint32_t x = 0; x < 4; ++x) {
        candidates.push_back({UnwrappedTileID{4, x, 0}, 0, 0});
        candidates.push_back({UnwrappedTileID{4, x, 1}, 8, 0});
    }

    const auto selected = cover_hold::selectCandidates(std::move(candidates), 4);
    EXPECT_EQ(selected.size(), 4u);
    EXPECT_EQ(
        ids(selected),
        (std::set<UnwrappedTileID>{
            UnwrappedTileID{4, 0, 0}, UnwrappedTileID{4, 1, 0}, UnwrappedTileID{4, 2, 0}, UnwrappedTileID{4, 3, 0}}));
}

TEST(CoverHoldBudget, UsesRecentOverlapThenCoverageThenTileIDAsStableTieBreakers) {
    const UnwrappedTileID newest{7, 10, 10};
    const UnwrappedTileID shallow{6, 6, 6};
    const UnwrappedTileID deep{8, 24, 24};
    const UnwrappedTileID stableFirst{7, 8, 8};
    const UnwrappedTileID stableSecond{7, 9, 8};

    const auto selected = cover_hold::selectCandidates(
        {{stableSecond, 2, 3}, {deep, 2, 2}, {shallow, 2, 2}, {newest, 2, 0}, {stableFirst, 2, 3}}, 5);

    ASSERT_EQ(selected.size(), 5u);
    EXPECT_EQ(selected[0].id, newest);
    EXPECT_EQ(selected[1].id, shallow);
    EXPECT_EQ(selected[2].id, deep);
    EXPECT_EQ(selected[3].id, stableFirst);
    EXPECT_EQ(selected[4].id, stableSecond);
}

TEST(CoverHoldBudget, MatchesExactParentAndChildRecentCover) {
    const UnwrappedTileID parent{9, 100, 100};
    const UnwrappedTileID exact{10, 200, 200};
    const UnwrappedTileID child{11, 400, 400};
    const UnwrappedTileID unrelated{10, 700, 700};
    const std::map<UnwrappedTileID, uint32_t> recent{{exact, 98}, {unrelated, 100}};

    EXPECT_EQ(cover_hold::newestOverlapAge(exact, recent, 100), 2u);
    EXPECT_EQ(cover_hold::newestOverlapAge(parent, recent, 100), 2u);
    EXPECT_EQ(cover_hold::newestOverlapAge(child, recent, 100), 2u);
    EXPECT_FALSE(cover_hold::newestOverlapAge(UnwrappedTileID{10, 300, 300}, recent, 100));
}

TEST(CoverHoldBudget, RetiresPreviousChildOnlyForRenderableCurrentIdealAncestor) {
    const UnwrappedTileID previous{12, 1600, 2400};
    const UnwrappedTileID parent{11, 800, 1200};
    const UnwrappedTileID grandparent{10, 400, 600};
    const UnwrappedTileID siblingParent{11, 801, 1200};
    const UnwrappedTileID child{13, 3200, 4800};

    EXPECT_TRUE(cover_hold::hasRenderableIdealAncestor(
        previous, std::set<UnwrappedTileID>{parent}, std::set<UnwrappedTileID>{parent}));
    EXPECT_TRUE(cover_hold::hasRenderableIdealAncestor(
        previous, std::set<UnwrappedTileID>{grandparent}, std::set<UnwrappedTileID>{grandparent}));

    EXPECT_FALSE(cover_hold::hasRenderableIdealAncestor(
        previous, std::set<UnwrappedTileID>{parent}, std::set<UnwrappedTileID>{}));
    EXPECT_FALSE(cover_hold::hasRenderableIdealAncestor(
        previous, std::set<UnwrappedTileID>{}, std::set<UnwrappedTileID>{parent}));
    EXPECT_FALSE(cover_hold::hasRenderableIdealAncestor(
        previous, std::set<UnwrappedTileID>{siblingParent}, std::set<UnwrappedTileID>{siblingParent}));
    EXPECT_FALSE(cover_hold::hasRenderableIdealAncestor(
        previous, std::set<UnwrappedTileID>{child}, std::set<UnwrappedTileID>{child}));
}

TEST(CoverHoldBudget, BoundsRecentIdealHistoryAndKeepsNewestIDsDeterministically) {
    std::map<UnwrappedTileID, uint32_t> recent{{UnwrappedTileID{5, 0, 0}, 96},
                                               {UnwrappedTileID{5, 1, 0}, 99},
                                               {UnwrappedTileID{5, 2, 0}, 100},
                                               {UnwrappedTileID{5, 3, 0}, 98},
                                               {UnwrappedTileID{5, 4, 0}, 99},
                                               {UnwrappedTileID{5, 5, 0}, 97}};

    EXPECT_EQ(cover_hold::boundRecentIdealTiles(recent, 100, 3), 3u);
    EXPECT_EQ(recent.size(), 3u);
    EXPECT_TRUE(recent.contains(UnwrappedTileID{5, 2, 0}));
    EXPECT_TRUE(recent.contains(UnwrappedTileID{5, 1, 0}));
    EXPECT_TRUE(recent.contains(UnwrappedTileID{5, 4, 0}));

    EXPECT_EQ(cover_hold::boundRecentIdealTiles(recent, 100, 0), 3u);
    EXPECT_TRUE(recent.empty());
}
