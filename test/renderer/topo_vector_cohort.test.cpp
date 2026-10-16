#include <mbgl/renderer/topo_vector_cohort.hpp>
#include <mbgl/renderer/topo_drape_detail_policy.hpp>
#include <mbgl/test/util.hpp>

#include <initializer_list>
#include <string_view>
#include <utility>

using namespace mbgl;

namespace {

OverscaledTileID tile(uint8_t z, uint32_t x, uint32_t y, int16_t wrap = 0) {
    return OverscaledTileID(z, wrap, CanonicalTileID(z, x, y));
}

// (layer ID, source ID) of every render item with a source, as RenderTerrain
// scans them.
topo_vector::DrapeBasemaps drapeBasemapsFor(
    std::initializer_list<std::pair<std::string_view, std::string_view>> renderedLayers) {
    topo_vector::DrapeBasemaps basemaps;
    for (const auto& [layerID, sourceID] : renderedLayers) {
        basemaps.addLayer(layerID, sourceID);
    }
    return basemaps;
}

} // namespace

TEST(TopoVectorCohort, DiagnosticDetailPolicyAcceptsOnlyExactSupportedLevels) {
    const char* defaultValues[] = {
        nullptr, "", "10", "9", "13", "99", " 12", "12 ", "12\n", "12.0", "12x", "unknown",
    };
    for (const char* value : defaultValues) {
        const auto policy = topo_vector::drapeZoomPolicyForOverride(value);
        EXPECT_EQ(policy.detailZoom, 10);
        EXPECT_EQ(policy.overviewZoom, 9);
    }
    for (const char* value : {"11", "12"}) {
        const auto policy = topo_vector::drapeZoomPolicyForOverride(value);
        EXPECT_EQ(policy.detailZoom, value[1] == '1' ? 11 : 12);
        EXPECT_EQ(policy.detailZoom - policy.overviewZoom, 1);
    }
}

TEST(TopoVectorCohort, DiagnosticDetailPolicyKeepsBudgetedQuartetsAtEveryLevel) {
    for (const char* value : {"10", "11", "12"}) {
        const auto policy = topo_vector::drapeZoomPolicyForOverride(value);
        std::unordered_set<OverscaledTileID> leaves;
        // Nine complete sibling groups: the unchanged 32-detail budget keeps
        // eight groups; the last becomes one overview parent, never 64 children.
        for (uint32_t group = 0; group < 9; ++group) {
            for (uint32_t dx = 0; dx < 2; ++dx) {
                for (uint32_t dy = 0; dy < 2; ++dy) {
                    leaves.insert(tile(policy.detailZoom, 400 + group * 4 + dx, 200 + dy));
                }
            }
        }
        const auto cohort = topo_vector::distanceLimitedCohortForTerrainLeaves(
            leaves, policy.detailZoom, policy.overviewZoom, 32,
            400.5 / std::exp2(policy.detailZoom), 200.5 / std::exp2(policy.detailZoom));
        EXPECT_EQ(cohort.size(), 33u);
        EXPECT_EQ(std::count_if(cohort.begin(), cohort.end(), [&](const auto& id) {
            return id.canonical.z == policy.detailZoom;
        }), 32);
        EXPECT_EQ(std::count_if(cohort.begin(), cohort.end(), [&](const auto& id) {
            return id.canonical.z == policy.overviewZoom;
        }), 1);
        for (const auto& a : cohort) {
            for (const auto& b : cohort) {
                if (a.canonical.z > b.canonical.z) EXPECT_FALSE(a.canonical.isChildOf(b.canonical));
            }
        }
    }
}

TEST(TopoVectorCohort, DiagnosticDetailKeepsDetailedSourceIDsAndLowerCameraLeaves) {
    const auto policy = topo_vector::drapeZoomPolicyForOverride("12");
    std::unordered_set<OverscaledTileID> exact{
        tile(12, 2230, 1038), tile(12, 2231, 1038),
        tile(11, 1200, 600), tile(9, 200, 100),
    };
    const auto sourceIDs = topo_vector::cohortForTerrainLeaves(exact, policy.detailZoom);
    EXPECT_EQ(std::set<OverscaledTileID>(sourceIDs.begin(), sourceIDs.end()),
              (std::set<OverscaledTileID>(exact.begin(), exact.end())));
}

TEST(TopoVectorCohort, DiagnosticMixedResolutionCoverKeepsNaturalLeavesOutsideDetailBudget) {
    const auto policy = topo_vector::drapeZoomPolicyForOverride("12");
    const std::set<OverscaledTileID> natural{
        tile(11, 1200, 600), tile(10, 500, 300), tile(9, 100, 100),
    };
    std::unordered_set<OverscaledTileID> leaves(natural.begin(), natural.end());
    for (uint32_t group = 0; group < 9; ++group) {
        for (uint32_t dx = 0; dx < 2; ++dx) {
            for (uint32_t dy = 0; dy < 2; ++dy) {
                leaves.insert(tile(12, 1600 + group * 4 + dx, 1000 + dy));
            }
        }
    }
    const auto cohort = topo_vector::distanceLimitedCohortForTerrainLeaves(
        leaves, policy.detailZoom, policy.overviewZoom, 32, 1600.5 / 4096.0, 1000.5 / 4096.0);
    const std::set<OverscaledTileID> actual(cohort.begin(), cohort.end());
    // 32 detailed IDs + one collapsed quartet + three natural lower leaves.
    // The detail budget is not a global target cap; the diagnostic must expose
    // this cost instead of pretending lower-resolution terrain disappeared.
    EXPECT_EQ(actual.size(), 36u);
    EXPECT_EQ(std::count_if(cohort.begin(), cohort.end(), [](const auto& id) {
        return id.canonical.z == 12;
    }), 32);
    for (const auto& naturalID : natural) EXPECT_EQ(actual.count(naturalID), 1u);
    for (const auto& a : cohort) {
        for (const auto& b : cohort) {
            if (a.canonical.z > b.canonical.z) EXPECT_FALSE(a.canonical.isChildOf(b.canonical));
        }
    }
}

TEST(TopoVectorCohort, CollapsesFinerTerrainLeavesToUniqueDetailParents) {
    std::unordered_set<OverscaledTileID> leaves{
        tile(12, 400, 800),
        tile(12, 401, 800),
        tile(12, 402, 801),
        tile(12, 403, 803),
    };

    const auto cohort = topo_vector::cohortForTerrainLeaves(leaves, 10);

    ASSERT_EQ(cohort.size(), 1u);
    EXPECT_EQ(cohort.front(), tile(10, 100, 200));
}

TEST(TopoVectorCohort, PreservesWrapsAndDistinctDetailParents) {
    std::unordered_set<OverscaledTileID> leaves{
        tile(11, 200, 400),
        tile(11, 202, 400),
        tile(11, 200, 400, 1),
    };

    const auto cohort = topo_vector::cohortForTerrainLeaves(leaves, 10);

    EXPECT_EQ(cohort,
              (std::vector<OverscaledTileID>{tile(10, 100, 200), tile(10, 101, 200), tile(10, 100, 200, 1)}));
}

TEST(TopoVectorCohort, KeepsLowZoomOverviewTargetsAtTheirNaturalZoom) {
    std::unordered_set<OverscaledTileID> leaves{
        tile(8, 100, 70),
        tile(10, 401, 281),
    };

    const auto cohort = topo_vector::cohortForTerrainLeaves(leaves, 10);

    EXPECT_EQ(cohort, (std::vector<OverscaledTileID>{tile(8, 100, 70), tile(10, 401, 281)}));
}

TEST(TopoVectorCohort, BoundsSharedDrapeTargetsToOneParentLevel) {
    std::unordered_set<OverscaledTileID> leaves{
        tile(12, 400, 800),
        tile(12, 401, 800),
        tile(13, 804, 1604),
        tile(10, 101, 202),
    };

    const auto cohort = topo_vector::cohortForTerrainLeaves(leaves, 10, 1);

    EXPECT_EQ(cohort,
              (std::vector<OverscaledTileID>{tile(10, 101, 202), tile(11, 200, 400), tile(12, 402, 802)}));
}

TEST(TopoVectorCohort, KeepsNearestSiblingGroupDetailedAndCollapsesFarGroupsAtomically) {
    std::unordered_set<OverscaledTileID> leaves{
        tile(10, 200, 200),
        tile(10, 201, 200),
        tile(10, 200, 201),
        tile(10, 201, 201),
        tile(10, 600, 600),
        tile(10, 601, 600),
        tile(10, 600, 601),
        tile(10, 601, 601),
    };

    const auto cohort = topo_vector::distanceLimitedCohortForTerrainLeaves(
        leaves,
        10,
        9,
        4,
        200.5 / 1024.0,
        200.5 / 1024.0);

    const std::set<OverscaledTileID> actual(cohort.begin(), cohort.end());
    EXPECT_EQ(actual,
              (std::set<OverscaledTileID>{
                  tile(9, 300, 300),
                  tile(10, 200, 200),
                  tile(10, 201, 200),
                  tile(10, 200, 201),
                  tile(10, 201, 201),
              }));
}

TEST(TopoVectorCohort, PreservesNaturalOverviewLeavesBesideAdaptiveGroups) {
    std::unordered_set<OverscaledTileID> leaves{
        tile(8, 50, 50),
        tile(10, 200, 200),
        tile(10, 201, 200),
    };

    const auto cohort = topo_vector::distanceLimitedCohortForTerrainLeaves(
        leaves,
        10,
        9,
        0,
        0.5,
        0.5);

    EXPECT_EQ(cohort, (std::vector<OverscaledTileID>{tile(8, 50, 50), tile(9, 100, 100)}));
}

TEST(TopoVectorCohort, SelectsOnlyOfficialTopoWithoutSatelliteHandoff) {
    EXPECT_TRUE(topo_vector::isOfficialSourceID("topo"));
    EXPECT_TRUE(topo_vector::isOfficialSourceID("norway-topo-vector"));
    EXPECT_FALSE(topo_vector::isOfficialSourceID("openfreemap"));
    EXPECT_FALSE(topo_vector::isOfficialSourceID("finland-openmaptiles"));
    EXPECT_FALSE(topo_vector::isOfficialSourceID("finland-nls-topography-northwest"));

    EXPECT_TRUE(topo_vector::useSharedDrapeCohort(true, false));
    EXPECT_FALSE(topo_vector::useSharedDrapeCohort(true, true));
    EXPECT_FALSE(topo_vector::useSharedDrapeCohort(false, false));
}

TEST(TopoVectorCohort, AerialBlendFlightDrapesAsSatelliteDespiteItsTopoLabels) {
    // buildBlendedImageryStyle({terrain3D: true}) in Sweden: the per-country
    // overviews, the orthophoto preview, and the Topo style's labels it keeps
    // above the photos. Before .104 the `topo` labels made this a Topo cohort.
    const auto blend = drapeBasemapsFor({
        {"imagery-sweden-overview", "imagery-sweden-overview"},
        {"imagery-finland-overview", "imagery-finland-overview"},
        {"satellite-orto-layer", "satellite-orto-raster"},
        {"place-label-major", "topo"},
        {"place-label-terrain", "topo"},
        {"ofm-place-city", "openfreemap"},
    });
    EXPECT_TRUE(blend.officialTopo);
    EXPECT_TRUE(blend.satelliteImagery);
    EXPECT_FALSE(blend.useSharedCohort());

    // Any one imagery layer in the render tree is enough, in either order.
    for (const auto& [imageryLayer, imagerySource] : {
             std::pair<std::string_view, std::string_view>{"imagery-sweden-overview", "imagery-sweden-overview"},
             {"satellite-orto-layer", "satellite-orto-raster"},
             {"satellite-orto-detail-layer", "satellite-orto-detail-raster"},
         }) {
        EXPECT_FALSE(
            drapeBasemapsFor({{imageryLayer, imagerySource}, {"place-label-major", "topo"}}).useSharedCohort());
        EXPECT_FALSE(
            drapeBasemapsFor({{"place-label-major", "topo"}, {imageryLayer, imagerySource}}).useSharedCohort());
    }

    // The Finnish flight keeps the Topo sources' labels and Finland's base too.
    EXPECT_FALSE(drapeBasemapsFor({
                                      {"imagery-finland-overview", "imagery-finland-overview"},
                                      {"satellite-orto-layer", "satellite-orto-raster"},
                                      {"waterway-river", "finland-openmaptiles"},
                                      {"place-label-major", "topo"},
                                  })
                     .useSharedCohort());
}

TEST(TopoVectorCohort, OfficialTopoKeepsItsSharedCohort) {
    const auto sweden = drapeBasemapsFor({
        {"ofm-water", "openfreemap"},
        {"land-open", "topo"},
        {"land-aker", "topo"},
        {"strandlinje", "topo"},
        {"hojdkurva-10m", "topo"},
        {"place-label-major", "topo"},
    });
    EXPECT_TRUE(sweden.officialTopo);
    EXPECT_FALSE(sweden.satelliteImagery);
    EXPECT_TRUE(sweden.useSharedCohort());

    EXPECT_TRUE(drapeBasemapsFor({{"hojdkurva", "norway-topo-vector"}}).useSharedCohort());
    // Layer IDs alone do not make imagery: the source must match its layer.
    EXPECT_TRUE(drapeBasemapsFor({{"land-open", "topo"}, {"satellite-orto-layer", "topo"}}).useSharedCohort());
    EXPECT_TRUE(drapeBasemapsFor({{"land-open", "topo"}, {"imagery-sweden-overview", "topo"}}).useSharedCohort());
}

TEST(TopoVectorCohort, SatelliteStylesAndHandoffsKeepTheFineTopology) {
    // Esri satellite has no Topo source at all.
    const auto esri = drapeBasemapsFor({{"satellite-terrain-layer", "satellite-terrain-raster"}});
    EXPECT_FALSE(esri.officialTopo);
    EXPECT_TRUE(esri.satelliteImagery);
    EXPECT_FALSE(esri.useSharedCohort());

    // The private aerial experiment (buildAerialTerrainStyle) inserts the
    // photos into the full Topo style, fills included.
    EXPECT_FALSE(drapeBasemapsFor({
                                      {"land-open", "topo"},
                                      {"satellite-orto-layer", "satellite-orto-raster"},
                                      {"satellite-finland-orto-layer", "satellite-finland-orto-raster"},
                                      {"place-label-major", "topo"},
                                  })
                     .useSharedCohort());

    // A Topo <-> satellite style handoff keeps the fine topology until the
    // imagery has left the render tree.
    EXPECT_FALSE(drapeBasemapsFor({{"land-open", "topo"}, {"satellite-terrain-layer", "satellite-terrain-raster"}})
                     .useSharedCohort());

    // Germany has no official Topo, with or without imagery.
    EXPECT_FALSE(drapeBasemapsFor({{"landcover", "germany-basemap"}}).useSharedCohort());
    EXPECT_FALSE(drapeBasemapsFor({}).useSharedCohort());
}

TEST(TopoVectorCohort, FillPublicationPreservesSwedishAndNorwegianSourcesOnEveryBackend) {
    for (const bool androidOpenGL : {false, true}) {
        EXPECT_TRUE(topo_vector::isAuthoritativeFillSourceID("topo", androidOpenGL));
        EXPECT_TRUE(topo_vector::isAuthoritativeFillSourceID("norway-topo-vector", androidOpenGL));
    }
}

TEST(TopoVectorCohort, FinnishBasePublishesOnlyOnAndroidOpenGLWithoutChangingSharedCohorts) {
    EXPECT_TRUE(topo_vector::isAuthoritativeFillSourceID("finland-openmaptiles", true));
    EXPECT_FALSE(topo_vector::isAuthoritativeFillSourceID("finland-openmaptiles", false));
    EXPECT_FALSE(topo_vector::isOfficialSourceID("finland-openmaptiles"));
    EXPECT_FALSE(topo_vector::useSharedDrapeCohort(
        topo_vector::isOfficialSourceID("finland-openmaptiles"), false));
}

TEST(TopoVectorCohort, FillPublicationRejectsOptionalOverlaysAndNearMatchingSourceIDs) {
    for (const std::string_view sourceID : {
             "", "openfreemap", "finland-openmaptiles-detail", "finland-openmaptiles ",
             "Finland-openmaptiles", "finland-nls-topography-northwest",
             "finland-nls-topography-southwest", "satellite-orto-raster",
             "satellite-orto-detail-raster", "satellite-finland-orto-raster",
             "satellite-finland-orto-detail-raster", "user-facilities"}) {
        EXPECT_FALSE(topo_vector::isAuthoritativeFillSourceID(sourceID, false));
        EXPECT_FALSE(topo_vector::isAuthoritativeFillSourceID(sourceID, true));
    }
}

TEST(TopoVectorCohort, FinnishSourceCoverPreservesNaturalKoliDrapeTargetZooms) {
    // The measured missing Koli target is inside the national source bounds.
    // A source-max clamp must preserve it and finer natural terrain leaves,
    // unlike the separate Swedish/Norwegian shared-canvas detail policy.
    const std::unordered_set<OverscaledTileID> naturalTargets{
        tile(8, 148, 70), tile(9, 299, 140), tile(10, 594, 277),
        tile(11, 1190, 554), tile(12, 2388, 1121), tile(12, 2388, 1121, 1),
    };
    const auto sourceCover = topo_vector::cohortForTerrainLeaves(naturalTargets, 14);
    EXPECT_EQ(std::set<OverscaledTileID>(sourceCover.begin(), sourceCover.end()),
              (std::set<OverscaledTileID>(naturalTargets.begin(), naturalTargets.end())));
    for (const auto& a : sourceCover) {
        for (const auto& b : sourceCover) {
            if (a.wrap == b.wrap && a.canonical.z > b.canonical.z) {
                EXPECT_FALSE(a.canonical.isChildOf(b.canonical));
            }
        }
    }
}

TEST(TopoVectorCohort, FinnishSourceCoverClampsOnlyTargetsAboveSourceMaximum) {
    const std::unordered_set<OverscaledTileID> targets{
        tile(9, 299, 140), tile(15, 19104, 8972), tile(15, 19105, 8973),
    };
    const auto sourceCover = topo_vector::cohortForTerrainLeaves(targets, 14);
    EXPECT_EQ(sourceCover, (std::vector<OverscaledTileID>{
        tile(9, 299, 140), tile(14, 9552, 4486),
    }));
}

#include <mbgl/renderer/topo_vector_cohort.hpp>
#include <mbgl/test/util.hpp>

#include <algorithm>
#include <set>
#include <tuple>
#include <unordered_set>
#include <vector>

using namespace mbgl;

namespace {
using FinnishReviewLeaves = std::unordered_set<OverscaledTileID>;
using FinnishReviewCell = std::tuple<int16_t, uint32_t, uint32_t>;

OverscaledTileID finnishReviewTile(uint8_t z, uint32_t x, uint32_t y, int16_t wrap = 0) {
    return OverscaledTileID(z, wrap, CanonicalTileID(z, x, y));
}

void finnishReviewQuartet(FinnishReviewLeaves& leaves, uint32_t x, uint32_t y, int16_t wrap = 0) {
    for (uint32_t dx = 0; dx < 2; ++dx) {
        for (uint32_t dy = 0; dy < 2; ++dy) {
            leaves.insert(finnishReviewTile(12, x + dx, y + dy, wrap));
        }
    }
}

// Independent area/overlap oracle: expand physical tiles into disjoint cells.
// It does not call either production overlap/ancestor helper or scaledTo().
std::set<FinnishReviewCell> finnishReviewCells(const FinnishReviewLeaves& leaves,
                                            uint8_t cellZoom = 16) {
    std::set<FinnishReviewCell> cells;
    for (const auto& leaf : leaves) {
        EXPECT_LE(leaf.canonical.z, cellZoom);
        if (leaf.canonical.z > cellZoom) continue;
        const uint32_t size = uint32_t{1} << (cellZoom - leaf.canonical.z);
        for (uint32_t dx = 0; dx < size; ++dx) {
            for (uint32_t dy = 0; dy < size; ++dy) {
                const auto inserted = cells.emplace(leaf.wrap,
                                                    leaf.canonical.x * size + dx,
                                                    leaf.canonical.y * size + dy);
                EXPECT_TRUE(inserted.second) << "Overlapping physical leaf coverage";
            }
        }
    }
    return cells;
}

void finnishReviewInvariant(const FinnishReviewLeaves& base,
                           const topo_vector::FinlandSurfaceCover& cover,
                           std::size_t budget) {
    EXPECT_LE(cover.splitCount, budget);
    EXPECT_EQ(cover.drapeLeaves.size(), base.size());
    EXPECT_EQ(cover.meshLeaves.size(), base.size() + 3 * cover.splitCount);
    const auto original = finnishReviewCells(base);
    EXPECT_EQ(finnishReviewCells(cover.meshLeaves), original);
    EXPECT_EQ(finnishReviewCells(cover.drapeLeaves), original);
}
} // namespace

TEST(FinnishCloseSurfaceIndependent, GateRequiresAndroidFinnishCloseViewWithoutOfficialCohort) {
    using topo_vector::useFinnishCloseSurface;
    for (bool android : {false, true}) {
        for (bool finnish : {false, true}) {
            for (bool official : {false, true}) {
                for (bool shared : {false, true}) {
                    EXPECT_EQ(useFinnishCloseSurface(android, finnish, official, shared, 15, 12, 2048, 512),
                              android && finnish && !official && !shared);
                }
            }
        }
    }
    EXPECT_TRUE(useFinnishCloseSurface(true, true, false, false, 15, 12, 2048, 512));
    EXPECT_FALSE(useFinnishCloseSurface(false, true, false, false, 15, 12, 2048, 512));
    EXPECT_FALSE(useFinnishCloseSurface(true, false, false, false, 15, 12, 2048, 512));
    EXPECT_FALSE(useFinnishCloseSurface(true, true, true, false, 15, 12, 2048, 512));
    EXPECT_FALSE(useFinnishCloseSurface(true, true, false, true, 15, 12, 2048, 512));
    EXPECT_FALSE(useFinnishCloseSurface(true, true, false, false, 14, 12, 2048, 512));
    EXPECT_FALSE(useFinnishCloseSurface(true, true, false, false, 13, 12, 2048, 512));
    EXPECT_FALSE(useFinnishCloseSurface(true, true, false, false, 15, 12, 4096, 512));
    EXPECT_TRUE(useFinnishCloseSurface(true, true, false, false, 16, 12, 4096, 512));
    EXPECT_FALSE(useFinnishCloseSurface(true, true, false, false, 15, 12, 2048, 0));
    EXPECT_FALSE(useFinnishCloseSurface(true, true, false, false, 15, 12, 256, 512));
    EXPECT_FALSE(useFinnishCloseSurface(true, true, false, false, 12, 12, 512, 512));
    EXPECT_TRUE(useFinnishCloseSurface(true, true, false, false, 13, 12, 512, 512));
    EXPECT_FALSE(useFinnishCloseSurface(true, true, false, false, 13, 12, 1536, 512));
    EXPECT_TRUE(useFinnishCloseSurface(true, true, false, false, 14, 12, 1536, 512));
}

TEST(FinnishCloseSurfaceIndependent, TenSplitCapPreservesExactAreaAndCanvasCount) {
    FinnishReviewLeaves base;
    std::vector<OverscaledTileID> detail;
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t x = 100 + i * 4;
        base.insert(finnishReviewTile(12, x, 100));
        detail.push_back(finnishReviewTile(16, x * 16, 1600));
    }
    for (uint32_t i = 0; i < 10; ++i) finnishReviewQuartet(base, 1000 + i * 4, 1000);
    const auto cover = topo_vector::refineFinnishSurfaceCover(base, detail, 12, 10,
                                                            100.5 / 4096, 100.5 / 4096);
    EXPECT_EQ(cover.splitCount, 10u);
    EXPECT_EQ(base.size(), 44u);
    EXPECT_EQ(cover.meshLeaves.size(), 74u);
    EXPECT_EQ(cover.drapeLeaves.size(), 44u);
    // Fair allocation spends two levels across all four visible roots before
    // giving a third level to two roots; no request monopolizes four levels.
    const auto visible = topo_vector::inspectFinnishSurfaceDetail(cover.meshLeaves, detail);
    EXPECT_EQ(visible.requestedFootprints, 4u);
    EXPECT_EQ(visible.coveredFootprints, 4u);
    EXPECT_EQ(visible.resolvedFootprints, 0u);
    EXPECT_EQ(visible.minimumLeafZoom, 14u);
    EXPECT_EQ(visible.maximumLeafZoom, 15u);
    EXPECT_EQ(visible.remainingSplits, 6u);
    finnishReviewInvariant(base, cover, 10);
}

TEST(FinnishCloseSurfaceIndependent, OneSplitUsesFarthestCompleteDonorAndNearestDetail) {
    FinnishReviewLeaves base{finnishReviewTile(12, 100, 100)};
    finnishReviewQuartet(base, 1000, 1000);
    finnishReviewQuartet(base, 2000, 2000);
    const auto cover = topo_vector::refineFinnishSurfaceCover(
        base, {finnishReviewTile(16, 1600, 1600)}, 12, 1, 100.5 / 4096, 100.5 / 4096);
    EXPECT_EQ(cover.splitCount, 1u);
    EXPECT_TRUE(cover.drapeLeaves.count(finnishReviewTile(11, 1000, 1000)));
    EXPECT_FALSE(cover.drapeLeaves.count(finnishReviewTile(11, 500, 500)));
    EXPECT_TRUE(cover.drapeLeaves.count(finnishReviewTile(12, 1000, 1000)));
    EXPECT_TRUE(cover.meshLeaves.count(finnishReviewTile(12, 2000, 2000)));
    EXPECT_FALSE(cover.meshLeaves.count(finnishReviewTile(12, 100, 100)));
    EXPECT_TRUE(cover.meshLeaves.count(finnishReviewTile(13, 200, 200)));
    finnishReviewInvariant(base, cover, 1);
}

TEST(FinnishCloseSurfaceIndependent, IncompleteFarQuartetCannotFundRefinement) {
    FinnishReviewLeaves base{finnishReviewTile(12, 100, 100)};
    finnishReviewQuartet(base, 1000, 1000);
    base.erase(finnishReviewTile(12, 1001, 1001));
    const auto cover = topo_vector::refineFinnishSurfaceCover(
        base, {finnishReviewTile(16, 1600, 1600)}, 12, 10, 100.5 / 4096, 100.5 / 4096);
    EXPECT_EQ(cover.splitCount, 0u);
    EXPECT_EQ(cover.meshLeaves, base);
    EXPECT_EQ(cover.drapeLeaves, base);
    finnishReviewInvariant(base, cover, 10);
}

TEST(FinnishCloseSurfaceIndependent, AllDetailFootprintsProtectFarQuartets) {
    FinnishReviewLeaves base;
    finnishReviewQuartet(base, 100, 100);
    finnishReviewQuartet(base, 1000, 1000);
    std::vector<OverscaledTileID> detail;
    for (const auto& id : base) {
        detail.push_back(finnishReviewTile(16, id.canonical.x * 16, id.canonical.y * 16));
    }
    const auto cover = topo_vector::refineFinnishSurfaceCover(base, detail, 12, 10, 0.0, 0.0);
    EXPECT_EQ(cover.splitCount, 0u);
    EXPECT_EQ(cover.meshLeaves, base);
    EXPECT_EQ(cover.drapeLeaves, base);
    finnishReviewInvariant(base, cover, 10);
}

TEST(FinnishCloseSurfaceIndependent, OneDetailedChildProtectsItsWholeDonorQuartet) {
    FinnishReviewLeaves base{finnishReviewTile(12, 100, 100)};
    finnishReviewQuartet(base, 1000, 1000);
    const auto cover = topo_vector::refineFinnishSurfaceCover(
        base, {finnishReviewTile(16, 1600, 1600), finnishReviewTile(16, 16000, 16000)},
        12, 10, 100.5 / 4096, 100.5 / 4096);
    EXPECT_EQ(cover.splitCount, 0u);
    EXPECT_EQ(cover.meshLeaves, base);
    EXPECT_EQ(cover.drapeLeaves, base);
    finnishReviewInvariant(base, cover, 10);
}

TEST(FinnishCloseSurfaceIndependent, SameCanonicalCoordinatesInOtherWrapCanFundDetail) {
    FinnishReviewLeaves base{finnishReviewTile(12, 100, 100, 1)};
    finnishReviewQuartet(base, 100, 100, 0);
    const auto cover = topo_vector::refineFinnishSurfaceCover(
        base, {finnishReviewTile(16, 1600, 1600, 1)}, 12, 1,
        1.0 + 100.5 / 4096, 100.5 / 4096);
    EXPECT_EQ(cover.splitCount, 1u);
    EXPECT_TRUE(cover.drapeLeaves.count(finnishReviewTile(11, 50, 50, 0)));
    EXPECT_FALSE(cover.drapeLeaves.count(finnishReviewTile(11, 50, 50, 1)));
    EXPECT_TRUE(cover.meshLeaves.count(finnishReviewTile(13, 200, 200, 1)));
    EXPECT_TRUE(cover.meshLeaves.count(finnishReviewTile(12, 100, 100, 0)));
    finnishReviewInvariant(base, cover, 1);
}

TEST(FinnishCloseSurfaceIndependent, DifferentWrapsNeverCompleteOneDonorQuartet) {
    FinnishReviewLeaves base{finnishReviewTile(12, 100, 100),
                             finnishReviewTile(12, 1000, 1000, 0),
                             finnishReviewTile(12, 1001, 1000, 0),
                             finnishReviewTile(12, 1000, 1001, 1),
                             finnishReviewTile(12, 1001, 1001, 1)};
    const auto cover = topo_vector::refineFinnishSurfaceCover(
        base, {finnishReviewTile(16, 1600, 1600)}, 12, 10, 100.5 / 4096, 100.5 / 4096);
    EXPECT_EQ(cover.splitCount, 0u);
    EXPECT_EQ(cover.meshLeaves, base);
    EXPECT_EQ(cover.drapeLeaves, base);
    finnishReviewInvariant(base, cover, 10);
}

TEST(FinnishCloseSurfaceIndependent, ZeroBudgetNoDetailAndZeroDemZoomAreIdentity) {
    FinnishReviewLeaves base{finnishReviewTile(12, 100, 100)};
    finnishReviewQuartet(base, 1000, 1000);
    const std::vector<OverscaledTileID> detail{finnishReviewTile(16, 1600, 1600)};
    for (const auto& cover : {
             topo_vector::refineFinnishSurfaceCover(base, detail, 12, 0, 0.0, 0.0),
             topo_vector::refineFinnishSurfaceCover(base, {}, 12, 10, 0.0, 0.0),
             topo_vector::refineFinnishSurfaceCover(base, detail, 0, 10, 0.0, 0.0)}) {
        EXPECT_EQ(cover.splitCount, 0u);
        EXPECT_EQ(cover.meshLeaves, base);
        EXPECT_EQ(cover.drapeLeaves, base);
        finnishReviewInvariant(base, cover, 10);
    }
}

TEST(FinnishCloseSurfaceIndependent, SourceCoverClampsCanonicalAndDeduplicatesLargestLayoutPerWrap) {
    FinnishReviewLeaves leaves{finnishReviewTile(15, 200, 200),
                               finnishReviewTile(16, 402, 400),
                               finnishReviewTile(16, 403, 401),
                               finnishReviewTile(16, 404, 400),
                               finnishReviewTile(15, 200, 200, 1)};
    const auto cover = topo_vector::sourceCoverForTerrainSurfaceLeaves(leaves, 14);
    const std::set<OverscaledTileID> expected{
        OverscaledTileID(16, 0, CanonicalTileID(14, 100, 100)),
        OverscaledTileID(16, 0, CanonicalTileID(14, 101, 100)),
        OverscaledTileID(15, 1, CanonicalTileID(14, 100, 100))};
    EXPECT_EQ((std::set<OverscaledTileID>(cover.begin(), cover.end())), expected);
    EXPECT_EQ(cover.size(), 3u);
}

TEST(FinnishCloseSurfaceIndependent, OrdinarySourceCoverPreservesNaturalZoomWrapAndLayout) {
    FinnishReviewLeaves leaves{finnishReviewTile(9, 200, 100, -1),
                               finnishReviewTile(12, 1000, 1000),
                               OverscaledTileID(15, 1, CanonicalTileID(14, 8000, 4000))};
    const auto cover = topo_vector::sourceCoverForTerrainSurfaceLeaves(leaves, 14);
    EXPECT_EQ((std::set<OverscaledTileID>(cover.begin(), cover.end())),
              (std::set<OverscaledTileID>(leaves.begin(), leaves.end())));
}

TEST(FinnishCloseSurfaceIndependent, PlannedDrapeFindsExactOrClosestAncestorWithoutCrossingWrap) {
    const auto mesh = finnishReviewTile(12, 100, 100, 1);
    const auto parent = finnishReviewTile(11, 50, 50, 1);
    const auto grandparent = finnishReviewTile(10, 25, 25, 1);
    EXPECT_EQ(topo_vector::plannedDrapeForSurfaceMesh(mesh, {mesh, parent}), mesh);
    EXPECT_EQ(topo_vector::plannedDrapeForSurfaceMesh(mesh, {parent, grandparent}), parent);
    EXPECT_EQ(topo_vector::plannedDrapeForSurfaceMesh(mesh, {grandparent}), grandparent);
    EXPECT_FALSE(topo_vector::plannedDrapeForSurfaceMesh(mesh, {finnishReviewTile(11, 50, 50, 0)}));
    EXPECT_FALSE(topo_vector::plannedDrapeForSurfaceMesh(mesh, {}));
}

TEST(FinnishCloseSurfaceIndependent, PlannedReadyAndRetiredCleanupTruthTable) {
    for (bool different : {false, true}) {
        for (bool ready : {false, true}) {
            for (bool retired : {false, true}) {
                const auto selected = topo_vector::selectPlannedDrapeAncestor(different, ready, retired);
                EXPECT_EQ(selected.usePlannedAncestor, different && ready);
                EXPECT_EQ(selected.needsRetiredCleanupUpdate, different && ready && retired);
            }
        }
    }
}

TEST(FinnishCloseSurfaceIndependent, DuplicateAndReorderedRequestsDoNotSpendExtraBudget) {
    FinnishReviewLeaves base{finnishReviewTile(12, 100, 100), finnishReviewTile(12, 104, 100)};
    for (uint32_t i = 0; i < 10; ++i) finnishReviewQuartet(base, 1000 + 4 * i, 1000);
    const auto a = finnishReviewTile(16, 1600, 1600);
    const auto b = finnishReviewTile(16, 1664, 1600);
    const auto normal = topo_vector::refineFinnishSurfaceCover(base, {a, b}, 12, 10,
                                                             100.5 / 4096, 100.5 / 4096);
    const auto duplicated = topo_vector::refineFinnishSurfaceCover(base, {b, a, a, b}, 12, 10,
                                                                 100.5 / 4096, 100.5 / 4096);
    EXPECT_EQ(normal.splitCount, 8u);
    EXPECT_EQ(duplicated.splitCount, normal.splitCount);
    EXPECT_EQ(duplicated.meshLeaves, normal.meshLeaves);
    EXPECT_EQ(duplicated.drapeLeaves, normal.drapeLeaves);
    finnishReviewInvariant(base, duplicated, 10);
}

#include <mbgl/renderer/topo_vector_cohort.hpp>
#include <mbgl/test/util.hpp>

#include <unordered_set>

TEST(FinnishCloseSurfaceIndependent, AtomicRetirementCleanupCoversRefineToBaseLifecycle) {
    using IDs = std::unordered_set<int>;
    const IDs refinedChildren{101, 102, 103, 104};
    const IDs ordinaryBase{12};
    const IDs retiredChildren{101, 102, 103, 104};

    // The base successor is not ready: atomic hold keeps the old children
    // published. They must remain usable and must not spin cleanup updates.
    EXPECT_FALSE(mbgl::topo_vector::needsRetiredSurfaceCleanup(
        refinedChildren, refinedChildren, retiredChildren));

    // The ordinary base publishes after leaving the close-refinement gate.
    // This transition still needs one cleanup update for the retired children.
    EXPECT_TRUE(mbgl::topo_vector::needsRetiredSurfaceCleanup(
        refinedChildren, ordinaryBase, retiredChildren));

    // Next cleanup removes the old ownership. Stable base publication then
    // terminates the follow-up request, including unrelated retired IDs.
    EXPECT_FALSE(mbgl::topo_vector::needsRetiredSurfaceCleanup(ordinaryBase, ordinaryBase, IDs{}));
    EXPECT_FALSE(mbgl::topo_vector::needsRetiredSurfaceCleanup(ordinaryBase, ordinaryBase, IDs{999}));

    // Merely dropping an ID without retired ownership needs no cleanup work.
    EXPECT_FALSE(mbgl::topo_vector::needsRetiredSurfaceCleanup(refinedChildren, ordinaryBase, IDs{}));

    // The same ownership rule covers entering refinement as well as exiting.
    EXPECT_TRUE(mbgl::topo_vector::needsRetiredSurfaceCleanup(ordinaryBase, refinedChildren, IDs{12}));
    EXPECT_FALSE(mbgl::topo_vector::needsRetiredSurfaceCleanup(refinedChildren, refinedChildren, IDs{}));
}

TEST(FinnishCloseSurfaceIndependent, MixedNaturalLeavesStayWholeAndEveryMeshHasPlannedCanvas) {
    EXPECT_FALSE(topo_vector::useFinnishCloseSurface(true, true, false, false, 13, 12, 1024, 512));
    EXPECT_TRUE(topo_vector::useFinnishCloseSurface(true, true, false, false, 14, 12, 1024, 512));
    const FinnishReviewLeaves natural{
        finnishReviewTile(9, 10, 30), finnishReviewTile(10, 1000, 900),
        finnishReviewTile(11, 1800, 1600), finnishReviewTile(12, 100, 100, -1)};
    FinnishReviewLeaves base(natural);
    base.insert(finnishReviewTile(12, 100, 100));
    for (uint32_t i = 0; i < 4; ++i) finnishReviewQuartet(base, 1000 + i * 4, 1000);
    const auto cover = topo_vector::refineFinnishSurfaceCover(
        base, {finnishReviewTile(16, 1600, 1600)}, 12, 10, 100.5 / 4096, 100.5 / 4096);
    ASSERT_EQ(cover.splitCount, 4u);
    finnishReviewInvariant(base, cover, 10);
    const auto sourceVector = topo_vector::sourceCoverForTerrainSurfaceLeaves(cover.drapeLeaves, 14);
    const std::set<OverscaledTileID> sourceIDs(sourceVector.begin(), sourceVector.end());
    for (const auto& id : natural) {
        EXPECT_TRUE(cover.meshLeaves.count(id));
        EXPECT_TRUE(cover.drapeLeaves.count(id));
        EXPECT_TRUE(sourceIDs.count(id));
    }
    for (const auto& mesh : cover.meshLeaves) {
        const auto planned = topo_vector::plannedDrapeForSurfaceMesh(mesh, cover.drapeLeaves);
        ASSERT_TRUE(planned);
        EXPECT_EQ(planned->wrap, mesh.wrap);
        EXPECT_LE(planned->canonical.z, mesh.canonical.z);
        const auto delta = mesh.canonical.z - planned->canonical.z;
        EXPECT_EQ(mesh.canonical.x >> delta, planned->canonical.x);
        EXPECT_EQ(mesh.canonical.y >> delta, planned->canonical.y);
    }
}

// Independently authored complete-footprint and budget diagnostic fixtures.
#include <mbgl/renderer/topo_vector_cohort.hpp>
#include <mbgl/test/util.hpp>

#include <algorithm>
#include <set>
#include <tuple>
#include <unordered_set>
#include <vector>

using namespace mbgl;

namespace {
using ForegroundReviewLeaves = std::unordered_set<OverscaledTileID>;
using ForegroundReviewCell = std::tuple<int16_t, uint32_t, uint32_t>;

OverscaledTileID foregroundReviewTile(uint8_t z, uint32_t x, uint32_t y, int16_t wrap = 0) {
    return OverscaledTileID(z, wrap, CanonicalTileID(z, x, y));
}

void foregroundReviewDonors(ForegroundReviewLeaves& leaves, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        for (uint32_t x = 0; x < 2; ++x) {
            for (uint32_t y = 0; y < 2; ++y) {
                leaves.insert(foregroundReviewTile(12, 1000 + i * 4 + x, 1000 + y));
            }
        }
    }
}

// Independent physical-cell oracle: no production ancestor or overlap helper.
std::set<ForegroundReviewCell> foregroundReviewCells(const ForegroundReviewLeaves& leaves) {
    std::set<ForegroundReviewCell> cells;
    for (const auto& leaf : leaves) {
        EXPECT_LE(leaf.canonical.z, 16);
        if (leaf.canonical.z > 16) continue;
        const uint32_t width = uint32_t{1} << (16 - leaf.canonical.z);
        for (uint32_t x = 0; x < width; ++x) {
            for (uint32_t y = 0; y < width; ++y) {
                EXPECT_TRUE(cells.emplace(leaf.wrap, leaf.canonical.x * width + x,
                                          leaf.canonical.y * width + y).second);
            }
        }
    }
    return cells;
}

void foregroundReviewInvariant(const ForegroundReviewLeaves& base,
                               const topo_vector::FinlandSurfaceCover& refined) {
    EXPECT_LE(refined.splitCount, 10u);
    EXPECT_EQ(refined.drapeLeaves.size(), base.size());
    EXPECT_EQ(refined.meshLeaves.size(), base.size() + 3 * refined.splitCount);
    EXPECT_EQ(foregroundReviewCells(refined.meshLeaves), foregroundReviewCells(base));
    EXPECT_EQ(foregroundReviewCells(refined.drapeLeaves), foregroundReviewCells(base));
}
} // namespace

TEST(FinnishForegroundIndependent, TenVisibleFootprintsNeedTheTwoPreviouslyCutRequests) {
    // Synthetic 5x2 footprint, matching observed10/cap8 cardinality. The frozen
    // Nuuksio log does not identify the real kept/cut tile coordinates.
    ForegroundReviewLeaves base{foregroundReviewTile(12, 100, 100)};
    foregroundReviewDonors(base, 10);
    std::vector<OverscaledTileID> full;
    for (uint32_t x = 1600; x < 1605; ++x) {
        for (uint32_t y = 1600; y < 1602; ++y) full.push_back(foregroundReviewTile(16, x, y));
    }
    std::vector<OverscaledTileID> clipped(full.begin(), full.begin() + 8);
    const auto partial = topo_vector::refineFinnishSurfaceCover(base, clipped, 12, 10,
                                                               100.0 / 4096, 100.0 / 4096);
    const auto complete = topo_vector::refineFinnishSurfaceCover(base, full, 12, 10,
                                                                100.0 / 4096, 100.0 / 4096);
    const auto before = topo_vector::inspectFinnishSurfaceDetail(partial.meshLeaves, full);
    const auto after = topo_vector::inspectFinnishSurfaceDetail(complete.meshLeaves, full);
    EXPECT_EQ(partial.splitCount, 5u);
    EXPECT_EQ(before.requestedFootprints, 10u);
    EXPECT_EQ(before.coveredFootprints, 10u);
    EXPECT_EQ(before.resolvedFootprints, 8u);
    EXPECT_EQ(before.remainingSplits, 2u);
    EXPECT_EQ(complete.splitCount, 7u);
    EXPECT_EQ(after.requestedFootprints, 10u);
    EXPECT_EQ(after.coveredFootprints, 10u);
    EXPECT_EQ(after.resolvedFootprints, 10u);
    EXPECT_EQ(after.minimumLeafZoom, 16);
    EXPECT_EQ(after.maximumLeafZoom, 16);
    EXPECT_EQ(after.remainingSplits, 0u);
    foregroundReviewInvariant(base, partial);
    foregroundReviewInvariant(base, complete);
}

TEST(FinnishForegroundIndependent, TenDispersedRequestsShareBudgetBeforeAnyDeepDescent) {
    ForegroundReviewLeaves base;
    std::vector<OverscaledTileID> requests;
    for (uint32_t i = 0; i < 10; ++i) {
        base.insert(foregroundReviewTile(12, 100 + i * 4, 100));
        requests.push_back(foregroundReviewTile(16, (100 + i * 4) * 16, 1600));
    }
    foregroundReviewDonors(base, 10);
    const auto cover = topo_vector::refineFinnishSurfaceCover(base, requests, 12, 10,
                                                             100.0 / 4096, 100.0 / 4096);
    const auto detail = topo_vector::inspectFinnishSurfaceDetail(cover.meshLeaves, requests);
    EXPECT_EQ(cover.splitCount, 10u);
    EXPECT_EQ(detail.requestedFootprints, 10u);
    EXPECT_EQ(detail.coveredFootprints, 10u);
    EXPECT_EQ(detail.resolvedFootprints, 0u);
    EXPECT_EQ(detail.minimumLeafZoom, 13);
    EXPECT_EQ(detail.maximumLeafZoom, 13);
    EXPECT_EQ(detail.remainingSplits, 30u);
    // Fairer detail is still unresolved detail; readiness must not call it complete.
    foregroundReviewInvariant(base, cover);
}

TEST(FinnishForegroundIndependent, FourIndependentRootsExposeTheSixRemainingSplits) {
    ForegroundReviewLeaves base;
    std::vector<OverscaledTileID> requests;
    for (uint32_t i = 0; i < 4; ++i) {
        base.insert(foregroundReviewTile(12, 100 + i * 4, 100));
        requests.push_back(foregroundReviewTile(16, (100 + i * 4) * 16, 1600));
    }
    foregroundReviewDonors(base, 10);
    const auto cover = topo_vector::refineFinnishSurfaceCover(base, requests, 12, 10,
                                                             100.0 / 4096, 100.0 / 4096);
    const auto detail = topo_vector::inspectFinnishSurfaceDetail(cover.meshLeaves, requests);
    EXPECT_EQ(detail.coveredFootprints, 4u);
    EXPECT_EQ(detail.resolvedFootprints, 0u);
    EXPECT_EQ(detail.minimumLeafZoom, 14);
    EXPECT_EQ(detail.maximumLeafZoom, 15);
    EXPECT_EQ(detail.remainingSplits, 6u);
    foregroundReviewInvariant(base, cover);
}

TEST(FinnishForegroundIndependent, DiagnosticsDeduplicateRequestsWithoutMergingWorldWraps) {
    const auto a = foregroundReviewTile(16, 1600, 1600);
    const auto b = foregroundReviewTile(16, 1600, 1600, 1);
    const ForegroundReviewLeaves base{foregroundReviewTile(12, 100, 100),
                                     foregroundReviewTile(12, 100, 100, 1)};
    const auto detail = topo_vector::inspectFinnishSurfaceDetail(base, {b, a, a});
    EXPECT_EQ(detail.requestedFootprints, 2u);
    EXPECT_EQ(detail.coveredFootprints, 2u);
    EXPECT_EQ(detail.resolvedFootprints, 0u);
    EXPECT_EQ(detail.remainingSplits, 8u);
}

TEST(FinnishForegroundIndependent, SourceLayoutOverscaleDoesNotDuplicatePhysicalFootprints) {
    const ForegroundReviewLeaves base{foregroundReviewTile(12, 100, 100)};
    const auto detail = topo_vector::inspectFinnishSurfaceDetail(
        base, {foregroundReviewTile(16, 1600, 1600),
               OverscaledTileID(17, 0, CanonicalTileID(16, 1600, 1600))});
    EXPECT_EQ(detail.requestedFootprints, 1u);
    EXPECT_EQ(detail.coveredFootprints, 1u);
    EXPECT_EQ(detail.resolvedFootprints, 0u);
    EXPECT_EQ(detail.remainingSplits, 4u);
}

TEST(FinnishForegroundIndependent, FourFinerChildrenCoverARequestButThreeDoNot) {
    const auto request = foregroundReviewTile(16, 1600, 1600);
    ForegroundReviewLeaves leaves;
    for (uint32_t x = 3200; x < 3202; ++x) {
        for (uint32_t y = 3200; y < 3202; ++y) leaves.insert(foregroundReviewTile(17, x, y));
    }
    const auto full = topo_vector::inspectFinnishSurfaceDetail(leaves, {request});
    EXPECT_EQ(full.coveredFootprints, 1u);
    EXPECT_EQ(full.resolvedFootprints, 1u);
    EXPECT_EQ(full.minimumLeafZoom, 17);
    EXPECT_EQ(full.maximumLeafZoom, 17);
    EXPECT_EQ(full.remainingSplits, 0u);
    leaves.erase(foregroundReviewTile(17, 3201, 3201));
    const auto missing = topo_vector::inspectFinnishSurfaceDetail(leaves, {request});
    EXPECT_EQ(missing.coveredFootprints, 0u);
    EXPECT_EQ(missing.resolvedFootprints, 0u);
    EXPECT_EQ(missing.remainingSplits, 0u); // Zero remaining splits does not prove geometric coverage.
}

TEST(FinnishForegroundIndependent, MissingGeometryAndNoRequestsRemainDistinct) {
    const auto absent = topo_vector::inspectFinnishSurfaceDetail({}, {foregroundReviewTile(16, 1600, 1600)});
    EXPECT_EQ(absent.requestedFootprints, 1u);
    EXPECT_EQ(absent.coveredFootprints, 0u);
    EXPECT_EQ(absent.resolvedFootprints, 0u);
    EXPECT_EQ(absent.minimumLeafZoom, 0);
    EXPECT_EQ(absent.maximumLeafZoom, 0);
    EXPECT_EQ(absent.remainingSplits, 0u);
    const auto empty = topo_vector::inspectFinnishSurfaceDetail({foregroundReviewTile(12, 100, 100)}, {});
    EXPECT_EQ(empty.requestedFootprints, 0u);
    EXPECT_EQ(empty.coveredFootprints, 0u);
    EXPECT_EQ(empty.resolvedFootprints, 0u);
    EXPECT_EQ(empty.remainingSplits, 0u);
}

TEST(FinnishForegroundIndependent, PartialZ15SurfaceKeepsItsActualZ15SourceLayout) {
    const ForegroundReviewLeaves leaves{foregroundReviewTile(15, 800, 800)};
    const auto detail = topo_vector::inspectFinnishSurfaceDetail(leaves, {foregroundReviewTile(16, 1600, 1600)});
    EXPECT_EQ(detail.coveredFootprints, 1u);
    EXPECT_EQ(detail.resolvedFootprints, 0u);
    EXPECT_EQ(detail.remainingSplits, 1u);
    const std::vector<OverscaledTileID> expected{OverscaledTileID(15, 0, CanonicalTileID(14, 400, 400))};
    EXPECT_EQ(topo_vector::sourceCoverForTerrainSurfaceLeaves(leaves, 14), expected);
}

TEST(FinnishForegroundIndependent, MixedZ15Z16SurfacesRequestOneLargestLayoutPerCanonicalSource) {
    const ForegroundReviewLeaves leaves{foregroundReviewTile(15, 800, 800),
                                       foregroundReviewTile(16, 1602, 1600)};
    const auto detail = topo_vector::inspectFinnishSurfaceDetail(
        leaves, {foregroundReviewTile(16, 1600, 1600), foregroundReviewTile(16, 1602, 1600)});
    EXPECT_EQ(detail.coveredFootprints, 2u);
    EXPECT_EQ(detail.resolvedFootprints, 1u);
    EXPECT_EQ(detail.minimumLeafZoom, 15);
    EXPECT_EQ(detail.maximumLeafZoom, 16);
    EXPECT_EQ(detail.remainingSplits, 1u);
    const std::vector<OverscaledTileID> expected{OverscaledTileID(16, 0, CanonicalTileID(14, 400, 400))};
    EXPECT_EQ(topo_vector::sourceCoverForTerrainSurfaceLeaves(leaves, 14), expected);
}

TEST(FinnishForegroundIndependent, FinalRetirementSnapshotDoesNotNeedAnotherTimerOrRenderRequest) {
    // Positive ownership or a cleanup request may be emitted less than one
    // second before the next update removes it. Emit that real zero once.
    EXPECT_TRUE(topo_vector::shouldEmitFinalRetiredSurfaceState(5, 0, 0, 0));
    EXPECT_TRUE(topo_vector::shouldEmitFinalRetiredSurfaceState(0, 1, 0, 0));
    EXPECT_TRUE(topo_vector::shouldEmitFinalRetiredSurfaceState(5, 1, 0, 0));
    EXPECT_FALSE(topo_vector::shouldEmitFinalRetiredSurfaceState(0, 0, 0, 0));
    EXPECT_FALSE(topo_vector::shouldEmitFinalRetiredSurfaceState(5, 1, 3, 0));
    EXPECT_FALSE(topo_vector::shouldEmitFinalRetiredSurfaceState(5, 1, 0, 1));
    EXPECT_FALSE(topo_vector::shouldEmitFinalRetiredSurfaceState(0, 0, 5, 0));
}

// Independent additions to the existing cohort test translation unit.
TEST(FinnishForegroundIndependent, QuiescenceRequiresEveryCurrentAndCandidateInvariant) {
    using State = topo_vector::SurfaceDiagnosticState;
    State ready;
    ready.bindings = ready.ready = ready.candidateBindings = ready.candidateReady = 161;
    ASSERT_TRUE(topo_vector::isSurfaceDiagnosticQuiescent(ready));
    EXPECT_FALSE(topo_vector::isSurfaceDiagnosticQuiescent(State{}));

    std::size_t State::* counts[] = {
        &State::bindings, &State::ready, &State::candidateBindings, &State::candidateReady};
    for (const auto member : counts) {
        for (const std::size_t invalid : {std::size_t{0}, std::size_t{160}, std::size_t{162}}) {
            State changed = ready;
            changed.*member = invalid;
            EXPECT_FALSE(topo_vector::isSurfaceDiagnosticQuiescent(changed));
        }
    }
    std::size_t State::* defects[] = {
        &State::unbound, &State::candidateUnbound, &State::emptyDEM, &State::candidateEmptyDEM,
        &State::meshMissing, &State::meshExtra, &State::meshDuplicates, &State::meshOverlap,
        &State::retiredTargets, &State::cleanupRequests};
    for (const auto member : defects) {
        State changed = ready;
        changed.*member = 1;
        EXPECT_FALSE(topo_vector::isSurfaceDiagnosticQuiescent(changed));
    }
    bool State::* flags[] = {&State::atomicHold, &State::atomicDrop, &State::pending};
    for (const auto member : flags) {
        State changed = ready;
        changed.*member = true;
        EXPECT_FALSE(topo_vector::isSurfaceDiagnosticQuiescent(changed));
    }
}

TEST(FinnishForegroundIndependent, OwnershipZeroBeforeReadinessStillEmitsFinalQuiescenceOnce) {
    topo_vector::SurfaceDiagnosticState state;
    state.bindings = state.candidateBindings = 161;
    state.ready = state.candidateReady = 160;
    state.retiredTargets = 5;
    state.cleanupRequests = 1;
    auto lastTargets = state.retiredTargets;
    auto lastCleanup = state.cleanupRequests;
    bool lastQuiescent = topo_vector::isSurfaceDiagnosticQuiescent(state);
    ASSERT_FALSE(lastQuiescent);

    // Ownership reaches zero in the next update, while the final binding is
    // still unready. This emitted snapshot must not consume the ready event.
    state.retiredTargets = state.cleanupRequests = 0;
    ASSERT_FALSE(topo_vector::isSurfaceDiagnosticQuiescent(state));
    ASSERT_TRUE(topo_vector::shouldEmitFinalRetiredSurfaceState(
        lastTargets, lastCleanup, 0, 0, lastQuiescent, false));
    lastTargets = lastCleanup = 0;
    lastQuiescent = false;
    EXPECT_FALSE(topo_vector::shouldEmitFinalRetiredSurfaceState(0, 0, 0, 0, false, false));

    // Less than one second later all actual current and candidate bindings
    // become ready. No timer, user input, or extra render request is needed.
    state.ready = state.candidateReady = 161;
    const bool nowQuiescent = topo_vector::isSurfaceDiagnosticQuiescent(state);
    ASSERT_TRUE(nowQuiescent);
    EXPECT_TRUE(topo_vector::shouldEmitFinalRetiredSurfaceState(
        lastTargets, lastCleanup, 0, 0, lastQuiescent, nowQuiescent));
    lastQuiescent = nowQuiescent; // The renderer stores this only on emission.
    EXPECT_FALSE(topo_vector::shouldEmitFinalRetiredSurfaceState(0, 0, 0, 0, lastQuiescent, true));
    EXPECT_FALSE(topo_vector::shouldEmitFinalRetiredSurfaceState(0, 0, 0, 0, true, false));
}

namespace {
void foregroundReviewBorderStrip(const uint32_t firstY, const std::size_t requiredSplits) {
    // Nearby Nuuksio coordinates, but deliberately synthetic: the historical
    // log records10 requested footprints without retaining their exact IDs.
    std::vector<OverscaledTileID> requests;
    ForegroundReviewLeaves base;
    for (uint32_t x = 37231; x <= 37232; ++x) {
        for (uint32_t y = firstY; y < firstY + 5; ++y) {
            requests.push_back(foregroundReviewTile(16, x, y));
            base.insert(foregroundReviewTile(12, x >> 4, y >> 4));
        }
    }
    foregroundReviewDonors(base, 16);
    const auto before = topo_vector::inspectFinnishSurfaceDetail(base, requests);
    ASSERT_EQ(before.requestedFootprints, 10u);
    ASSERT_EQ(before.remainingSplits, requiredSplits);

    const auto oldBudget = topo_vector::refineFinnishSurfaceCover(
        base, requests, 12, 10, 37231.886 / 65536, 18917.677 / 65536);
    const auto partial = topo_vector::inspectFinnishSurfaceDetail(oldBudget.meshLeaves, requests);
    EXPECT_EQ(oldBudget.splitCount, 10u);
    EXPECT_EQ(partial.coveredFootprints, 10u);
    EXPECT_LT(partial.resolvedFootprints, 10u);
    EXPECT_EQ(partial.remainingSplits, requiredSplits - 10);

    ASSERT_EQ(topo_vector::finnishCloseSurfaceSplitBudget, 16u);
    const auto funded = topo_vector::refineFinnishSurfaceCover(
        base, requests, 12, topo_vector::finnishCloseSurfaceSplitBudget,
        37231.886 / 65536, 18917.677 / 65536);
    const auto complete = topo_vector::inspectFinnishSurfaceDetail(funded.meshLeaves, requests);
    EXPECT_EQ(funded.splitCount, requiredSplits);
    EXPECT_EQ(complete.requestedFootprints, 10u);
    EXPECT_EQ(complete.coveredFootprints, 10u);
    EXPECT_EQ(complete.resolvedFootprints, 10u);
    EXPECT_EQ(complete.minimumLeafZoom, 16);
    EXPECT_EQ(complete.maximumLeafZoom, 16);
    EXPECT_EQ(complete.remainingSplits, 0u);
    for (const auto* cover : {&oldBudget, &funded}) {
        EXPECT_LE(cover->splitCount, 16u);
        EXPECT_EQ(cover->drapeLeaves.size(), base.size());
        EXPECT_EQ(cover->meshLeaves.size(), base.size() + 3 * cover->splitCount);
        EXPECT_EQ(foregroundReviewCells(cover->meshLeaves), foregroundReviewCells(base));
        EXPECT_EQ(foregroundReviewCells(cover->drapeLeaves), foregroundReviewCells(base));
    }
}
} // namespace

TEST(FinnishForegroundIndependent, BorderCrossingTenFootprintsNeedFourteenFundedSplits) {
    foregroundReviewBorderStrip(18915, 14);
}

TEST(FinnishForegroundIndependent, BorderCrossingTenFootprintsCanConsumeAllSixteenFundedSplits) {
    foregroundReviewBorderStrip(18917, 16);
}

TEST(FinnishCloseSurfaceIndependent, LateBaseResizeRequestsCleanupOnlyAfterSuccessorPublishes) {
    using IDs = std::unordered_set<int>;
    const IDs oldBase{11, 21};
    const IDs successorBase{12, 21};
    const IDs retired{11};

    // No virtual meshes remain. A late ring resize is still holding the old
    // texture while its successor bakes; cleanup must not spin or release it.
    EXPECT_FALSE(topo_vector::needsFinnishSurfaceCleanup(true, false, oldBase, oldBase, retired));

    // The base-to-base atomic publication drops exactly the retired texture.
    // The old virtual-only runtime gate missed this final follow-up update.
    EXPECT_TRUE(topo_vector::needsFinnishSurfaceCleanup(true, false, oldBase, successorBase, retired));

    // On the next update the existing ownership-safe prune releases it. A
    // stable publication requests no further cleanup, even before map erasure.
    EXPECT_FALSE(topo_vector::needsFinnishSurfaceCleanup(true, false, successorBase, successorBase, retired));
    EXPECT_FALSE(topo_vector::needsFinnishSurfaceCleanup(true, false, successorBase, successorBase, IDs{}));
}

TEST(FinnishCloseSurfaceIndependent, BaseResizeKeepsPredecessorUntilEveryBindingStopsSamplingIt) {
    using IDs = std::unordered_set<int>;
    const IDs oldPublished{11, 21};
    const IDs partiallyReplaced{11, 12, 21};
    const IDs fullyReplaced{12, 21};
    const IDs retired{11, 999};

    // Another mesh still samples the old exact texture (e.g. shared ancestor
    // fallback), so replacing only one binding cannot trigger its cleanup.
    EXPECT_FALSE(topo_vector::needsFinnishSurfaceCleanup(
        true, false, oldPublished, partiallyReplaced, retired));
    EXPECT_TRUE(topo_vector::needsFinnishSurfaceCleanup(
        true, false, partiallyReplaced, fullyReplaced, retired));

    // Counts alone are insufficient: an unrelated retired allocation cannot
    // cause a follow-up when no retired texture lost published ownership.
    EXPECT_FALSE(topo_vector::needsFinnishSurfaceCleanup(
        true, false, oldPublished, fullyReplaced, IDs{999}));
}

TEST(FinnishCloseSurfaceIndependent, CleanupContextPreservesOutgoingVirtualCoverAndOtherSources) {
    using IDs = std::unordered_set<int>;
    const IDs oldPublished{11};
    const IDs nextPublished{12};
    const IDs retired{11};

    // Ordinary non-Finnish base covers retain their previous scheduling.
    EXPECT_FALSE(topo_vector::needsFinnishSurfaceCleanup(
        false, false, oldPublished, nextPublished, retired));
    // An outgoing virtual cover still qualifies when the Finnish source is
    // no longer rendered, preserving the existing transition cleanup path.
    EXPECT_TRUE(topo_vector::needsFinnishSurfaceCleanup(
        false, true, oldPublished, nextPublished, retired));
    EXPECT_TRUE(topo_vector::needsFinnishSurfaceCleanup(
        true, true, oldPublished, nextPublished, retired));
    EXPECT_FALSE(topo_vector::needsFinnishSurfaceCleanup(
        false, true, oldPublished, oldPublished, retired));
}
