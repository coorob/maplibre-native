#include <mbgl/renderer/raster_drape_cohort.hpp>
#include <mbgl/test/util.hpp>

using namespace mbgl;

namespace {

OverscaledTileID tile(uint8_t z, uint32_t x, uint32_t y, int16_t wrap = 0) {
    return OverscaledTileID(z, wrap, CanonicalTileID(z, x, y));
}

} // namespace

TEST(RasterDrapeCohort, SelectsDeepestFloorAndExactFourChildren) {
    const auto target = tile(9, 277, 129);
    const auto children = target.canonical.children();
    std::vector<OverscaledTileID> candidates{
        tile(7, 69, 32),
        target,
        OverscaledTileID(10, target.wrap, children[2]),
        OverscaledTileID(10, target.wrap, children[0]),
        OverscaledTileID(10, target.wrap, children[3]),
        OverscaledTileID(10, target.wrap, children[1]),
    };

    const auto selection = raster_drape::selectCohort(target, candidates);
    ASSERT_TRUE(selection.floor);
    EXPECT_EQ(*selection.floor, target);
    EXPECT_TRUE(selection.hasCompleteChildren());
    for (std::size_t i = 0; i < children.size(); ++i) {
        ASSERT_TRUE(selection.children[i]);
        EXPECT_EQ(selection.children[i]->canonical, children[i]);
    }
}

TEST(RasterDrapeCohort, RejectsPartialDuplicateWrongWrapAndDeeperDetail) {
    const auto target = tile(9, 277, 129);
    const auto children = target.canonical.children();
    const auto first = OverscaledTileID(10, target.wrap, children[0]);

    EXPECT_FALSE(raster_drape::selectCohort(target, {first, first, first, first}).hasCompleteChildren());
    EXPECT_FALSE(raster_drape::selectCohort(target,
                                            {first,
                                             OverscaledTileID(10, 1, children[1]),
                                             OverscaledTileID(10, target.wrap, children[2]),
                                             OverscaledTileID(10, target.wrap, children[3])})
                     .hasCompleteChildren());
    EXPECT_FALSE(raster_drape::selectCohort(target,
                                            {tile(11, children[0].x * 2, children[0].y * 2),
                                             tile(11, children[1].x * 2, children[1].y * 2),
                                             tile(11, children[2].x * 2, children[2].y * 2),
                                             tile(11, children[3].x * 2, children[3].y * 2)})
                     .hasCompleteChildren());
}

TEST(RasterDrapeCohort, ProvesOnlyACompleteUnionCover) {
    const auto target = tile(8, 100, 70);
    const auto children = target.canonical.children();
    std::vector<OverscaledTileID> complete;
    for (const auto& child : children) {
        complete.emplace_back(child.z, target.wrap, child);
    }

    EXPECT_TRUE(raster_drape::coversTargetAtAnyResolution(target, {target}));
    EXPECT_TRUE(raster_drape::coversTargetAtAnyResolution(target, complete));
    complete.pop_back();
    EXPECT_FALSE(raster_drape::coversTargetAtAnyResolution(target, complete));
    EXPECT_FALSE(raster_drape::coversTargetAtAnyResolution(target, {tile(8, 100, 70, 1)}));
}

TEST(RasterDrapeCohort, IgnoresUnrelatedDeepTilesWithoutRecursiveExpansion) {
    const auto target = tile(8, 100, 70);
    EXPECT_FALSE(raster_drape::coversTargetAtAnyResolution(target, {tile(20, 1, 1)}));
}

TEST(RasterDrapeCohort, NeverPublishesAMixedRealAndEmptyDEMCover) {
    using Action = raster_drape::IncompleteDEMAction;
    EXPECT_EQ(raster_drape::selectIncompleteDEMAction(8, 0, false, false), Action::Publish);
    EXPECT_EQ(raster_drape::selectIncompleteDEMAction(8, 1, true, true), Action::HoldPrevious);
    EXPECT_EQ(raster_drape::selectIncompleteDEMAction(8, 1, false, false), Action::DropTerrain);
    EXPECT_EQ(raster_drape::selectIncompleteDEMAction(8, 1, true, false), Action::DropTerrain);
}

TEST(RasterDrapeCohort, OnlyTheCompleteSatelliteBaseControlsRasterReadiness) {
    EXPECT_TRUE(raster_drape::isAuthoritativeTerrainRaster("satellite-terrain-layer", "satellite-terrain-raster"));
    EXPECT_FALSE(raster_drape::isAuthoritativeTerrainRaster("satellite-orto-layer", "satellite-orto-raster"));
    EXPECT_FALSE(raster_drape::isAuthoritativeTerrainRaster("satellite-orto-layer", "satellite-terrain-raster"));
    EXPECT_FALSE(raster_drape::isAuthoritativeTerrainRaster("satellite-orto-detail-layer",
                                                          "satellite-orto-detail-raster"));
    EXPECT_FALSE(raster_drape::isAuthoritativeTerrainRaster("satellite-finland-orto-layer",
                                                          "satellite-finland-orto-raster"));
    EXPECT_FALSE(raster_drape::isAuthoritativeTerrainRaster("satellite-finland-orto-detail-layer",
                                                          "satellite-finland-orto-detail-raster"));
    EXPECT_FALSE(raster_drape::isAuthoritativeTerrainRaster("satellite-terrain-layer",
                                                          "satellite-finland-orto-raster"));
    EXPECT_FALSE(raster_drape::isAuthoritativeTerrainRaster("satellite-finland-orto-layer",
                                                          "satellite-terrain-raster"));
}

TEST(RasterDrapeCohort, RequiresAnAuthoritativeSourceBeforePublishing) {
    EXPECT_FALSE(raster_drape::hasAuthoritativePublicationRequirement(false, 0));
    EXPECT_TRUE(raster_drape::hasAuthoritativePublicationRequirement(false, 1));
    EXPECT_TRUE(raster_drape::hasAuthoritativePublicationRequirement(true, 0));
}

TEST(RasterDrapeCohort, RetriesAHeldNewerPublicationGeneration) {
    EXPECT_FALSE(raster_drape::publicationNeedsFrameRetry(0, 0, false, 0));
    EXPECT_FALSE(raster_drape::publicationNeedsFrameRetry(7, 7, false, 1));
    EXPECT_FALSE(raster_drape::publicationNeedsFrameRetry(8, 7, false, 0));
    EXPECT_TRUE(raster_drape::publicationNeedsFrameRetry(8, 7, false, 1));
    EXPECT_TRUE(raster_drape::publicationNeedsFrameRetry(8, 7, true, 0));
}

TEST(RasterDrapeCohort, AutomaticallyRetriesOnlyWorkARenderedFrameCanAdvance) {
    // The current generation has not been attempted yet.
    EXPECT_TRUE(raster_drape::publicationNeedsAutomaticFrameRetry(8, 7, false, 1, 7, true));

    // The same generation already proved it needs a source/style update.
    EXPECT_FALSE(raster_drape::publicationNeedsAutomaticFrameRetry(8, 7, false, 1, 8, true));

    // A same-generation attempt that can advance on another rendered frame
    // remains eligible (for example a newly allocated ready-to-bake target).
    EXPECT_TRUE(raster_drape::publicationNeedsAutomaticFrameRetry(8, 7, false, 1, 8, false));

    // No authoritative work or no newer generation never self-wakes.
    EXPECT_FALSE(raster_drape::publicationNeedsAutomaticFrameRetry(8, 7, false, 0, 7, false));
    EXPECT_FALSE(raster_drape::publicationNeedsAutomaticFrameRetry(8, 8, false, 1, 7, false));
}

TEST(RasterDrapeCohort, UnbakedTargetHonoursMinimumWithoutSpinningOnExternalBlockers) {
    EXPECT_TRUE(raster_drape::unbakedTargetNeedsAutomaticFrame(0, 1, 0, 0, false));
    EXPECT_FALSE(raster_drape::unbakedTargetNeedsAutomaticFrame(0, 1, 8, 8, true));
    EXPECT_TRUE(raster_drape::unbakedTargetNeedsAutomaticFrame(0, 1, 8, 7, true));
    EXPECT_TRUE(raster_drape::unbakedTargetNeedsAutomaticFrame(1, 2, 8, 8, false));
    EXPECT_FALSE(raster_drape::unbakedTargetNeedsAutomaticFrame(2, 2, 8, 8, false));
}

TEST(RasterDrapeCohort, RasterTransactionReadinessIsExactPerTarget) {
    const auto target = tile(9, 277, 129);
    const auto parent = tile(8, 138, 64);
    std::vector<OverscaledTileID> childTargets;
    for (const auto& child : target.canonical.children()) {
        childTargets.emplace_back(child.z, target.wrap, child);
    }

    EXPECT_TRUE(raster_drape::containsExactTarget(target, {target}));
    EXPECT_FALSE(raster_drape::containsExactTarget(target, {parent}));
    EXPECT_FALSE(raster_drape::containsExactTarget(target, childTargets));
}

TEST(RasterDrapeCohort, SameTileIDDifferentTextureIdentityIsNotCurrent) {
    int firstTexture = 1;
    int replacementTexture = 2;

    EXPECT_TRUE(raster_drape::textureIdentityMatches(&firstTexture, &firstTexture));
    EXPECT_FALSE(raster_drape::textureIdentityMatches(&replacementTexture, &firstTexture));
    EXPECT_FALSE(raster_drape::textureIdentityMatches(nullptr, &firstTexture));
}

TEST(RasterDrapeCohort, RequiresARealImageOrRetainedTextureSource) {
    RasterBucket empty(PremultipliedImage{});
    RasterBucket image(PremultipliedImage{Size{1, 1}});

    EXPECT_FALSE(raster_drape::bucketHasTextureSource(empty));
    EXPECT_TRUE(raster_drape::bucketHasTextureSource(image));
}

TEST(RasterDrapeCohort, MosaicRoutesFloorAndFinerTilesWithoutRequiringAFloor) {
    const auto target = tile(12, 2176, 1432);
    const auto child = OverscaledTileID(13, 0, target.canonical.children()[1]);
    const auto grandchild = OverscaledTileID(15, 0, CanonicalTileID(15, 2176 * 8 + 3, 1432 * 8 + 5));
    const auto tooDeep = OverscaledTileID(16, 0, CanonicalTileID(16, 2176 * 16 + 3, 1432 * 16 + 5));
    const auto outside = tile(13, 4354, 2864);
    const auto otherWrap = OverscaledTileID(13, 1, target.canonical.children()[0]);

    // The aerial blend near a close camera: only tiles finer than the target.
    auto mosaic = raster_drape::selectMosaic(target, {child, grandchild, tooDeep, outside, otherWrap}, 3);
    EXPECT_FALSE(mosaic.floor);
    ASSERT_EQ(mosaic.details.size(), 2u);
    EXPECT_EQ(mosaic.details[0], child);
    EXPECT_EQ(mosaic.details[1], grandchild);

    // The deepest ancestor-or-equal tile is the floor.
    mosaic = raster_drape::selectMosaic(target, {tile(10, 544, 358), target, child}, 3);
    ASSERT_TRUE(mosaic.floor);
    EXPECT_EQ(*mosaic.floor, target);
    EXPECT_EQ(mosaic.details.size(), 1u);
}

TEST(RasterDrapeCohort, MosaicRoutesOneTilePerCanonicalID) {
    const auto target = tile(12, 2176, 1432);
    const auto canonical = target.canonical.children()[2];
    const auto overscaled = OverscaledTileID(15, 0, canonical);
    const auto exact = OverscaledTileID(13, 0, canonical);
    const auto mosaic = raster_drape::selectMosaic(target, {overscaled, exact}, 3);
    ASSERT_EQ(mosaic.details.size(), 1u);
    EXPECT_EQ(mosaic.details[0].canonical, canonical);
}

TEST(RasterDrapeCohort, MosaicKeepsStaleTilesUntilTheirAreaIsRepainted) {
    const auto target = tile(12, 2176, 1432);
    const auto children = target.canonical.children();
    const auto staleChild = OverscaledTileID(13, 0, children[0]);
    const auto oldFloor = tile(11, 1088, 716);

    // Nothing routed repaints the stale child or the old floor.
    raster_drape::Mosaic empty;
    EXPECT_FALSE(raster_drape::mosaicSupersedes(empty, target, staleChild));
    EXPECT_FALSE(raster_drape::mosaicSupersedes(empty, target, oldFloor));

    // A new floor replaces an old floor, but cannot replace detail: dropping
    // the sharper child would blur its area until finer tiles reload.
    raster_drape::Mosaic withFloor;
    withFloor.floor = target;
    EXPECT_TRUE(raster_drape::mosaicSupersedes(withFloor, target, oldFloor));
    EXPECT_FALSE(raster_drape::mosaicSupersedes(withFloor, target, staleChild));

    // Three of four grandchildren leave a hole; all four repaint the child.
    raster_drape::Mosaic finer;
    const auto grandchildren = children[0].children();
    for (std::size_t i = 0; i < 3; ++i) finer.details.emplace_back(14, 0, grandchildren[i]);
    EXPECT_FALSE(raster_drape::mosaicSupersedes(finer, target, staleChild));
    finer.details.emplace_back(14, 0, grandchildren[3]);
    EXPECT_TRUE(raster_drape::mosaicSupersedes(finer, target, staleChild));

    // A routed tile at the same or coarser zoom containing it does too.
    raster_drape::Mosaic same;
    same.details.push_back(staleChild);
    EXPECT_TRUE(raster_drape::mosaicSupersedes(same, target, staleChild));
}
