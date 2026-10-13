#include <mbgl/test/util.hpp>
#include <mbgl/renderer/terrain_symbol_projection.hpp>
#include <mbgl/util/camera.hpp>
#include <mbgl/renderer/buckets/symbol_bucket.hpp>

#include <limits>

using namespace mbgl;

TEST(TerrainSymbolProjection, UsesActualAnchorWithinTile) {
    const UnwrappedTileID tile{0, CanonicalTileID{2, 3, 2}};
    const auto northwest = terrainSymbolAnchorLatLng(tile, {0, 0});
    EXPECT_NEAR(northwest.latitude(), 0.0, 1e-10);
    EXPECT_NEAR(northwest.longitude(), 90.0, 1e-10);
    const auto southeast = terrainSymbolAnchorLatLng(tile, {util::EXTENT, util::EXTENT});
    EXPECT_NEAR(southeast.latitude(), -66.51326044311186, 1e-10);
    EXPECT_NEAR(southeast.longitude(), 180.0, 1e-10);
}

TEST(TerrainSymbolProjection, PreservesWorldCopiesAndCrossZoomAnchor) {
    const auto parent = terrainSymbolAnchorLatLng({2, CanonicalTileID{10, 512, 256}}, {6144, 2048});
    const auto child = terrainSymbolAnchorLatLng({2, CanonicalTileID{11, 1025, 512}}, {4096, 4096});
    EXPECT_NEAR(parent.longitude(), child.longitude(), 1e-10);
    EXPECT_NEAR(parent.latitude(), child.latitude(), 1e-10);
    const auto canonical = terrainSymbolAnchorLatLng({0, CanonicalTileID{10, 512, 256}}, {6144, 2048});
    EXPECT_NEAR(parent.longitude() - canonical.longitude(), 720.0, 1e-10);
    EXPECT_NEAR(parent.latitude(), canonical.latitude(), 1e-10);
}

TEST(TerrainSymbolProjection, DistinctPeakAndValleyShareNoTileCentreHeight) {
    size_t calls = 0;
    TerrainSymbolElevationSampler sample({0, CanonicalTileID{2, 2, 1}}, 2.0f, 1000.0f,
        [&](const LatLng& anchor) -> std::optional<float> {
            ++calls;
            return anchor.longitude() < 45.0 ? 700.0f : 1600.0f;
        });
    EXPECT_FLOAT_EQ(sample({1024, 4096}), -600.0f);
    EXPECT_FLOAT_EQ(sample({7168, 4096}), 1200.0f);
    EXPECT_FLOAT_EQ(sample({1024, 4096}), -600.0f);
    EXPECT_EQ(calls, 2u);
}

TEST(TerrainSymbolProjection, CameraOriginAndExaggerationDoNotResampleDEM) {
    size_t calls = 0;
    TerrainSymbolElevationSampler sample({0, CanonicalTileID{10, 512, 256}}, 1.0f, 1000.0f,
        [&](const LatLng&) -> std::optional<float> { ++calls; return 1300.0f; });
    EXPECT_FLOAT_EQ(sample({500, 300}), 300.0f);
    sample.setTransform(2.0f, 1200.0f);
    EXPECT_FLOAT_EQ(sample({500, 300}), 200.0f);
    sample.setTransform(1.5f, 0.0f); // Absolute-heights path.
    EXPECT_FLOAT_EQ(sample({500, 300}), 1950.0f);
    EXPECT_EQ(calls, 1u);
}

TEST(TerrainSymbolProjection, MissingOrInvalidSamplesKeepZeroPlane) {
    size_t calls = 0;
    TerrainSymbolElevationSampler missing({0, CanonicalTileID{2, 2, 1}}, 2.0f, 1000.0f,
        [&](const LatLng&) -> std::optional<float> { ++calls; return std::nullopt; });
    EXPECT_FLOAT_EQ(missing({100, 200}), 0.0f);
    EXPECT_FLOAT_EQ(missing({100, 200}), 0.0f);
    EXPECT_EQ(calls, 1u);
    EXPECT_FLOAT_EQ(missing({std::numeric_limits<float>::infinity(), 0}), 0.0f);
    EXPECT_EQ(calls, 1u);
    EXPECT_FLOAT_EQ(terrainSymbolRenderHeight(std::numeric_limits<float>::quiet_NaN(), 1.0f, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(terrainSymbolRenderHeight(1000.0f, 1.0f, 1000.0f), 0.0f);
    EXPECT_FLOAT_EQ(terrainSymbolRenderHeight(-100.0f, 1.0f, 0.0f), -100.0f);
}

TEST(TerrainSymbolProjection, FreshSnapshotRefreshesPreviouslyMissingAnchor) {
    size_t calls = 0;
    TerrainSymbolElevationSampler sample({0, CanonicalTileID{2, 2, 1}}, 1.0f, 0.0f,
        [&](const LatLng&) -> std::optional<float> { ++calls; return std::nullopt; });
    EXPECT_FLOAT_EQ(sample({200, 300}), 0.0f);
    sample = TerrainSymbolElevationSampler({0, CanonicalTileID{2, 2, 1}}, 1.0f, 0.0f,
        [&](const LatLng&) -> std::optional<float> { ++calls; return 1400.0f; });
    EXPECT_FLOAT_EQ(sample({200, 300}), 1400.0f);
    EXPECT_EQ(calls, 2u);
}

TEST(TerrainSymbolProjection, CPUMatrixAndVertexHeightUseSameCameraMetres) {
    util::Camera camera;
    camera.setPosition({{0.5, 0.3, 0.001}});
    camera.setOrientation(0.0, 0.8, 0.4);
    const double scale = 4096.0;
    mat4 clip;
    matrix::multiply(clip, camera.getCameraToClipPerspective(0.8, 1.5, 10.0, 10000.0),
                     camera.getWorldToCamera(scale, false));
    const vec4 ground{{0.5 * Projection::worldSize(scale), 0.3 * Projection::worldSize(scale), 0.0, 1.0}};
    const float height = terrainSymbolRenderHeight(1450.0f, 1.5f, 1000.0f);
    vec4 cpu, shader;
    matrix::transformMat4(cpu, ground, terrainSymbolMatrix(clip, height));
    vec4 elevated = ground;
    elevated[2] = height;
    matrix::transformMat4(shader, elevated, clip);
    for (size_t i = 0; i < 4; ++i) EXPECT_NEAR(cpu[i], shader[i], 1e-9);
}

TEST(TerrainSymbolProjection, ElevatedAnchorHasCorrectNearProjectionDepth) {
    util::Camera camera;
    camera.setPosition({{0.5, 0.5, 0.001}});
    camera.setOrientation(0.0, 0.0, 0.0);
    const double scale = 4096.0;
    const vec4 anchor{{0.5 * Projection::worldSize(scale), 0.5 * Projection::worldSize(scale), 0.0, 1.0}};
    mat4 projection;
    matrix::multiply(projection, camera.getCameraToClipPerspective(0.8, 1.5, 100.0, 10000.0),
                     camera.getWorldToCamera(scale, false));
    vec4 ground, peak;
    matrix::transformMat4(ground, anchor, projection);
    matrix::transformMat4(peak, anchor, terrainSymbolMatrix(projection, 1500.0f));
    ASSERT_GT(ground[3], 0.0);
    ASSERT_GT(peak[3], 0.0);
    EXPECT_LT(peak[2] / peak[3], ground[2] / ground[3]);
    EXPECT_GT(peak[2] / peak[3], -1.0);
    EXPECT_LT(peak[2] / peak[3], 1.0);
}

TEST(TerrainSymbolProjection, HeightStreamUsesGlobalGlyphOrderAcrossSegments) {
    std::vector<PlacedSymbol> symbols;
    symbols.emplace_back(Point<float>{100, 200}, 0, 12, 12, std::array<float, 2>{0, 0},
                         WritingModeType::Horizontal, GeometryCoordinates{}, std::vector<float>{});
    symbols.emplace_back(Point<float>{700, 200}, 0, 12, 12, std::array<float, 2>{0, 0},
                         WritingModeType::Horizontal, GeometryCoordinates{}, std::vector<float>{});
    // Both are the first symbol in their segment, exactly as addSymbol returns.
    symbols[0].vertexStartIndex = 0;
    symbols[1].vertexStartIndex = 0;
    symbols[0].glyphOffsets = {0, 12};
    symbols[1].glyphOffsets = {0};
    const auto heights = terrainSymbolVertexElevations(symbols, 12,
        [](const Point<float>& anchor) { return anchor.x < 400 ? -400.0f : 800.0f; });
    ASSERT_EQ(heights.size(), 12u);
    for (size_t i = 0; i < 8; ++i) EXPECT_FLOAT_EQ(heights[i], -400.0f);
    for (size_t i = 8; i < 12; ++i) EXPECT_FLOAT_EQ(heights[i], 800.0f);
    const auto incomplete = terrainSymbolVertexElevations(symbols, 8,
        [](const Point<float>&) { return 1200.0f; });
    EXPECT_EQ(incomplete, std::vector<float>(8, 0.0f));
}
