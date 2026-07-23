#include <mbgl/renderer/tile_pyramid.hpp>
#include <mbgl/renderer/cover_hold_budget.hpp>
#include <mbgl/renderer/paint_parameters.hpp>
#include <mbgl/renderer/render_source.hpp>
#include <mbgl/renderer/tile_parameters.hpp>
#include <mbgl/renderer/query.hpp>
#include <mbgl/map/transform.hpp>
#include <mbgl/math/clamp.hpp>
#include <mbgl/actor/scheduler.hpp>
#include <mbgl/util/tile_cover.hpp>
#include <mbgl/util/tile_range.hpp>
#include <mbgl/util/enum.hpp>
#include <mbgl/util/logging.hpp>

#include <mbgl/algorithm/update_renderables.hpp>

#include <mapbox/geometry/envelope.hpp>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <algorithm>

namespace mbgl {

using namespace style;

namespace {
TileObserver nullObserver;
const std::map<OverscaledTileID, std::unique_ptr<Tile>> emptyPrefetchedTiles;

// .66: raster sources feed the terrain drape gap-fill from this cache —
// a demotion-resized canvas behind the camera can only regain NATIVE-res
// imagery if its exact/child tiles are still retained. The conservative
// viewport-derived size (~57 tiles on the soak phone) evicted the
// corridor's exacts within a minute, so 74% of the phone-64 flight's
// gap-fills fell back to z10-into-z12 ancestors — the flat green mush.
// Raster tiles are cheap (256² ≈ 0.25 MB uploaded), so scale the cache
// for raster sources only (DEM/vector keep the conservative size).
// KLATTRA_RASTER_CACHE_SCALE=1 reverts.
size_t klattraRasterCacheScale() {
    static const size_t scale = [] {
        const char* v = std::getenv("KLATTRA_RASTER_CACHE_SCALE");
        if (v && *v) {
            char* end = nullptr;
            const unsigned long parsed = std::strtoul(v, &end, 10);
            if (end != v) return static_cast<size_t>(std::clamp<unsigned long>(parsed, 1, 16));
        }
        return static_cast<size_t>(4);
    }();
    return scale;
}

// DIAG: sample the same physical-footprint counter used by iOS jetsam while
// the flat 2D RasterDEM path is active. RenderTerrain owns the older sampler,
// but 2D never enters that code. Keep a sampled peak so the log remains useful
// when the current footprint falls between COVERHOLD heartbeats.
struct KlattraMemorySample {
    int64_t physicalMB = -1;
    int64_t sampledPeakMB = -1;
};

KlattraMemorySample klattraMemorySample() {
#if defined(__APPLE__)
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        static uint64_t sampledPeakBytes = 0;
        sampledPeakBytes = std::max(sampledPeakBytes, static_cast<uint64_t>(info.phys_footprint));
        constexpr double bytesPerMB = 1024.0 * 1024.0;
        return {
            static_cast<int64_t>(std::lround(static_cast<double>(info.phys_footprint) / bytesPerMB)),
            static_cast<int64_t>(std::lround(static_cast<double>(sampledPeakBytes) / bytesPerMB)),
        };
    }
#endif
    return {};
}

// Launch-gated pressure policy shared by all TilePyramids in this renderer
// process. The active terrain DEM samples the physical footprint once per
// update; when it trips, the other tiled sources can react on their next
// update without each issuing its own task_info call.
std::atomic_bool& klattraCoverHoldHighWaterTripped() {
    static std::atomic_bool tripped{false};
    return tripped;
}

std::size_t klattraNonRasterDEMPressureCoverHoldCap() {
    static const std::size_t cap = [] {
        const char* v = std::getenv("KLATTRA_NON_DEM_PRESSURE_HOLD_CAP");
        if (!v || !*v) return std::size_t{0};
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(v, &end, 10);
        return end != v && parsed > 0 && parsed <= 4096 ? static_cast<std::size_t>(parsed) : std::size_t{0};
    }();
    return cap;
}
} // namespace

TilePyramid::TilePyramid(const TaggedScheduler& threadPool_)
    : cache(threadPool_),
      observer(&nullObserver) {}

TilePyramid::~TilePyramid() = default;

bool TilePyramid::isLoaded() const {
    for (const auto& pair : tiles) {
        if (!pair.second->isComplete()) {
            return false;
        }
    }

    return true;
}

Tile* TilePyramid::getTile(const OverscaledTileID& tileID) {
    auto it = tiles.find(tileID);
    return it == tiles.end() ? cache.get(tileID) : it->second.get();
}

const Tile* TilePyramid::getRenderedTile(const UnwrappedTileID& tileID) const {
    auto it = renderedTiles.find(tileID);
    return it != renderedTiles.end() ? &it->second.get() : nullptr;
}

void TilePyramid::update(const std::vector<Immutable<style::LayerProperties>>& layers,
                         const bool needsRendering,
                         const bool needsRelayout,
                         const TileParameters& parameters,
                         const style::Source::Impl& sourceImpl,
                         const uint16_t tileSize,
                         const Range<uint8_t> zoomRange,
                         std::optional<LatLngBounds> bounds,
                         std::function<std::unique_ptr<Tile>(const OverscaledTileID&, TileObserver*)> createTile) {
    // If we need a relayout, abandon any cached tiles; they're now stale.
    if (needsRelayout) {
        cache.clear();
    }

    // If we're not going to render anything, move our existing tiles into
    // the cache (if they're not stale) or abandon them, and return.
    if (!needsRendering) {
        // KLATTRA diagnostics (2D transition flash, post-.34): this purge is
        // the only path that empties renderedTiles without a trace — a
        // one-frame needsRendering flap here would explain the vector land
        // pyramids collapsing to fragments with COVERHOLD silent (nothing
        // left to hold) and SRCTILES only seeing the aftermath as a DIP.
        // RasterDEM defaults on in .68; opt out with KLATTRA_LOG_SRCPURGE=0.
        if (!renderedTiles.empty()) {
            static const int8_t purgeLogMode = [] {
                const char* v = std::getenv("KLATTRA_LOG_SRCPURGE");
                if (!v) return int8_t{-1};
                return (*v == '0' || *v == 'f' || *v == 'F') ? int8_t{0} : int8_t{1};
            }();
            const bool purgeLog = purgeLogMode > 0 ||
                                  (purgeLogMode < 0 && sourceImpl.type == SourceType::RasterDEM);
            if (purgeLog) {
                Log::Warning(Event::Render,
                             "[KLATTRA SRCPURGE] source=" + sourceImpl.id +
                                 " droppedRendered=" + std::to_string(renderedTiles.size()) +
                                 " tilesHeld=" + std::to_string(tiles.size()) +
                                 " relayout=" + (needsRelayout ? "1" : "0"));
                static const bool traceStderr = std::getenv("KLATTRA_TRACE_STDERR") != nullptr;
                if (traceStderr) {
                    fprintf(stderr,
                            "[KLATTRA_TRACE] [KLATTRA SRCPURGE] source=%s droppedRendered=%zu tilesHeld=%zu "
                            "relayout=%d\n",
                            sourceImpl.id.c_str(),
                            renderedTiles.size(),
                            tiles.size(),
                            needsRelayout ? 1 : 0);
                }
            }
        }
        for (auto& entry : tiles) {
            if (!needsRelayout) {
                // These tiles are invisible, we set optional necessity
                // for them and thus suppress network requests on
                // tiles expiration (see `OnlineFileRequest`).
                entry.second->setNecessity(TileNecessity::Optional);
                cache.add(entry.first, std::move(entry.second));
            } else {
                cache.deferredRelease(std::move(entry.second));
            }
        }

        tiles.clear();
        renderedTiles.clear();
        coverHoldAges.clear();
        recentIdealTiles.clear();
        cache.deferPendingReleases();

        return;
    }

    handleWrapJump(static_cast<float>(parameters.transformState.getLatLng().longitude()));

    // Optionally shift the zoom level
    double zoom = util::clamp<double>(parameters.transformState.getZoom() + parameters.tileLodZoomShift,
                                      parameters.transformState.getMinZoom(),
                                      parameters.transformState.getMaxZoom());

    const auto type = sourceImpl.type;
    // Determine the overzooming/underzooming amounts and required tiles.
    int32_t overscaledZoom = util::coveringZoomLevel(zoom, type, tileSize);
    int32_t tileZoom = overscaledZoom;
    int32_t panZoom = zoomRange.max;

    const std::optional<uint8_t>& sourcePrefetchZoomDelta = sourceImpl.getPrefetchZoomDelta();
    const std::optional<uint8_t>& maxParentTileOverscaleFactor = sourceImpl.getMaxOverscaleFactorForParentTiles();
    const Duration minimumUpdateInterval = sourceImpl.getMinimumTileUpdateInterval();
    const bool isVolatile = sourceImpl.isVolatile();

    std::vector<OverscaledTileID> idealTiles;
    std::vector<OverscaledTileID> panTiles;

    util::TileCoverParameters tileCoverParameters = {.transformState = parameters.transformState,
                                                     .tileLodMinRadius = parameters.tileLodMinRadius,
                                                     .tileLodScale = parameters.tileLodScale,
                                                     .tileLodPitchThreshold = parameters.tileLodPitchThreshold,
                                                     .tileLodMode = parameters.tileLodMode,
                                                     .tileLodMinZoom = parameters.tileLodMinZoom,
                                                     .tileCoverMinElevationMeters = parameters.tileCoverMinElevationMeters,
                                                     .tileCoverMaxElevationMeters = parameters.tileCoverMaxElevationMeters,
                                                     .tileCoverMaxTiles = parameters.tileCoverMaxTiles};

    // Raster DEM is not a normal visual source: at pitched zoom-outs the
    // camera can drop below the DEM archive's minzoom while terrain still
    // needs a mesh/drape cover for the visible ground. Clamp to the source
    // minzoom instead of returning no tiles, otherwise terrain disappears
    // and drape-capable layers fall back to the bare background.
    const bool underMinRasterDEM = type == SourceType::RasterDEM && overscaledZoom < zoomRange.min;
    if (std::cmp_greater_equal(overscaledZoom, zoomRange.min) || underMinRasterDEM) {
        int32_t idealZoom = std::min<int32_t>(
            zoomRange.max,
            std::max<int32_t>(zoomRange.min, overscaledZoom));

        // Make sure we're not reparsing overzoomed raster tiles.
        if (type == SourceType::Raster || type == SourceType::RasterDEM) {
            tileZoom = idealZoom;
        }

        // Only attempt prefetching in continuous mode.
        if (parameters.mode == MapMode::Continuous && type != style::SourceType::GeoJSON &&
            type != style::SourceType::Annotations) {
            // Request lower zoom level tiles (if configured to do so) in an attempt
            // to show something on the screen faster at the cost of a little of bandwidth.
            const uint8_t prefetchZoomDelta = sourcePrefetchZoomDelta ? *sourcePrefetchZoomDelta
                                                                      : parameters.prefetchZoomDelta;
            if (prefetchZoomDelta) {
                panZoom = std::max<int32_t>(tileZoom - prefetchZoomDelta, zoomRange.min);
            }

            if (panZoom < idealZoom) {
                panTiles = util::tileCover(tileCoverParameters, panZoom, zoomRange);
            }
        }

        idealTiles = util::tileCover(tileCoverParameters, idealZoom, zoomRange, tileZoom);
        if (parameters.mode == MapMode::Tile && type != SourceType::Raster && type != SourceType::RasterDEM &&
            idealTiles.size() > 1) {
            mbgl::Log::Warning(mbgl::Event::General,
                               "Provided camera options returned " + std::to_string(idealTiles.size()) +
                                   " tiles, only " + util::toString(idealTiles[0]) + " is taken in Tile mode.");
            idealTiles = {idealTiles[0]};
        }
    }

    // Stores a list of all the tiles that we're definitely going to retain.
    // There are two kinds of tiles we need: the ideal tiles determined by the
    // tile cover. They may not yet be in use because they're still loading. In
    // addition to that, we also need to retain all tiles that we're actively
    // using, e.g. as a replacement for tile that aren't loaded yet.
    std::set<OverscaledTileID> retain;

    auto retainTileFn = [&](Tile& tile, TileNecessity necessity) -> void {
        if (retain.emplace(tile.id).second) {
            tile.setUpdateParameters({.minimumUpdateInterval = minimumUpdateInterval, .isVolatile = isVolatile});
            tile.setNecessity(necessity);
        }

        if (needsRelayout) {
            tile.setLayers(layers);
        }
    };
    auto getTileFn = [&](const OverscaledTileID& tileID) -> Tile* {
        auto it = tiles.find(tileID);
        return it == tiles.end() ? nullptr : it->second.get();
    };

    // The min and max zoom for TileRange are based on the updateRenderables
    // algorithm. Tiles are created at the ideal tile zoom or at lower zoom
    // levels. Child tiles are used from the cache, but not created.
    std::optional<util::TileRange> tileRange = std::nullopt;
    if (bounds) {
        int32_t maxZoom = (parameters.tileLodMode == TileLodMode::Distance)
                              ? zoomRange.max
                              : std::min(tileZoom, static_cast<int32_t>(zoomRange.max));
        tileRange = util::TileRange::fromLatLngBounds(*bounds, zoomRange.min, maxZoom);
    }
    auto createTileFn = [&](const OverscaledTileID& tileID) -> Tile* {
        if (tileRange && !tileRange->contains(tileID.canonical)) {
            return nullptr;
        }
        std::unique_ptr<Tile> tile = cache.pop(tileID);
        if (!tile) {
            tile = createTile(tileID, observer);
            if (!tile) return nullptr;
            tile->setLayers(layers);
        }

        return tiles.emplace(tileID, std::move(tile)).first->second.get();
    };

    auto previouslyRenderedTiles = std::move(renderedTiles);

    auto renderTileFn = [&](const UnwrappedTileID& tileID, Tile& tile) {
        addRenderTile(tileID, tile);
        previouslyRenderedTiles.erase(tileID); // Still rendering this tile, no need for special fading logic.
        tile.markRenderedIdeal();
    };

    renderedTiles.clear();

    if (!panTiles.empty()) {
        algorithm::updateRenderables(
            getTileFn,
            createTileFn,
            retainTileFn,
            [](const UnwrappedTileID&, Tile&) {},
            panTiles,
            emptyPrefetchedTiles,
            zoomRange,
            maxParentTileOverscaleFactor);
    }

    algorithm::updateRenderables(getTileFn,
                                 createTileFn,
                                 retainTileFn,
                                 renderTileFn,
                                 idealTiles,
                                 tiles,
                                 zoomRange,
                                 maxParentTileOverscaleFactor);

    // KLATTRA cover-hold v2 (2D transition flash): mid-gesture the tile cover
    // legitimately fragments (fractional-zoom flips thrash the ideal zoom and
    // the cover can be a single column for a few frames — traska.34/.35
    // device data, ids on tape). Sources with deep cached ancestors bridge
    // the fragments invisibly; the topo vector source (minzoom ~8, sparse
    // cache) and the DEM relief often cannot, so their uncovered viewport
    // area rendered nothing for a frame burst — the black flash. The v1
    // predicate (hold while overlapping an unpainted IDEAL tile) failed
    // exactly here: the uncovered screen area has no ideal tile at all when
    // the ideal list itself is the fragment.
    //
    // v2 holds relative to the PREVIOUS cover instead: keep a previously
    // rendered tile while no same-or-shallower rendered tile covers its area,
    // for at most coverHoldMaxFrames frames. The age cap makes every hold
    // self-extinguishing (pans and steady-state zoom-ins age out in ~250 ms,
    // masked behind the new cover; updateTileMasks dedupes overlap per
    // pixel), and zoom-outs drop instantly when the shallower parent renders.
    // Skipped on relayout — stale-style tiles must not linger.
    // Expiry (v5): every timer cap turned out to expire whole hold cohorts
    // mid-gesture — the tiles uncovered by one fragmentation event age in
    // lockstep and drop on the same frame, a synchronized viewport-wide
    // flash recurring each cap interval (traska.37: 15 frames; traska.38:
    // 120 frames, aged>0 exactly on every DIP). A hold now expires when it
    // stops overlapping the union of ideal tiles seen in the last
    // coverHoldWindow updates — i.e. when it genuinely leaves the viewport —
    // with the age cap kept only as a pathological backstop (~10 s).
    // .68: RasterDEM needs this hold too. Disabling it in .67 removed the
    // overlapping green surfaces only when the raw source happened to become
    // disjoint, but exposed the original fragmented-cover hole as a pure-black
    // frame (95 bindings immediately after resume versus the normal 112).
    // RenderTerrain now expands updateTileMasks into disjoint terrain leaves,
    // so held parents fill only uncovered child regions instead of drawing a
    // second full displaced surface underneath them.
    static const bool coverHoldDisabled = std::getenv("KLATTRA_DISABLE_COVERHOLD") != nullptr;
    static const uint16_t coverHoldMaxFrames = [] {
        const char* v = std::getenv("KLATTRA_COVERHOLD_FRAMES");
        const long parsed = v ? std::strtol(v, nullptr, 10) : 0;
        return static_cast<uint16_t>((parsed > 0 && parsed < 65535) ? parsed : 3600);
    }();
    static const uint32_t coverHoldWindow = [] {
        const char* v = std::getenv("KLATTRA_COVERHOLD_WINDOW");
        const long parsed = v ? std::strtol(v, nullptr, 10) : 0;
        return static_cast<uint32_t>((parsed > 0 && parsed < 100000) ? parsed : 90);
    }();
    // DIAG: RasterDEM parents contain complete elevation data, unlike sparse
    // vector parents. Allow a newly renderable exact ideal parent to retire
    // retained children from the previous zoom generation. Keep this launch
    // gated until physical-device movement confirms continuity.
    static const bool rasterDEMRetireLoadedAncestors = [] {
        const char* v = std::getenv("KLATTRA_RASTER_DEM_RETIRE_LOADED_ANCESTORS");
        return v && *v && !(*v == '0' || *v == 'f' || *v == 'F');
    }();
    // DIAG: do not reduce normal continuity quality pre-emptively. After the
    // platform reports an actual memory warning, halve only the custom
    // RasterDEM fallback allowance for active terrain. The current active cover,
    // holdForFade tiles, and RenderTerrain's atomic complete-cover swap are
    // independent of this budget.
    static const bool rasterDEMPressureHalfHoldEnabled = [] {
        const char* v = std::getenv("KLATTRA_RASTER_DEM_PRESSURE_HALF_HOLD");
        return v && *v && !(*v == '0' || *v == 'f' || *v == 'F');
    }();
    // DIAG: iOS 18.1.1 on the physical iPhone 13 can jump directly from a
    // healthy process footprint to the fatal per-process jetsam limit without
    // delivering UIApplicationDidReceiveMemoryWarning. When explicitly set,
    // use the same sticky half-hold safety policy before that final surge.
    static const int64_t rasterDEMHighWaterHalfHoldMB = [] {
        const char* v = std::getenv("KLATTRA_RASTER_DEM_HIGHWATER_HALF_HOLD_MB");
        if (!v || !*v) return int64_t{0};
        char* end = nullptr;
        const long parsed = std::strtol(v, &end, 10);
        return end != v && parsed > 0 ? static_cast<int64_t>(parsed) : int64_t{0};
    }();

    // GeoJSON sources are local and immediately renderable. Extending their
    // previous cover provides no loading bridge, while terrain-draped line
    // layers cannot use the main-pass stencil masks that normally de-duplicate
    // retained parent/child tiles. Keep standard holdForFade handling below,
    // but reserve the custom long-lived cover hold for network-backed sources.
    const bool coverHoldEnabledForSource = !coverHoldDisabled && type != SourceType::GeoJSON;

    const bool boundedRasterDEMCoverHold = coverHoldEnabledForSource && type == SourceType::RasterDEM;
    const std::size_t normalRasterDEMCoverHoldBudget =
        boundedRasterDEMCoverHold ? cover_hold::rasterDEMCoverHoldBudget(parameters.tileCoverMaxTiles) : 0;
    const std::size_t configuredNonRasterDEMPressureCap = klattraNonRasterDEMPressureCoverHoldCap();
    const bool nonRasterDEMPressureCapEnabled =
        coverHoldEnabledForSource && type != SourceType::RasterDEM && configuredNonRasterDEMPressureCap > 0;
    KlattraMemorySample highWaterMemory;
    const bool highWaterHalfHoldEnabled = boundedRasterDEMCoverHold && parameters.usedByTerrain &&
                                          rasterDEMHighWaterHalfHoldMB > 0;
    if (highWaterHalfHoldEnabled) {
        highWaterMemory = klattraMemorySample();
        if (!rasterDEMPressureHalfHold && highWaterMemory.physicalMB >= rasterDEMHighWaterHalfHoldMB) {
            // Match the explicit iOS memory-pressure path: shed cached tiles
            // immediately, then keep the reduced cover-hold budget sticky for
            // the rest of this source's lifetime.
            cache.clear();
            rasterDEMPressureHalfHold = true;
            klattraCoverHoldHighWaterTripped().store(true, std::memory_order_release);
            cover_hold::activateProcessPressure();
            ++memoryHighWaterTrips;
            Log::Warning(Event::Render,
                         "[KLATTRA DEM HIGHWATER] source=" + sourceImpl.id +
                             " thresholdMB=" + std::to_string(rasterDEMHighWaterHalfHoldMB) +
                             " physMB=" + std::to_string(highWaterMemory.physicalMB) +
                             " trips=" + std::to_string(memoryHighWaterTrips));
            static const bool traceStderr = std::getenv("KLATTRA_TRACE_STDERR") != nullptr;
            if (traceStderr) {
                fprintf(stderr,
                        "[KLATTRA_TRACE] [KLATTRA DEM HIGHWATER] source=%s thresholdMB=%lld physMB=%lld trips=%u\n",
                        sourceImpl.id.c_str(),
                        static_cast<long long>(rasterDEMHighWaterHalfHoldMB),
                        static_cast<long long>(highWaterMemory.physicalMB),
                        memoryHighWaterTrips);
            }
        }
    }
    if (nonRasterDEMPressureCapEnabled && !nonRasterDEMPressureCoverHold &&
        cover_hold::processPressureActive()) {
        cache.clear();
        nonRasterDEMPressureCoverHold = true;
        if (klattraCoverHoldHighWaterTripped().load(std::memory_order_acquire)) {
            ++memoryHighWaterTrips;
            Log::Warning(Event::Render,
                         "[KLATTRA SOURCE HIGHWATER] source=" + sourceImpl.id +
                             " thresholdMB=" + std::to_string(rasterDEMHighWaterHalfHoldMB) +
                             " cap=" + std::to_string(configuredNonRasterDEMPressureCap) +
                             " trips=" + std::to_string(memoryHighWaterTrips));
            static const bool traceStderr = std::getenv("KLATTRA_TRACE_STDERR") != nullptr;
            if (traceStderr) {
                fprintf(stderr,
                        "[KLATTRA_TRACE] [KLATTRA SOURCE HIGHWATER] source=%s thresholdMB=%lld cap=%zu trips=%u\n",
                        sourceImpl.id.c_str(),
                        static_cast<long long>(rasterDEMHighWaterHalfHoldMB),
                        configuredNonRasterDEMPressureCap,
                        memoryHighWaterTrips);
            }
        }
    }
    const bool halfHoldEnabled = rasterDEMPressureHalfHoldEnabled || highWaterHalfHoldEnabled;
    const bool pressureHalfHold = boundedRasterDEMCoverHold && parameters.usedByTerrain &&
                                  halfHoldEnabled && rasterDEMPressureHalfHold;
    const bool boundedNonRasterDEMCoverHold =
        nonRasterDEMPressureCapEnabled && nonRasterDEMPressureCoverHold;
    const bool boundedCoverHold = boundedRasterDEMCoverHold || boundedNonRasterDEMCoverHold;
    const std::size_t coverHoldBudget = boundedRasterDEMCoverHold
                                            ? (pressureHalfHold
                                                   ? cover_hold::rasterDEMPressureCoverHoldBudget(
                                                         normalRasterDEMCoverHoldBudget)
                                                   : normalRasterDEMCoverHoldBudget)
                                            : (boundedNonRasterDEMCoverHold
                                                   ? cover_hold::nonRasterDEMPressureCoverHoldBudget(
                                                         configuredNonRasterDEMPressureCap, idealTiles.size())
                                                   : 0);
    const std::size_t recentIdealCap = boundedCoverHold ? cover_hold::recentIdealBudget(coverHoldBudget) : 0;
    const bool retireLoadedRasterDEMAncestors =
        boundedRasterDEMCoverHold && parameters.usedByTerrain && rasterDEMRetireLoadedAncestors;
    std::set<UnwrappedTileID> currentIdealTiles;
    if (retireLoadedRasterDEMAncestors) {
        for (const auto& idealTile : idealTiles) {
            currentIdealTiles.insert(idealTile.toUnwrapped());
        }
    }

    ++coverHoldUpdateIndex;
    std::size_t recentIdealExpired = 0;
    std::size_t recentIdealEvicted = 0;
    if (coverHoldEnabledForSource) {
        for (const auto& idealTile : idealTiles) {
            recentIdealTiles[idealTile.toUnwrapped()] = coverHoldUpdateIndex;
        }
        for (auto it = recentIdealTiles.begin(); it != recentIdealTiles.end();) {
            if (coverHoldUpdateIndex - it->second > coverHoldWindow) {
                it = recentIdealTiles.erase(it);
                ++recentIdealExpired;
            } else {
                ++it;
            }
        }
        if (boundedCoverHold) {
            recentIdealEvicted = cover_hold::boundRecentIdealTiles(
                recentIdealTiles, coverHoldUpdateIndex, recentIdealCap);
        }
    }

    std::size_t coverHeld = 0;
    std::size_t coverEligible = 0;
    std::size_t coverBudgetEvicted = 0;
    std::size_t fadeHeld = 0;
    std::size_t coverRejectedAge = 0, coverRejectedNotRenderable = 0, coverRetiredCovered = 0;
    std::size_t coverRetiredAncestor = 0;
    std::size_t coverExpiredOffscreen = 0;
    const std::size_t coverRejectedRelayout = needsRelayout ? previouslyRenderedTiles.size() : 0;
    std::map<UnwrappedTileID, uint16_t> nextCoverHoldAges;
    std::vector<cover_hold::Candidate> boundedCandidates;
    for (auto previouslyRenderedTile : previouslyRenderedTiles) {
        Tile& tile = previouslyRenderedTile.second;
        tile.markRenderedPreviously();
        if (tile.holdForFade()) {
            // Since it was rendered in the last frame, we know we have it
            // Don't mark the tile "Required" to avoid triggering a new network request
            retainTileFn(tile, TileNecessity::Optional);
            addRenderTile(previouslyRenderedTile.first, tile);
            ++fadeHeld;
            continue;
        }
        if (!coverHoldEnabledForSource || needsRelayout) {
            continue;
        }
        if (!tile.isRenderable()) {
            ++coverRejectedNotRenderable;
            continue;
        }
        const UnwrappedTileID& previousID = previouslyRenderedTile.first;
        if (retireLoadedRasterDEMAncestors &&
            cover_hold::hasRenderableIdealAncestor(previousID, currentIdealTiles, renderedTiles)) {
            ++coverRetiredCovered;
            ++coverRetiredAncestor;
            continue;
        }
        // Generic same-or-deeper coverage ONLY (v3): a shallower rendered parent can
        // be feature-empty at coarse zooms (the topo archive carries no land
        // polygons below ~z10) — it covers the area geometrically while
        // painting nothing, which IS the flash (traska.36 device data:
        // z7-z9 parents inside every dip set, holds credited them, land
        // fills still painted 1-3 tile fragments). RasterDEM's launch-gated,
        // exact-ideal-parent exception is handled above. A previously rendered
        // tile is replaced only once all four child quadrants are rendered;
        // everything else rides the age cap (~250 ms, masked behind newer
        // tiles by updateTileMasks) — which is also what retires zoom-out
        // holds, legacy-crossfade style.
        bool covered = true;
        for (const auto& child : previousID.children()) {
            if (renderedTiles.find(child) == renderedTiles.end()) {
                covered = false;
                break;
            }
        }
        if (covered) {
            ++coverRetiredCovered;
            continue;
        }
        const auto overlapAge = cover_hold::newestOverlapAge(previousID, recentIdealTiles, coverHoldUpdateIndex);
        if (!overlapAge) {
            ++coverExpiredOffscreen;
            continue;
        }
        const auto ageIt = coverHoldAges.find(previousID);
        const uint16_t age = ageIt == coverHoldAges.end() ? 0 : ageIt->second;
        if (age >= coverHoldMaxFrames) {
            ++coverRejectedAge;
            continue;
        }
        ++coverEligible;
        if (boundedCoverHold) {
            boundedCandidates.push_back({previousID, age, *overlapAge});
            continue;
        }
        nextCoverHoldAges[previousID] = static_cast<uint16_t>(age + 1);
        retainTileFn(tile, TileNecessity::Optional);
        addRenderTile(previousID, tile);
        ++coverHeld;
    }

    if (boundedCoverHold) {
        const auto selected = cover_hold::selectCandidates(std::move(boundedCandidates), coverHoldBudget);
        coverBudgetEvicted = coverEligible - selected.size();
        for (const auto& candidate : selected) {
            const auto previousIt = previouslyRenderedTiles.find(candidate.id);
            assert(previousIt != previouslyRenderedTiles.end());
            Tile& tile = previousIt->second;
            nextCoverHoldAges[candidate.id] = static_cast<uint16_t>(candidate.holdAge + 1);
            retainTileFn(tile, TileNecessity::Optional);
            addRenderTile(candidate.id, tile);
            ++coverHeld;
        }
        assert(coverHeld <= coverHoldBudget);
    }
    coverHoldAges = std::move(nextCoverHoldAges);

    // KLATTRA diagnostics: cover-hold activity. Logs on change (including the
    // return to 0) plus a 1 Hz heartbeat while holds are active. RasterDEM is
    // default-on in .68 because held coverage is the black-hole referee;
    // other sources remain opt-in. KLATTRA_LOG_COVERHOLD=0 disables it.
    {
        static const int8_t coverHoldLogMode = [] {
            const char* v = std::getenv("KLATTRA_LOG_COVERHOLD");
            if (!v) return int8_t{-1};
            return (*v == '0' || *v == 'f' || *v == 'F') ? int8_t{0} : int8_t{1};
        }();
        const bool coverHoldLog = coverHoldLogMode > 0 ||
                                  (coverHoldLogMode < 0 && type == SourceType::RasterDEM);
        if (coverHoldLog) {
            struct State {
                std::size_t last = SIZE_MAX;
                std::size_t lastAged = 0;
                std::chrono::steady_clock::time_point lastLog{};
            };
            static std::unordered_map<const void*, State> states;
            auto& st = states[this];
            const auto now = std::chrono::steady_clock::now();
            const bool changed = (st.last == SIZE_MAX) ? coverHeld > 0
                                                       : (coverHeld != st.last || coverRejectedAge != st.lastAged);
            const bool heartbeat = coverHeld > 0 || type == SourceType::RasterDEM;
            if (changed || (heartbeat && now - st.lastLog >= std::chrono::seconds(1))) {
                st.lastLog = now;
                const auto memory = klattraMemorySample();
                Log::Warning(
                    Event::Render,
                    "[KLATTRA COVERHOLD] source=" + sourceImpl.id + " ideal=" + std::to_string(idealTiles.size()) +
                        " rendered=" + std::to_string(renderedTiles.size()) + " held=" + std::to_string(coverHeld) +
                        " eligible=" + std::to_string(coverEligible) + " budget=" + std::to_string(coverHoldBudget) +
                        " budgetEvicted=" + std::to_string(coverBudgetEvicted) + " fadeHeld=" +
                        std::to_string(fadeHeld) + " recentIdeal=" + std::to_string(recentIdealTiles.size()) +
                        " recentExpired=" + std::to_string(recentIdealExpired) + " recentEvicted=" +
                        std::to_string(recentIdealEvicted) + " retired=" + std::to_string(coverRetiredCovered) +
                        " ancestorMode=" + std::to_string(retireLoadedRasterDEMAncestors ? 1 : 0) +
                        " ancestorRetired=" + std::to_string(coverRetiredAncestor) +
                        " pressureMode=" + std::to_string(pressureHalfHold ? 1 : 0) +
                        " nonDEMPressureMode=" + std::to_string(boundedNonRasterDEMCoverHold ? 1 : 0) +
                        " pressureEvents=" + std::to_string(memoryPressureEvents) +
                        " highWaterMB=" + std::to_string(rasterDEMHighWaterHalfHoldMB) +
                        " highWaterTrips=" + std::to_string(memoryHighWaterTrips) +
                        " aged=" + std::to_string(coverRejectedAge) +
                        " expiredOff=" + std::to_string(coverExpiredOffscreen) +
                        " notRenderable=" + std::to_string(coverRejectedNotRenderable) +
                        " relayout=" + std::to_string(coverRejectedRelayout) +
                        " physMB=" + std::to_string(memory.physicalMB) +
                        " physPeakMB=" + std::to_string(memory.sampledPeakMB));
                static const bool traceStderr = std::getenv("KLATTRA_TRACE_STDERR") != nullptr;
                if (traceStderr) {
                    fprintf(stderr,
                            "[KLATTRA_TRACE] [KLATTRA COVERHOLD] source=%s ideal=%zu rendered=%zu "
                            "held=%zu eligible=%zu budget=%zu budgetEvicted=%zu fadeHeld=%zu "
                            "recentIdeal=%zu recentExpired=%zu recentEvicted=%zu retired=%zu "
                            "ancestorMode=%d ancestorRetired=%zu pressureMode=%d nonDEMPressureMode=%d "
                            "pressureEvents=%u aged=%zu "
                            "highWaterMB=%lld highWaterTrips=%u expiredOff=%zu notRenderable=%zu relayout=%zu "
                            "physMB=%lld physPeakMB=%lld\n",
                            sourceImpl.id.c_str(),
                            idealTiles.size(),
                            renderedTiles.size(),
                            coverHeld,
                            coverEligible,
                            coverHoldBudget,
                            coverBudgetEvicted,
                            fadeHeld,
                            recentIdealTiles.size(),
                            recentIdealExpired,
                            recentIdealEvicted,
                            coverRetiredCovered,
                            retireLoadedRasterDEMAncestors ? 1 : 0,
                            coverRetiredAncestor,
                            pressureHalfHold ? 1 : 0,
                            boundedNonRasterDEMCoverHold ? 1 : 0,
                            memoryPressureEvents,
                            coverRejectedAge,
                            static_cast<long long>(rasterDEMHighWaterHalfHoldMB),
                            memoryHighWaterTrips,
                            coverExpiredOffscreen,
                            coverRejectedNotRenderable,
                            coverRejectedRelayout,
                            static_cast<long long>(memory.physicalMB),
                            static_cast<long long>(memory.sampledPeakMB));
                }
            }
            st.last = coverHeld;
            st.lastAged = coverRejectedAge;
        }
    }

    if (type != SourceType::Annotations && cacheEnabled) {
        auto conservativeCacheSize = static_cast<size_t>(
            std::max(static_cast<double>(parameters.transformState.getSize().width) / tileSize, 1.0) *
            std::max(static_cast<double>(parameters.transformState.getSize().height) / tileSize, 1.0) *
            (parameters.transformState.getMaxZoom() - parameters.transformState.getMinZoom() + 1) * 0.5);
        // .66: see klattraRasterCacheScale above — the drape gap-fill's
        // candidate reservoir must outlive the flight corridor.
        if (type == SourceType::Raster) {
            conservativeCacheSize *= klattraRasterCacheScale();
        }
        cache.setSize(conservativeCacheSize);
    } else {
        cache.setSize(0);
    }

    // Remove stale tiles. This goes through the (sorted!) tiles map and retain
    // set in lockstep and removes items from tiles that don't have the
    // corresponding key in the retain set.
    {
        auto tilesIt = tiles.begin();
        auto retainIt = retain.begin();
        while (tilesIt != tiles.end()) {
            if (retainIt == retain.end() || tilesIt->first < *retainIt) {
                // Remove the tile from the map.
                // If it requires re-layout, discard it asynchronously, otherwise keep it in the cache
                const auto key = tilesIt->first;
                if (std::unique_ptr<Tile> tile = std::move(tiles.extract(tilesIt++).mapped())) {
                    if (needsRelayout) {
                        cache.deferredRelease(std::move(tile));
                    } else {
                        tile->setNecessity(TileNecessity::Optional);
                        cache.add(key, std::move(tile));
                    }
                }
            } else {
                if (!(*retainIt < tilesIt->first)) {
                    ++tilesIt;
                }
                ++retainIt;
            }
        }
    }

    for (auto& pair : tiles) {
        pair.second->setShowCollisionBoxes(parameters.debugOptions & MapDebugOptions::Collision);
    }

    // Initialize renderable tiles and update the contained layer render data.
    for (auto& entry : renderedTiles) {
        Tile& tile = entry.second;
        assert(tile.isRenderable());
        tile.usedByRenderedLayers = false;

        const bool holdForFade = tile.holdForFade();
        for (const auto& layerProperties : layers) {
            const auto* typeInfo = layerProperties->baseImpl->getTypeInfo();
            if (holdForFade && typeInfo->fadingTiles == LayerTypeInfo::FadingTiles::NotRequired) {
                continue;
            }
            tile.usedByRenderedLayers |= tile.layerPropertiesUpdated(layerProperties);
        }
    }

    // KLATTRA diagnostics (2D black-flash hunt): rendered-set continuity.
    // land-open DRAWABLES collapse 6→1→6 across zoom transitions (traska.31
    // device data); this tells whether the TILE SET dips with them (cover/
    // retention side) or holds (drawable-culling side). Logs on any ≥2 dip
    // plus a 1 Hz heartbeat. RasterDEM defaults on in .68; opt out with
    // KLATTRA_LOG_SRCTILES=0.
    {
        static const int8_t srcTilesLogMode = [] {
            const char* v = std::getenv("KLATTRA_LOG_SRCTILES");
            if (!v) return int8_t{-1};
            return (*v == '0' || *v == 'f' || *v == 'F') ? int8_t{0} : int8_t{1};
        }();
        const bool srcTilesLog = srcTilesLogMode > 0 ||
                                 (srcTilesLogMode < 0 && type == SourceType::RasterDEM);
        if (srcTilesLog) {
            struct State {
                std::size_t last = SIZE_MAX;
                std::chrono::steady_clock::time_point lastLog{};
            };
            static std::unordered_map<const void*, State> states;
            auto& st = states[this];
            const std::size_t rendered = renderedTiles.size();
            const auto now = std::chrono::steady_clock::now();
            const bool dip = st.last != SIZE_MAX && rendered + 2 <= st.last;
            if (dip || now - st.lastLog >= std::chrono::seconds(1)) {
                st.lastLog = now;
                // On a dip, name the surviving rendered tiles so the log can
                // be diffed against TILEPAINT's painted-tile ids.
                std::string dipIDs;
                if (dip) {
                    dipIDs = " ids=";
                    for (const auto& entry : renderedTiles) {
                        if (dipIDs.size() > 5) dipIDs += ' ';
                        dipIDs += std::to_string(entry.first.canonical.z) + ":" +
                                  std::to_string(entry.first.canonical.x) + "," +
                                  std::to_string(entry.first.canonical.y);
                    }
                }
                Log::Warning(Event::Render,
                             "[KLATTRA SRCTILES] source=" + sourceImpl.id + " rendered=" + std::to_string(rendered) +
                                 " prev=" + (st.last == SIZE_MAX ? std::string("-") : std::to_string(st.last)) +
                                 " tilesHeld=" + std::to_string(tiles.size()) +
                                 " ideal=" + std::to_string(idealTiles.size()) + (dip ? " DIP" : "") + dipIDs);
                static const bool traceStderr = std::getenv("KLATTRA_TRACE_STDERR") != nullptr;
                if (traceStderr) {
                    fprintf(stderr,
                            "[KLATTRA_TRACE] [KLATTRA SRCTILES] source=%s rendered=%zu prev=%zu tilesHeld=%zu%s\n",
                            sourceImpl.id.c_str(),
                            rendered,
                            st.last == SIZE_MAX ? 0 : st.last,
                            tiles.size(),
                            dip ? " DIP" : "");
                }
            }
            st.last = rendered;
        }
    }

    cache.deferPendingReleases();
}

void TilePyramid::handleWrapJump(float lng) {
    // On top of the regular z/x/y values, TileIDs have a `wrap` value that specify
    // which cppy of the world the tile belongs to. For example, at `lng: 10` you
    // might render z/x/y/0 while at `lng: 370` you would render z/x/y/1.
    //
    // When lng values get wrapped (going from `lng: 370` to `long: 10`) you expect
    // to see the same thing on the screen (370 degrees and 10 degrees is the same
    // place in the world) but all the TileIDs will have different wrap values.
    //
    // In order to make this transition seamless, we calculate the rounded difference of
    // "worlds" between the last frame and the current frame. If the map panned by
    // a world, then we can assign all the tiles new TileIDs with updated wrap values.
    // For example, assign z/x/y/1 a new id: z/x/y/0. It is the same tile, just rendered
    // in a different position.
    //
    // This enables us to reuse the tiles at more ideal locations and prevent flickering.

    const float lngDifference = lng - prevLng;
    const float worldDifference = lngDifference / 360.f;
    const auto wrapDelta = static_cast<int16_t>(std::round(worldDifference));
    prevLng = lng;

    if (wrapDelta) {
        std::map<OverscaledTileID, std::unique_ptr<Tile>> newTiles;
        std::map<UnwrappedTileID, std::reference_wrapper<Tile>> newRenderTiles;
        for (auto& tile : tiles) {
            auto newID = tile.second->id.unwrapTo(tile.second->id.wrap + wrapDelta);
            tile.second->id = newID;
            newTiles.emplace(newID, std::move(tile.second));
        }
        tiles = std::move(newTiles);

        for (auto& tile : renderedTiles) {
            UnwrappedTileID newID = tile.first.unwrapTo(tile.first.wrap + wrapDelta);
            newRenderTiles.emplace(newID, tile.second);
        }
        renderedTiles = std::move(newRenderTiles);
        // Cover-hold bookkeeping is keyed by wrap-bearing ids; remapping is
        // not worth it for an antimeridian jump — just reset.
        coverHoldAges.clear();
        recentIdealTiles.clear();
    }
}

std::unordered_map<std::string, std::vector<Feature>> TilePyramid::queryRenderedFeatures(
    const ScreenLineString& geometry,
    const TransformState& transformState,
    const std::unordered_map<std::string, const RenderLayer*>& layers,
    const RenderedQueryOptions& options,
    const mat4& projMatrix,
    const SourceFeatureState& featureState) const {
    std::unordered_map<std::string, std::vector<Feature>> result;
    if (renderedTiles.empty() || geometry.empty()) {
        return result;
    }

    LineString<double> queryGeometry;
    queryGeometry.reserve(geometry.size());

    for (const auto& p : geometry) {
        queryGeometry.push_back(
            TileCoordinate::fromScreenCoordinate(transformState, 0, {p.x, transformState.getSize().height - p.y}).p);
    }

    mapbox::geometry::box<double> box = mapbox::geometry::envelope(queryGeometry);

    auto cmp = [](const UnwrappedTileID& a, const UnwrappedTileID& b) {
        return std::tie(a.canonical.z, a.canonical.y, a.wrap, a.canonical.x) <
               std::tie(b.canonical.z, b.canonical.y, b.wrap, b.canonical.x);
    };

    std::map<UnwrappedTileID, std::reference_wrapper<Tile>, decltype(cmp)> sortedTiles{
        renderedTiles.begin(), renderedTiles.end(), cmp};

    auto maxPitchScaleFactor = transformState.maxPitchScaleFactor();

    for (const auto& entry : sortedTiles) {
        const UnwrappedTileID& id = entry.first;
        Tile& tile = entry.second;

        const auto scale = static_cast<float>(transformState.getScale() /
                                              (1 << id.canonical.z)); // equivalent to std::pow(2,
                                                                      // transformState.getZoom() - id.canonical.z);
        auto queryPadding = maxPitchScaleFactor * tile.getQueryPadding(layers) * util::EXTENT / util::tileSize_D /
                            scale;

        GeometryCoordinate tileSpaceBoundsMin = TileCoordinate::toGeometryCoordinate(id, box.min);
        if (tileSpaceBoundsMin.x - queryPadding >= util::EXTENT ||
            tileSpaceBoundsMin.y - queryPadding >= util::EXTENT) {
            continue;
        }

        GeometryCoordinate tileSpaceBoundsMax = TileCoordinate::toGeometryCoordinate(id, box.max);
        if (tileSpaceBoundsMax.x + queryPadding < 0 || tileSpaceBoundsMax.y + queryPadding < 0) {
            continue;
        }

        GeometryCoordinates tileSpaceQueryGeometry;
        tileSpaceQueryGeometry.reserve(queryGeometry.size());
        for (const auto& c : queryGeometry) {
            tileSpaceQueryGeometry.push_back(TileCoordinate::toGeometryCoordinate(id, c));
        }

        tile.queryRenderedFeatures(
            result, tileSpaceQueryGeometry, transformState, layers, options, projMatrix, featureState);
    }

    return result;
}

std::vector<Feature> TilePyramid::querySourceFeatures(const SourceQueryOptions& options) const {
    std::vector<Feature> result;

    for (const auto& pair : tiles) {
        pair.second->querySourceFeatures(result, options);
    }

    return result;
}

void TilePyramid::setCacheEnabled(bool enable) {
    cacheEnabled = enable;
}

void TilePyramid::reduceMemoryUse() {
    cache.clear();
}

void TilePyramid::reduceMemoryUseForMemoryPressure() {
    reduceMemoryUse();
    ++memoryPressureEvents;
    static const bool rasterDEMPressureHalfHoldEnabled = [] {
        const char* v = std::getenv("KLATTRA_RASTER_DEM_PRESSURE_HALF_HOLD");
        return v && *v && !(*v == '0' || *v == 'f' || *v == 'F');
    }();
    if (rasterDEMPressureHalfHoldEnabled) {
        rasterDEMPressureHalfHold = true;
    }
    if (klattraNonRasterDEMPressureCoverHoldCap() > 0) {
        nonRasterDEMPressureCoverHold = true;
        cover_hold::activateProcessPressure();
    }
}

void TilePyramid::setObserver(TileObserver* observer_) {
    observer = observer_;
}

void TilePyramid::dumpDebugLogs() const {
    for (const auto& pair : tiles) {
        pair.second->dumpDebugLogs();
    }
}

void TilePyramid::clearAll() {
    fadingTiles = false;
    tiles.clear();
    renderedTiles.clear();
    coverHoldAges.clear();
    recentIdealTiles.clear();
    cache.clear();
}

void TilePyramid::addRenderTile(const UnwrappedTileID& tileID, Tile& tile) {
    assert(tile.isRenderable());
    renderedTiles.emplace(tileID, tile);
}

void TilePyramid::updateFadingTiles() {
    fadingTiles = false;
    for (auto& entry : renderedTiles) {
        Tile& tile = entry.second;
        if (tile.holdForFade()) {
            fadingTiles = true;
            tile.performedFadePlacement();
        }
    }
}

} // namespace mbgl
