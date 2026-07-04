#include <mbgl/math/log2.hpp>
#include <mbgl/util/bounding_volumes.hpp>
#include <mbgl/util/constants.hpp>
#include <mbgl/util/interpolate.hpp>
#include <mbgl/util/logging.hpp>
#include <mbgl/util/projection.hpp>
#include <mbgl/util/tile_coordinate.hpp>
#include <mbgl/util/tile_cover.hpp>
#include <mbgl/util/tile_cover_impl.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <list>
#include <unordered_set>

using namespace std::numbers;

namespace mbgl {

namespace {

// Once-per-second cap/cover summary at Warning level (the store binary's
// native log filter passes Warning). Only capped covers log — in practice
// that is the raster-dem terrain source. Opt out: KLATTRA_LOG_COVER_SUMMARY=0.
bool klattraLogCoverSummary() {
    static const bool enabled = [] {
        const char* v = std::getenv("KLATTRA_LOG_COVER_SUMMARY");
        return !(v && (*v == '0' || *v == 'f' || *v == 'F'));
    }();
    return enabled;
}

std::string klattraZoomHistogramString(const std::array<uint32_t, 26>& counts) {
    std::string out;
    for (std::size_t z = 0; z < counts.size(); ++z) {
        if (!counts[z]) continue;
        if (!out.empty()) out += ' ';
        out += 'z' + std::to_string(z) + ':' + std::to_string(counts[z]);
    }
    return out.empty() ? std::string("-") : out;
}

using ScanLine = const std::function<void(int32_t x0, int32_t x1, int32_t y)>;

// Taken from polymaps src/Layer.js
// https://github.com/simplegeo/polymaps/blob/master/src/Layer.js#L333-L383
struct edge {
    double x0 = 0, y0 = 0;
    double x1 = 0, y1 = 0;
    double dx = 0, dy = 0;

    edge(Point<double> a, Point<double> b) {
        if (a.y > b.y) std::swap(a, b);
        x0 = a.x;
        y0 = a.y;
        x1 = b.x;
        y1 = b.y;
        dx = b.x - a.x;
        dy = b.y - a.y;
    }
};

// scan-line conversion
void scanSpans(edge e0, edge e1, int32_t ymin, int32_t ymax, ScanLine& scanLine) {
    const double y0 = ::fmax(ymin, std::floor(e1.y0));
    const double y1 = ::fmin(ymax, std::ceil(e1.y1));

    // sort edges by x-coordinate
    if ((e0.x0 == e1.x0 && e0.y0 == e1.y0) ? (e0.x0 + e1.dy / e0.dy * e0.dx < e1.x1)
                                           : (e0.x1 - e1.dy / e0.dy * e0.dx < e1.x0)) {
        std::swap(e0, e1);
    }

    // scan lines!
    const double m0 = e0.dx / e0.dy;
    const double m1 = e1.dx / e1.dy;
    const double d0 = e0.dx > 0;       // use y + 1 to compute x0
    const double d1 = e1.dx < 0;       // use y + 1 to compute x1
    for (double y = y0; y < y1; y++) { // NOLINT(clang-analyzer-security.FloatLoopCounter)
        double x0 = m0 * ::fmax(0, ::fmin(e0.dy, y + d0 - e0.y0)) + e0.x0;
        double x1 = m1 * ::fmax(0, ::fmin(e1.dy, y + d1 - e1.y0)) + e1.x0;
        scanLine(static_cast<int32_t>(std::floor(x1)), static_cast<int32_t>(std::ceil(x0)), static_cast<int32_t>(y));
    }
}

// scan-line conversion
void scanTriangle(const Point<double>& a,
                  const Point<double>& b,
                  const Point<double>& c,
                  int32_t ymin,
                  int32_t ymax,
                  ScanLine& scanLine) {
    edge ab = edge(a, b);
    edge bc = edge(b, c);
    edge ca = edge(c, a);

    // sort edges by y-length
    if (ab.dy > bc.dy) {
        std::swap(ab, bc);
    }
    if (ab.dy > ca.dy) {
        std::swap(ab, ca);
    }
    if (bc.dy > ca.dy) {
        std::swap(bc, ca);
    }

    // scan span! scan span!
    if (ab.dy) scanSpans(ca, ab, ymin, ymax, scanLine);
    if (bc.dy) scanSpans(ca, bc, ymin, ymax, scanLine);
}

} // namespace

namespace util {

namespace {

std::vector<UnwrappedTileID> tileCover(const Point<double>& tl,
                                       const Point<double>& tr,
                                       const Point<double>& br,
                                       const Point<double>& bl,
                                       const Point<double>& c,
                                       uint8_t z) {
    const int32_t tiles = 1 << z;

    struct ID {
        int32_t x, y;
        double sqDist;
    };

    std::vector<ID> t;

    // skip the first few allocations, assuming we usually end up with at least a few tiles
    t.reserve(8);

    auto scanLine = [&](int32_t x0, int32_t x1, int32_t y) {
        int32_t x;
        if (y >= 0 && y <= tiles) {
            for (x = x0; x < x1; ++x) {
                const auto dx = x + 0.5 - c.x;
                const auto dy = y + 0.5 - c.y;
                t.emplace_back(ID{.x = x, .y = y, .sqDist = dx * dx + dy * dy});
            }
        }
    };

    // Divide the screen up in two triangles and scan each of them:
    // \---+
    // | \ |
    // +---\.
    scanTriangle(tl, tr, br, 0, tiles, scanLine);
    scanTriangle(br, bl, tl, 0, tiles, scanLine);

    // Sort first by distance, then by x/y.
    std::sort(t.begin(), t.end(), [](const ID& a, const ID& b) noexcept {
        return std::tie(a.sqDist, a.x, a.y) < std::tie(b.sqDist, b.x, b.y);
    });

    // Erase duplicate tile IDs (they typically occur at the common side of both triangles).
    t.erase(std::unique(t.begin(), t.end(), [](const ID& a, const ID& b) { return a.x == b.x && a.y == b.y; }),
            t.end());

    std::vector<UnwrappedTileID> result;
    result.reserve(t.size());
    for (const auto& id : t) {
        result.emplace_back(z, id.x, id.y);
    }
    return result;
}

} // namespace

int32_t coveringZoomLevel(double zoom, style::SourceType type, uint16_t size) noexcept {
    zoom += util::log2(util::tileSize_D / size);
    if (type == style::SourceType::Raster || type == style::SourceType::Video) {
        return static_cast<int32_t>(std::round(zoom));
    } else {
        return static_cast<int32_t>(std::floor(zoom));
    }
}

std::vector<OverscaledTileID> tileCover(const TileCoverParameters& state,
                                        uint8_t z,
                                        const Range<uint8_t> zoomRange,
                                        const std::optional<uint8_t>& overscaledZ) {
    struct Node {
        AABB aabb;
        uint8_t zoom;
        uint32_t x, y;
        int16_t wrap;
        bool fullyVisible;
    };

    struct ResultTile {
        OverscaledTileID id;
        double sqrDist;
    };

    auto childrenOf = [](const Node& node) -> std::vector<Node> {
        std::vector<Node> children(4);
        for (int i = 0; i < 4; i++) {
            const uint32_t childX = (node.x << 1) + (i % 2);
            const uint32_t childY = (node.y << 1) + (i >> 1);

            children[i] = node;
            children[i].aabb = node.aabb.quadrant(i);
            children[i].zoom = node.zoom + 1;
            children[i].x = childX;
            children[i].y = childY;
        }
        return children;
    };

    const auto& transform = state.transformState;
    const double numTiles = std::pow(2.0, z);
    const double worldSize = Projection::worldSize(transform.getScale());
    const double metersToTileUnits = worldSize > 0.0
        ? (1.0 / Projection::getMetersPerPixelAtLatitude(transform.getLatLng().latitude(), transform.getZoom())) /
              worldSize * numTiles
        : 0.0;
    const double minElevation = std::min(state.tileCoverMinElevationMeters, state.tileCoverMaxElevationMeters) *
                                metersToTileUnits;
    const double maxElevation = std::max(state.tileCoverMinElevationMeters, state.tileCoverMaxElevationMeters) *
                                metersToTileUnits;
    const bool allowVariableZoom = transform.getPitch() > state.tileLodPitchThreshold;
    // Variable-zoom floor: the source minzoom (upstream 6.27 behavior), raised
    // further by the fork's per-source tileLodMinZoom (raster-dem pop control).
    const uint8_t variableZoomFloor = std::min<uint8_t>(
        std::max<uint8_t>(zoomRange.min, state.tileLodMinZoom), z);
    const uint8_t minZoom = allowVariableZoom ? variableZoomFloor : z;
    const uint8_t maxZoom = ((state.tileLodMode == TileLodMode::Distance) && allowVariableZoom) ? zoomRange.max : z;
    const uint8_t overscaledZoom = std::max(overscaledZ.value_or(z), maxZoom);
    const bool flippedY = transform.getViewportMode() == ViewportMode::FlippedY;

    const auto centerPoint = TileCoordinate::fromScreenCoordinate(
                                 transform, z, {transform.getSize().width / 2.0, transform.getSize().height / 2.0})
                                 .p;

    const vec3 centerCoord = {{centerPoint.x, centerPoint.y, 0.0}};

    assert(transform.getFreeCameraOptions().position);
    const vec3 cameraPositionMercator = *transform.getFreeCameraOptions().position;
    const double nominalScale = std::pow(2.0, z);
    const vec3 cameraCoord = vec3Scale(cameraPositionMercator, nominalScale);
    const double cameraToCenterDistanceMercator = vec3Length(vec3Sub(cameraCoord, centerCoord)) / worldSize;

    const Frustum frustum = Frustum::fromInvProjMatrix(transform.getInvProjectionMatrix(), worldSize, z, flippedY);

    // There should always be a certain number of maximum zoom level tiles
    // surrounding the center location
    assert(state.tileLodMinRadius >= 1);
    const double radiusOfMaxLvlLodInTiles = std::max(1.0, state.tileLodMinRadius);

    const auto newRootTile = [&](int16_t wrap) -> Node {
        return {.aabb = AABB({{wrap * numTiles, 0.0, minElevation}}, {{(wrap + 1) * numTiles, numTiles, maxElevation}}),
                .zoom = uint8_t(0),
                .x = uint16_t(0),
                .y = uint16_t(0),
                .wrap = wrap,
                .fullyVisible = false};
    };

    // Perform depth-first traversal on tile tree to find visible tiles
    std::vector<Node> stack;
    std::vector<ResultTile> result;
    stack.reserve(128);

    // World copies shall be rendered three times on both sides from closest to farthest
    for (int i = 1; i <= 3; i++) {
        stack.push_back(newRootTile(-i));
        stack.push_back(newRootTile(i));
    }

    stack.push_back(newRootTile(0));

    while (!stack.empty()) {
        Node node = stack.back();
        stack.pop_back();

        // Use cached visibility information of ancestor nodes
        if (!node.fullyVisible) {
            const IntersectionResult intersection = frustum.intersects(node.aabb);

            if (intersection == IntersectionResult::Separate) continue;

            node.fullyVisible = intersection == IntersectionResult::Contains;
        }

        bool shouldSplitTile;
        if (state.tileLodMode == TileLodMode::Distance) {
            const vec3 camToTileMercator = vec3Scale(node.aabb.distanceXYZ(cameraCoord), 1.0 / worldSize);
            const double distanceToTileMercator = vec3Length(camToTileMercator);
            const double cosPitchToTile = std::max(0.0, camToTileMercator[2] / distanceToTileMercator);
            const double pitchExponent =
                0.5; // 0: constant screen width, 1/2: constant screen area, 1: constant screen height
            double tileScale = std::pow(2.0, node.zoom);
            shouldSplitTile = distanceToTileMercator * tileScale < std::pow(cosPitchToTile, pitchExponent) *
                                                                       cameraToCenterDistanceMercator /
                                                                       state.tileLodScale * nominalScale;
        } else {
            const vec3 distanceXyz = node.aabb.distanceXYZ(centerCoord);
            const double* longestDim = std::max_element(distanceXyz.data(), distanceXyz.data() + distanceXyz.size());
            assert(longestDim);

            // We're using distance based heuristics to determine if a tile should
            // be split into quadrants or not. radiusOfMaxLvlLodInTiles defines that
            // there's always a certain number of maxLevel tiles next to the map
            // center. Using the fact that a parent node in quadtree is twice the
            // size of its children (per dimension) we can define distance
            // thresholds for each relative level:
            // f(k) = offset + 2 + 4 + 8 + 16 + ... + 2^k
            // This is the same as:
            // f(k) = offset + 2^(k+1)-2
            const double distToSplit = radiusOfMaxLvlLodInTiles + (1 << (maxZoom - node.zoom)) - 2;
            shouldSplitTile = *longestDim * state.tileLodScale < distToSplit;
        }

        // Have we reached the target depth or is the tile too far away to be any split further?
        if (node.zoom == maxZoom || (!shouldSplitTile && node.zoom >= minZoom)) {
            // Perform precise intersection test between the frustum and aabb.
            // This will cull < 1% false positives missed by the original test
            if (node.fullyVisible || frustum.intersectsPrecise(node.aabb, true) != IntersectionResult::Separate) {
                const OverscaledTileID id = {
                    node.zoom == maxZoom ? overscaledZoom : node.zoom, node.wrap, node.zoom, node.x, node.y};
                vec3 coordToLoadFirst = (state.tileLodMode == TileLodMode::Distance) ? cameraCoord : centerCoord;
                const double dx = node.wrap * numTiles + node.x + 0.5 - coordToLoadFirst[0];
                const double dy = node.y + 0.5 - coordToLoadFirst[1];

                result.push_back({id, dx * dx + dy * dy});
            }
        } else {
            std::vector<Node> children = childrenOf(node);
            stack.insert(stack.end(), children.begin(), children.end());
        }
    }

    // Sort results by distance
    std::sort(
        result.begin(), result.end(), [](const ResultTile& a, const ResultTile& b) { return a.sqrDist < b.sqrDist; });

    std::vector<OverscaledTileID> ids;
    ids.reserve(result.size());

    for (const auto& tile : result) {
        ids.push_back(tile.id);
    }

    // Frustum-cover membership, kept only when a cap is active so the cap can
    // prefer genuinely visible tiles over elevation-dilation padding.
    std::unordered_set<OverscaledTileID> frustumCover;
    bool dilated = false;

    if (state.tileCoverMinElevationMeters != 0.0 || state.tileCoverMaxElevationMeters != 0.0) {
        if (state.tileCoverMaxTiles > 0) {
            frustumCover.insert(ids.begin(), ids.end());
        }
        dilated = true;
        std::vector<OverscaledTileID> expanded = ids;
        constexpr int32_t radius = 4;
        for (const auto& id : ids) {
            // Bounds and wrap must use the tile's OWN grid size: a pitched
            // (variable-zoom) cover emits canonical.z below the ideal z, and
            // sizing the grid at the ideal z allowed x/y outside the tile
            // grid — an assert in debug builds, phantom tile requests in
            // release.
            const int32_t tileCountAtTileZ = 1 << id.canonical.z;
            for (int32_t dy = -radius; dy <= radius; ++dy) {
                const int32_t y = static_cast<int32_t>(id.canonical.y) + dy;
                if (y < 0 || y >= tileCountAtTileZ) continue;
                for (int32_t dx = -radius; dx <= radius; ++dx) {
                    int32_t x = static_cast<int32_t>(id.canonical.x) + dx;
                    int16_t wrap = id.wrap;
                    while (x < 0) {
                        x += tileCountAtTileZ;
                        --wrap;
                    }
                    while (x >= tileCountAtTileZ) {
                        x -= tileCountAtTileZ;
                        ++wrap;
                    }
                    expanded.emplace_back(id.overscaledZ, wrap, id.canonical.z, static_cast<uint32_t>(x),
                                          static_cast<uint32_t>(y));
                }
            }
        }
        std::sort(expanded.begin(), expanded.end());
        expanded.erase(std::unique(expanded.begin(), expanded.end()), expanded.end());
        ids = std::move(expanded);
    }

    if (state.tileCoverMaxTiles > 0 && ids.size() > state.tileCoverMaxTiles) {
        // Rank every tile in a single zoom-consistent space before cutting:
        // scale each tile centre into ideal-zoom (z) tile units and measure
        // against the already-computed screen-centre coordinate. Comparing
        // distances at each tile's own canonical z (previous behaviour) made
        // one z10 unit equal one z12 unit, so coarse horizon tiles
        // systematically outranked the visible foreground. Frustum-cover
        // tiles always outrank dilation-only padding so the budget is spent
        // on what is actually on screen — padding only fills slots the
        // visible cover doesn't need.
        struct RankedTile {
            bool dilationOnly;
            double sqrDist;
            OverscaledTileID id;
        };
        std::vector<RankedTile> ranked;
        ranked.reserve(ids.size());
        for (const auto& id : ids) {
            const double scaleToIdealZ = std::ldexp(1.0, static_cast<int>(z) - static_cast<int>(id.canonical.z));
            const double tilesAtTileZ = std::ldexp(1.0, static_cast<int>(id.canonical.z));
            const double dx = (static_cast<double>(id.wrap) * tilesAtTileZ + static_cast<double>(id.canonical.x) +
                               0.5) * scaleToIdealZ -
                              centerCoord[0];
            const double dy = (static_cast<double>(id.canonical.y) + 0.5) * scaleToIdealZ - centerCoord[1];
            const bool dilationOnly = dilated && frustumCover.find(id) == frustumCover.end();
            ranked.push_back({dilationOnly, dx * dx + dy * dy, id});
        }
        std::sort(ranked.begin(), ranked.end(), [](const RankedTile& a, const RankedTile& b) {
            if (a.dilationOnly != b.dilationOnly) {
                return b.dilationOnly;
            }
            if (a.sqrDist != b.sqrDist) {
                return a.sqrDist < b.sqrDist;
            }
            return a.id < b.id;
        });
        ids.clear();
        ids.reserve(state.tileCoverMaxTiles);
        for (std::size_t i = 0; i < state.tileCoverMaxTiles; ++i) {
            ids.push_back(ranked[i].id);
        }

        // Cut telemetry: frustum-cover tiles cut = real on-screen holes;
        // dilation-only cuts are harmless padding. 1 Hz, Warning so the
        // release log filter passes it.
        if (klattraLogCoverSummary()) {
            static std::chrono::steady_clock::time_point lastLog{};
            const auto now = std::chrono::steady_clock::now();
            if (now - lastLog >= std::chrono::seconds(1)) {
                lastLog = now;
                std::array<uint32_t, 26> keptByZ{};
                std::array<uint32_t, 26> cutByZ{};
                uint32_t frustumCut = 0;
                for (std::size_t i = 0; i < ranked.size(); ++i) {
                    const auto cz = std::min<std::size_t>(ranked[i].id.canonical.z, keptByZ.size() - 1);
                    if (i < state.tileCoverMaxTiles) {
                        ++keptByZ[cz];
                    } else {
                        ++cutByZ[cz];
                        if (!ranked[i].dilationOnly) {
                            ++frustumCut;
                        }
                    }
                }
                Log::Warning(Event::Render,
                             "[KLATTRA COVER] pre=" + std::to_string(ranked.size()) +
                                 " cap=" + std::to_string(state.tileCoverMaxTiles) +
                                 " frustumCut=" + std::to_string(frustumCut) +
                                 " keptByZ=" + klattraZoomHistogramString(keptByZ) +
                                 " cutByZ=" + klattraZoomHistogramString(cutByZ) +
                                 " pitchDeg=" + std::to_string(transform.getPitch() * 180.0 / pi) +
                                 " idealZ=" + std::to_string(static_cast<int>(z)));
            }
        }
    }

    return ids;
}

std::vector<UnwrappedTileID> tileCover(const LatLngBounds& bounds_, uint8_t z) {
    if (bounds_.isEmpty() || bounds_.south() > util::LATITUDE_MAX || bounds_.north() < -util::LATITUDE_MAX) {
        return {};
    }

    const LatLngBounds bounds = LatLngBounds::hull({std::max(bounds_.south(), -util::LATITUDE_MAX), bounds_.west()},
                                                   {std::min(bounds_.north(), util::LATITUDE_MAX), bounds_.east()});

    return tileCover(Projection::project(bounds.northwest(), z),
                     Projection::project(bounds.northeast(), z),
                     Projection::project(bounds.southeast(), z),
                     Projection::project(bounds.southwest(), z),
                     Projection::project(bounds.center(), z),
                     z);
}

std::vector<UnwrappedTileID> tileCover(const Geometry<double>& geometry, uint8_t z) {
    std::vector<UnwrappedTileID> result;
    TileCover tc(geometry, z, true);
    while (tc.hasNext()) {
        result.push_back(*tc.next());
    };

    return result;
}

// Taken from https://github.com/mapbox/sphericalmercator#xyzbbox-zoom-tms_style-srs
// Computes the projected tiles for the lower left and upper right points of the bounds
// and uses that to compute the tile cover count
uint64_t tileCount(const LatLngBounds& bounds, uint8_t zoom) noexcept {
    if (zoom == 0) {
        return 1;
    }
    const auto sw = Projection::project(bounds.southwest(), zoom);
    const auto ne = Projection::project(bounds.northeast(), zoom);
    const auto maxTile = std::pow(2.0, zoom);
    const auto x1 = floor(sw.x);
    const auto x2 = ceil(ne.x) - 1;
    const auto y1 = util::clamp(floor(sw.y), 0.0, maxTile - 1);
    const auto y2 = util::clamp(floor(ne.y), 0.0, maxTile - 1);

    const auto dx = x1 > x2 ? (maxTile - x1) + x2 : x2 - x1;
    const auto dy = y1 - y2;
    return static_cast<uint64_t>((dx + 1) * (dy + 1));
}

uint64_t tileCount(const Geometry<double>& geometry, uint8_t z) {
    uint64_t tileCount = 0;

    TileCover tc(geometry, z, true);
    while (tc.next()) {
        tileCount++;
    };
    return tileCount;
}

TileCover::TileCover(const LatLngBounds& bounds_, uint8_t z) {
    LatLngBounds bounds = LatLngBounds::hull({std::max(bounds_.south(), -util::LATITUDE_MAX), bounds_.west()},
                                             {std::min(bounds_.north(), util::LATITUDE_MAX), bounds_.east()});

    if (bounds.isEmpty() || bounds.south() > util::LATITUDE_MAX || bounds.north() < -util::LATITUDE_MAX) {
        bounds = LatLngBounds::world();
    }

    const auto sw = Projection::project(bounds.southwest(), z);
    const auto ne = Projection::project(bounds.northeast(), z);
    const auto se = Projection::project(bounds.southeast(), z);
    const auto nw = Projection::project(bounds.northwest(), z);

    const Polygon<double> p({{sw, nw, ne, se, sw}});
    impl = std::make_unique<TileCover::Impl>(z, p, false);
}

TileCover::TileCover(const Geometry<double>& geom, uint8_t z, bool project /* = true*/)
    : impl(std::make_unique<TileCover::Impl>(z, geom, project)) {}

TileCover::~TileCover() = default;

std::optional<UnwrappedTileID> TileCover::next() {
    return impl->next();
}

bool TileCover::hasNext() {
    return impl->hasNext();
}

} // namespace util
} // namespace mbgl
