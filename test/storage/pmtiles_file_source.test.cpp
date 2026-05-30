#include <mbgl/storage/pmtiles_file_source.hpp>
#include <mbgl/storage/resource.hpp>
#include <mbgl/storage/resource_options.hpp>
#include <mbgl/util/platform.hpp>
#include <mbgl/util/run_loop.hpp>

#include <filesystem>
#include <memory>
#include <vector>

#include <climits>
#include <gtest/gtest.h>

namespace {

std::string toAbsoluteURL(const std::string &fileName) {
    auto path = std::filesystem::current_path() / "test/fixtures/storage/pmtiles" / fileName;
    return std::string(mbgl::util::PMTILES_PROTOCOL) + std::string(mbgl::util::FILE_PROTOCOL) + path.string();
}

} // namespace

using namespace mbgl;

TEST(PMTilesFileSource, AcceptsURL) {
    PMTilesFileSource pmtiles(ResourceOptions::Default(), ClientOptions());
    EXPECT_TRUE(pmtiles.canRequest(Resource::style("pmtiles:///test")));
    EXPECT_FALSE(pmtiles.canRequest(Resource::style("pmtile://test")));
    EXPECT_FALSE(pmtiles.canRequest(Resource::style("pmtiles:")));
    EXPECT_FALSE(pmtiles.canRequest(Resource::style("")));
}

// Nonexistent pmtiles file raises error
TEST(PMTilesFileSource, NonExistentFile) {
    util::RunLoop loop;

    PMTilesFileSource pmtiles(ResourceOptions::Default(), ClientOptions());

    std::unique_ptr<AsyncRequest> req = pmtiles.request(
        {Resource::Unknown, toAbsoluteURL("does_not_exist")}, [&](Response res) {
            req.reset();
            ASSERT_NE(nullptr, res.error);
            EXPECT_EQ(Response::Error::Reason::NotFound, res.error->reason);
            EXPECT_NE((res.error->message).find("path not found"), std::string::npos);
            ASSERT_FALSE(res.data.get());
            loop.stop();
        });

    loop.run();
}

// Existing pmtiles file default request returns TileJSON
TEST(PMTilesFileSource, TileJSON) {
    util::RunLoop loop;

    PMTilesFileSource pmtiles(ResourceOptions::Default(), ClientOptions());

    std::unique_ptr<AsyncRequest> req = pmtiles.request(
        {Resource::Unknown, toAbsoluteURL("geography-class-png.pmtiles")}, [&](Response res) {
            req.reset();
            EXPECT_EQ(nullptr, res.error);
            ASSERT_TRUE(res.data.get());
            // basic test that TileJSON included a tile URL
            EXPECT_NE((*res.data).find("geography-class-png.pmtiles"), std::string::npos);
            loop.stop();
        });

    loop.run();
}

// Existing tiles return tile data
TEST(PMTilesFileSource, Tile) {
    util::RunLoop loop;

    PMTilesFileSource pmtiles(ResourceOptions::Default(), ClientOptions());

    std::unique_ptr<AsyncRequest> req = pmtiles.request(
        Resource::tile(toAbsoluteURL("geography-class-png.pmtiles"), 1.0, 0, 0, 0, Tileset::Scheme::XYZ),
        [&](Response res) {
            req.reset();
            EXPECT_EQ(nullptr, res.error);
            ASSERT_TRUE(res.data.get());
            ASSERT_EQ(res.noContent, false);
            loop.stop();
        });

    loop.run();
}

// Nonexistent tiles do not raise errors, they simply return no content
TEST(PMTilesFileSource, NonExistentTile) {
    util::RunLoop loop;

    PMTilesFileSource pmtiles(ResourceOptions::Default(), ClientOptions());

    std::unique_ptr<AsyncRequest> req = pmtiles.request(
        Resource::tile(toAbsoluteURL("geography-class-png.pmtiles"), 1.0, 0, 0, 4, Tileset::Scheme::XYZ),
        [&](Response res) {
            req.reset();
            EXPECT_EQ(nullptr, res.error);
            ASSERT_FALSE(res.data.get());
            ASSERT_EQ(res.noContent, true);
            loop.stop();
        });

    loop.run();
}

// A tile whose bytes start with gzip magic but are otherwise corrupt must yield an error
// response — the decompression failure must not propagate as an exception.
TEST(PMTilesFileSource, CorruptGzipTile) {
    util::RunLoop loop;

    PMTilesFileSource pmtiles(ResourceOptions::Default(), ClientOptions());

    std::unique_ptr<AsyncRequest> req = pmtiles.request(
        Resource::tile(toAbsoluteURL("corrupt-gzip-tile.pmtiles"), 1.0, 0, 0, 0, Tileset::Scheme::XYZ),
        [&](Response res) {
            req.reset();
            ASSERT_NE(nullptr, res.error);
            EXPECT_EQ(Response::Error::Reason::Other, res.error->reason);
            EXPECT_NE(res.error->message.find("Error decompressing PMTiles tile:"), std::string::npos);
            loop.stop();
        });

    loop.run();
}

// An archive whose header flags tile_compression=GZIP but whose individual tiles are stored
// without compression must be served without error (uncompressed bytes passed through as-is).
TEST(PMTilesFileSource, UncompressedTile) {
    util::RunLoop loop;

    PMTilesFileSource pmtiles(ResourceOptions::Default(), ClientOptions());

    std::unique_ptr<AsyncRequest> req = pmtiles.request(
        Resource::tile(toAbsoluteURL("uncompressed-tiles.pmtiles"), 1.0, 0, 0, 0, Tileset::Scheme::XYZ),
        [&](Response res) {
            req.reset();
            EXPECT_EQ(nullptr, res.error);
            ASSERT_TRUE(res.data.get());
            ASSERT_EQ(res.noContent, false);
            loop.stop();
        });

    loop.run();
}

// Regression: a tile request chains header -> directory -> tile sub-requests.
// Each hop used to overwrite a single per-request task slot, which freed the
// still-executing sub-request from inside its own completion callback -- a
// use-after-free on the PMTilesFileSource worker thread that crashed during map
// teardown/transitions. Fire several overlapping chains and require them all to
// complete cleanly. Under ASan this also catches the freed-mid-callback access.
TEST(PMTilesFileSource, ConcurrentTileRequests) {
    util::RunLoop loop;

    PMTilesFileSource pmtiles(ResourceOptions::Default(), ClientOptions());

    constexpr int kCount = 8;
    int completed = 0;
    std::vector<std::unique_ptr<AsyncRequest>> reqs;

    for (int i = 0; i < kCount; ++i) {
        reqs.push_back(pmtiles.request(
            Resource::tile(toAbsoluteURL("geography-class-png.pmtiles"), 1.0, 0, 0, 0, Tileset::Scheme::XYZ),
            [&](Response res) {
                EXPECT_EQ(nullptr, res.error);
                if (++completed == kCount) {
                    loop.stop();
                }
            }));
    }

    loop.run();

    EXPECT_EQ(completed, kCount);
}

// Regression: tearing the source down while requests are in flight must cancel
// the chained sub-requests cleanly and never free an in-flight request from
// inside its own callback. Models the iOS map-view teardown that crashed.
TEST(PMTilesFileSource, DestroyDuringRequest) {
    util::RunLoop loop;

    auto pmtiles = std::make_unique<PMTilesFileSource>(ResourceOptions::Default(), ClientOptions());

    std::vector<std::unique_ptr<AsyncRequest>> reqs;
    for (int i = 0; i < 8; ++i) {
        reqs.push_back(pmtiles->request(
            Resource::tile(toAbsoluteURL("geography-class-png.pmtiles"), 1.0, 0, 0, 0, Tileset::Scheme::XYZ),
            [](Response) {}));
    }

    // Destroy the file source (joins the worker thread), then the requests,
    // while sub-requests may still be in flight. Must not crash.
    pmtiles.reset();
    reqs.clear();

    SUCCEED();
}
