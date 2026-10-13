#include <mbgl/test/util.hpp>

#include <mbgl/util/image.hpp>
#include <mbgl/util/tileset.hpp>
#include <mbgl/geometry/dem_data.hpp>

using namespace mbgl;

auto fakeImage = [](Size s) {
    PremultipliedImage img = PremultipliedImage(s);

    for (size_t i = 0; i < img.bytes(); i++) {
        img.data[i] = (i + 1) % 4 == 0 ? 1 : std::rand() % 255;
    }
    return img;
};

TEST(DEMData, ConstructorMapbox) {
    PremultipliedImage image = fakeImage({16, 16});
    DEMData demdata(image, Tileset::RasterEncoding::Mapbox);

    EXPECT_EQ(demdata.dim, 16);
    EXPECT_EQ(demdata.stride, 18);
    EXPECT_EQ(demdata.getImage()->bytes(), size_t(18 * 18 * 4));
};

TEST(DEMData, ConstructorTerrarium) {
    PremultipliedImage image = fakeImage({16, 16});
    DEMData demdata(image, Tileset::RasterEncoding::Terrarium);

    EXPECT_EQ(demdata.dim, 16);
    EXPECT_EQ(demdata.stride, 18);
    EXPECT_EQ(demdata.getImage()->bytes(), size_t(18 * 18 * 4));
};

TEST(DEMData, InitialBackfill) {
    PremultipliedImage image1 = fakeImage({4, 4});
    DEMData dem1(image1, Tileset::RasterEncoding::Mapbox);

    bool nonempty = true;
    // checking that a 1 px border around the fake image has been populated
    // with a non-empty pixel value
    for (int x = -1; x < 5; x++) {
        for (int y = -1; y < 5; y++) {
            if (dem1.get(x, y) == -65536) {
                nonempty = false;
                break;
            }
        }
    }
    EXPECT_TRUE(nonempty);

    bool verticalBorderMatch = true;
    int vertx[] = {-1, 4};
    for (int x : vertx) {
        for (int y = 0; y < 4; y++) {
            if (dem1.get(x, y) != dem1.get(x < 0 ? x + 1 : x - 1, y)) {
                verticalBorderMatch = false;
                break;
            }
        }
    }
    // vertical border of DEM data is initially equal to next column of data
    EXPECT_TRUE(verticalBorderMatch);

    // horizontal borders empty
    bool horizontalBorderMatch = true;
    int horiz[] = {-1, 4};
    for (int y : horiz) {
        for (int x = 0; x < 4; x++) {
            if (dem1.get(x, y) != dem1.get(x, y < 0 ? y + 1 : y - 1)) {
                horizontalBorderMatch = false;
                break;
            }
        }
    }
    // horizontal border of DEM data is initially equal to next row of data

    EXPECT_TRUE(horizontalBorderMatch);
    // -1, 1 corner initially equal to closest corner data
    EXPECT_TRUE(dem1.get(-1, 4) == dem1.get(0, 3));
    // 1, 1 corner initially equal to closest corner data
    EXPECT_TRUE(dem1.get(4, 4) == dem1.get(3, 3));
    // -1, -1 corner initially equal to closest corner data
    EXPECT_TRUE(dem1.get(-1, -1) == dem1.get(0, 0));
    // -1, 1 corner initially equal to closest corner data
    EXPECT_TRUE(dem1.get(4, -1) == dem1.get(3, 0));
};

TEST(DEMData, BackfillNeighbor) {
    PremultipliedImage image1 = fakeImage({4, 4});
    DEMData dem0(image1, Tileset::RasterEncoding::Mapbox);

    PremultipliedImage image2 = fakeImage({4, 4});
    DEMData dem1(image2, Tileset::RasterEncoding::Mapbox);

    dem0.backfillBorder(dem1, -1, 0);
    for (int y = 0; y < 4; y++) {
        // dx = -1, dy = 0, so the left edge of dem1 should equal the right edge
        // of dem0 backfills Left neighbor
        EXPECT_TRUE(dem0.get(-1, y) == dem1.get(3, y));
    }

    dem0.backfillBorder(dem1, 0, -1);
    // backfills TopCenter neighbor
    for (int x = 0; x < 4; x++) {
        EXPECT_TRUE(dem0.get(x, -1) == dem1.get(x, 3));
    }

    dem0.backfillBorder(dem1, 1, 0);
    // backfills Right neighbor
    for (int y = 0; y < 4; y++) {
        EXPECT_TRUE(dem0.get(4, y) == dem1.get(0, y));
    }

    dem0.backfillBorder(dem1, 0, 1);
    // backfills BottomCenter neighbor
    for (int x = 0; x < 4; x++) {
        EXPECT_TRUE(dem0.get(x, 4) == dem1.get(x, 0));
    }

    dem0.backfillBorder(dem1, -1, 1);
    // backfulls TopRight neighbor
    EXPECT_TRUE(dem0.get(-1, 4) == dem1.get(3, 0));

    dem0.backfillBorder(dem1, 1, 1);
    // backfulls BottomRight neighbor
    EXPECT_TRUE(dem0.get(4, 4) == dem1.get(0, 0));

    dem0.backfillBorder(dem1, -1, -1);
    // backfulls TopLeft neighbor
    EXPECT_TRUE(dem0.get(-1, -1) == dem1.get(3, 3));

    dem0.backfillBorder(dem1, 1, -1);
    // backfulls BottomLeft neighbor
    EXPECT_TRUE(dem0.get(4, -1) == dem1.get(0, 3));
};

TEST(DEMData, BackfillInvalidatesElevationSamples) {
    PremultipliedImage low({2, 2});
    PremultipliedImage high({2, 2});
    for (size_t i = 0; i < low.bytes(); i += 4) {
        low.data[i] = high.data[i] = 128;
        low.data[i + 1] = 0;
        high.data[i + 1] = 100;
        low.data[i + 2] = high.data[i + 2] = 0;
        low.data[i + 3] = high.data[i + 3] = 255;
    }
    DEMData tile(low, Tileset::RasterEncoding::Terrarium);
    DEMData neighbor(high, Tileset::RasterEncoding::Terrarium);
    const auto image = tile.getImagePtr();
    const auto revision = tile.getRevision();
    EXPECT_EQ(tile.get(2, 0), 0);
    tile.backfillBorder(neighbor, 1, 0);
#if defined(__ANDROID__) && MLN_RENDER_BACKEND_OPENGL
    EXPECT_NE(tile.getImagePtr(), image);
    // The old border image still matches the GPU upload held by terrain.
    const auto rightBorderOffset = (1 * tile.stride + 3) * 4;
    EXPECT_EQ(image->data[rightBorderOffset + 1], 0);
    EXPECT_EQ(tile.getImagePtr()->data[rightBorderOffset + 1], 100);
#else
    EXPECT_EQ(tile.getImagePtr(), image);
#endif
    EXPECT_EQ(tile.get(2, 0), 100);
    EXPECT_EQ(tile.get(1, 0), 0);
    EXPECT_GT(tile.getRevision(), revision);
    EXPECT_EQ(neighbor.getRevision(), 0u);
}

#if defined(__ANDROID__) && MLN_RENDER_BACKEND_OPENGL
TEST(DEMData, SharedSnapshotSurvivesChangedNeighborWithoutCopyingNoOps) {
    PremultipliedImage pixels({2, 2});
    std::fill_n(pixels.data.get(), pixels.bytes(), uint8_t{0});
    DEMData tile(pixels, Tileset::RasterEncoding::Mapbox);
    DEMData same(pixels, Tileset::RasterEncoding::Mapbox);
    const auto held = tile.getImagePtr();
    tile.backfillBorder(same, 1, 0);
    EXPECT_EQ(tile.getImagePtr(), held);
    EXPECT_EQ(tile.getRevision(), 0u);
    tile.backfillBorder(same, 0, 0);
    EXPECT_EQ(tile.getImagePtr(), held);
    EXPECT_EQ(tile.getRevision(), 0u);
    tile.backfillBorder(same, 2, 0);
    EXPECT_EQ(tile.getImagePtr(), held);
    EXPECT_EQ(tile.getRevision(), 0u);
    pixels.data[1] = 100;
    DEMData changed(pixels, Tileset::RasterEncoding::Mapbox);
    tile.backfillBorder(changed, 1, 0);
    EXPECT_NE(tile.getImagePtr(), held);
    EXPECT_EQ(held->data[(tile.stride + 3) * 4 + 1], 0);
    EXPECT_EQ(tile.getImagePtr()->data[(tile.stride + 3) * 4 + 1], 100);
    EXPECT_EQ(tile.getRevision(), 1u);
    // The new image has no external owner, so another changed edge mutates
    // it directly while the old held snapshot remains unchanged.
    const auto* current = tile.getImage();
    tile.backfillBorder(changed, 0, 1);
    EXPECT_EQ(tile.getImage(), current);
    EXPECT_EQ(tile.getRevision(), 2u);
    EXPECT_EQ(held->data[(3 * tile.stride + 1) * 4 + 1], 0);
}
#endif
