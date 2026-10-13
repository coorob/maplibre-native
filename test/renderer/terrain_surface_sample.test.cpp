#include <mbgl/test/util.hpp>
#include <mbgl/renderer/terrain_surface_sample.hpp>
#include <mbgl/geometry/dem_data.hpp>
#include <mbgl/renderer/render_terrain.hpp>

#include <limits>

using namespace mbgl;

namespace {
// CPU/GPU multiply contraction can move Terrain-RGB decode by a fraction
// of a millimetre; the fixtures detect metre-scale coordinate/mesh errors.
constexpr float elevationTolerance = 0.001f;
void setElevation(PremultipliedImage& image, uint32_t x, uint32_t y, float elevation) {
    const auto encoded = static_cast<uint32_t>(std::lround((elevation + 10000.0f) * 10.0f));
    const auto offset = (static_cast<size_t>(y) * image.size.width + x) * 4;
    image.data[offset] = (encoded >> 16) & 255;
    image.data[offset + 1] = (encoded >> 8) & 255;
    image.data[offset + 2] = encoded & 255;
    image.data[offset + 3] = 255;
}

PremultipliedImage ramp() {
    PremultipliedImage interior({256, 256});
    for (uint32_t y = 0; y < interior.size.height; ++y) {
        for (uint32_t x = 0; x < interior.size.width; ++x) setElevation(interior, x, y, x * 10.0f);
    }
    // Real source upload: interior pixel centers are at (x+.5)/256.
    // Unloaded neighbor borders are initially replicated by DEMData.
    DEMData dem(interior, Tileset::RasterEncoding::Mapbox);
    return dem.getImage()->clone();
}
} // namespace

TEST(TerrainSurfaceSample, UsesRenderedTexelCentersOnPaddedRamp) {
    const auto image = ramp();
    // Geographic u=.75 lies at interior pixel191.5, midway between
    // heights1910 and1920. The neighbor padding does not enlarge the tile.
    const auto elevation = terrain_surface::sample(image, 0.75f, 0.5f, {0, 0}, 1);
    ASSERT_TRUE(elevation);
    EXPECT_NEAR(1915.0f, *elevation, elevationTolerance);
    EXPECT_NEAR(635.0f, *terrain_surface::sample(image, 0.25f, 0.5f, {0, 0}, 1), elevationTolerance);
}

TEST(TerrainSurfaceSample, UsesReplicatedNeighborBorderAtSourceEdges) {
    const auto image = ramp();
    EXPECT_NEAR(0.0f, *terrain_surface::sample(image, 0, 0.5f, {0, 0}, 1), elevationTolerance);
    EXPECT_NEAR(2550.0f, *terrain_surface::sample(image, 1, 0.5f, {0, 0}, 1), elevationTolerance);
}

TEST(TerrainSurfaceSample, InterpolatesActualTriangleBetweenGridVertices) {
    const auto image = ramp();
    // Away from the clamp, an affine ramp stays affine within either mesh triangle.
    EXPECT_NEAR(terrain_surface::sampleDEM(image, 0.751f, 0.501f),
                *terrain_surface::sample(image, 0.751f, 0.501f, {0, 0}, 1), 0.001f);
    EXPECT_NEAR(terrain_surface::sampleDEM(image, 0.755f, 0.505f),
                *terrain_surface::sample(image, 0.755f, 0.505f, {0, 0}, 1), 0.001f);
}

TEST(TerrainSurfaceSample, UsesRenderedDiagonalOnSaddleInsteadOfBilinearSurface) {
    PremultipliedImage interior({256, 256});
    for (uint32_t y = 0; y < 256; ++y) {
        for (uint32_t x = 0; x < 256; ++x) setElevation(interior, x, y, 0);
    }
    // Four mesh vertices at (32,32)..(33,33) read the centers of these
    // constant 2x2 blocks: TL=0, TR=100, BL=100, BR=0 metres.
    for (uint32_t y = 63; y <= 66; ++y) {
        for (uint32_t x = 63; x <= 66; ++x) {
            setElevation(interior, x, y, ((x >= 65) != (y >= 65)) ? 100 : 0);
        }
    }
    DEMData dem(interior, Tileset::RasterEncoding::Mapbox);
    const auto& image = *dem.getImage();
    EXPECT_NEAR(50.0f, *terrain_surface::sample(image, 32.25f / 128, 32.25f / 128, {0, 0}, 1), elevationTolerance);
    EXPECT_NEAR(50.0f, *terrain_surface::sample(image, 32.75f / 128, 32.75f / 128, {0, 0}, 1), elevationTolerance);
    EXPECT_NEAR(100.0f, *terrain_surface::sample(image, 32.5f / 128, 32.5f / 128, {0, 0}, 1), elevationTolerance);
    EXPECT_NEAR(50.0f, terrain_surface::sampleDEM(image, 32.5f / 128, 32.5f / 128), elevationTolerance);
}

TEST(TerrainSurfaceSample, SamplesTheActualAncestorBindingRemap) {
    const auto image = ramp();
    EXPECT_NEAR(1595.0f, *terrain_surface::sample(image, 0.25f, 0.5f, {0.5f, 0}, 0.5f), elevationTolerance);
    EXPECT_NEAR(1307.0f, *terrain_surface::sample(image, 0.2f, 0.5f, {0.5f, 0}, 0.0625f), 0.001f);
}

TEST(TerrainSurfaceSample, RejectsMissingImageAndInvalidCoordinates) {
    const auto image = ramp();
    EXPECT_FALSE(terrain_surface::sample(PremultipliedImage{}, 0.5f, 0.5f, {0, 0}, 1));
    EXPECT_FALSE(terrain_surface::sample(image, -0.1f, 0.5f, {0, 0}, 1));
    EXPECT_FALSE(terrain_surface::sample(image, 0.5f, 1.1f, {0, 0}, 1));
    EXPECT_FALSE(terrain_surface::sample(image, std::numeric_limits<float>::quiet_NaN(), 0.5f, {0, 0}, 1));
    EXPECT_FALSE(terrain_surface::sample(image, 0.5f, 0.5f, {0, 0}, 0));
}

TEST(TerrainSurfaceSample, AppliesInvalidDEMRuleAtMeshVertices) {
    PremultipliedImage image({1, 1});
    for (const float invalid : {-500.1f, 9000.1f, -10000.0f}) {
        setElevation(image, 0, 0, invalid);
        EXPECT_NEAR(0.0f, *terrain_surface::sample(image, 0.5f, 0.5f, {0, 0}, 1), elevationTolerance);
    }
    // Avoid an exact encoded threshold: GPU float contraction may put
    // nominal -500.0 on either side of the shader comparison.
    for (const float valid : {-499.9f, -100.0f, 8999.9f}) {
        setElevation(image, 0, 0, valid);
        EXPECT_NEAR(valid, *terrain_surface::sample(image, 0.5f, 0.5f, {0, 0}, 1), elevationTolerance);
    }
}

TEST(TerrainSurfaceSample, BindingCacheTracksGeometryButNotDrapeColour) {
    RenderTerrain::DEMBinding first(nullptr, OverscaledTileID{12, 0, 12, 10, 20});
    first.demImage = std::make_shared<PremultipliedImage>(ramp());
    first.demImageRevision = 1;
    auto second = first;
    EXPECT_TRUE(first.sameElevationSurface(second));
    second.drapeReady = !first.drapeReady;
    second.drapeScale = 0.5f;
    EXPECT_TRUE(first.sameElevationSurface(second));
    second.demImageRevision++;
    EXPECT_FALSE(first.sameElevationSurface(second));
    second = first;
    second.demScale = 0.5f;
    EXPECT_FALSE(first.sameElevationSurface(second));
    second = first;
    second.demTL[0] = 0.5f;
    EXPECT_FALSE(first.sameElevationSurface(second));
    second = first;
    second.usedEmptyDEM = true;
    EXPECT_FALSE(first.sameElevationSurface(second));
    second = first;
    second.demImage = std::make_shared<PremultipliedImage>(ramp());
    EXPECT_FALSE(first.sameElevationSurface(second));
    second = first;
    second.sourceID = OverscaledTileID{11, 0, 11, 5, 10};
    EXPECT_FALSE(first.sameElevationSurface(second));
}
