#include <mbgl/renderer/topo_vector_cohort.hpp>
#include <mbgl/renderer/topo_drape_detail_policy.hpp>
#include <mbgl/test/util.hpp>

using namespace mbgl;

namespace {

OverscaledTileID tile(uint8_t z, uint32_t x, uint32_t y, int16_t wrap = 0) {
    return OverscaledTileID(z, wrap, CanonicalTileID(z, x, y));
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

    EXPECT_TRUE(topo_vector::useSharedDrapeCohort(true, false));
    EXPECT_FALSE(topo_vector::useSharedDrapeCohort(true, true));
    EXPECT_FALSE(topo_vector::useSharedDrapeCohort(false, false));
}
