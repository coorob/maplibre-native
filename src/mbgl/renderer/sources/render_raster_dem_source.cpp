#include <mbgl/renderer/sources/render_raster_dem_source.hpp>
#include <mbgl/renderer/sources/klattra_terrain_cover.hpp>
#include <mbgl/renderer/render_tile.hpp>
#include <mbgl/tile/raster_dem_tile.hpp>
#include <mbgl/algorithm/update_tile_masks.hpp>
#include <mbgl/geometry/dem_data.hpp>
#include <mbgl/renderer/buckets/hillshade_bucket.hpp>
#include <mbgl/renderer/tile_parameters.hpp>
#include <mbgl/util/logging.hpp>
#include <mbgl/util/tile_cover.hpp>
#include <mbgl/util/math.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <numbers>

namespace mbgl {

using namespace style;

namespace {

double klattraEnvTerrainLodPitchDeg() {
    // The stock 60° variable-zoom gate sits INSIDE the trail-preview pitch
    // band (46–63°): for most of a flyover no far-field tiles were emitted
    // at all, so the terrain mesh stopped at the near full-zoom radius and
    // the void backdrop showed as permanent black wedges at the frustum
    // edges (device pause-test 2026-07-04). 40° keeps the whole preview
    // band — and hand-pitched 3D browsing — inside variable-zoom cover.
    const char* value = std::getenv("KLATTRA_TERRAIN_LOD_PITCH_DEG");
    if (!value || !*value) {
        return 40.0;
    }
    char* end = nullptr;
    const double parsed = std::strtod(value, &end);
    if (end == value) {
        return 40.0;
    }
    return std::clamp(parsed, 20.0, 60.0);
}

// Shared with tile_cover.cpp's cut telemetry (each file carries its own
// copy — anonymous namespace). Opt in with KLATTRA_LOG_COVER_SUMMARY=1.
bool klattraLogCoverSummary() {
    static const bool enabled = [] {
        const char* v = std::getenv("KLATTRA_LOG_COVER_SUMMARY");
        return v && *v && !(*v == '0' || *v == 'f' || *v == 'F');
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

} // namespace

RenderRasterDEMSource::RenderRasterDEMSource(Immutable<style::TileSource::Impl> impl_,
                                             const TaggedScheduler& threadPool_)
    : RenderTileSetSource(std::move(impl_), threadPool_) {}

const style::TileSource::Impl& RenderRasterDEMSource::impl() const {
    return static_cast<const style::TileSource::Impl&>(*baseImpl);
}

const std::optional<Tileset>& RenderRasterDEMSource::getTileset() const {
    return impl().tileset;
}

void RenderRasterDEMSource::updateInternal(const Tileset& tileset,
                                           const std::vector<Immutable<LayerProperties>>& layers,
                                           const bool needsRendering,
                                           const bool needsRelayout,
                                           const TileParameters& parameters) {
    // Tilt the LOD heuristic toward variable-zoom for DEM sources so that
    // the 3D terrain mesh extends out to the visible horizon. Default
    // `tileLodMinRadius=3, tileLodScale=1` keeps a 3-tile radius of full-
    // zoom tiles around the camera and only drops to coarser tiles past
    // that radius; in practice — see TERRAIN_PROGRESS.md "Horizon
    // bleed-through" note — that never emits any lower-zoom tiles for the
    // far field at pitch ≳ 70°, so the terrain mesh stops short and the
    // flat basemap shows through at the horizon. Bumping `tileLodScale`
    // and tightening `tileLodMinRadius` for this source specifically lets
    // the existing variable-zoom logic in `tileCover()` (gated on
    // `pitch > tileLodPitchThreshold`) emit z=8..z=11 tiles for distant
    // areas without disturbing any other source's tile cover.
    //
    // Values picked, two iterations:
    //
    // - First pass `tileLodScale=4.0, MinRadius=1.0` produced ~21 DEM
    //   tiles in cover (vs 8 baseline) and closed the horizon hole, but
    //   the extra drape-tile churn exposed a latent assertion in
    //   `mtl::UploadPass::buildAttributeBindings` during fill-drape
    //   upload while zooming.
    // - Second pass `tileLodScale=2.0, MinRadius=2.0` stopped the crash
    //   but emitted z=8 horizon tiles (16× lower resolution than the
    //   z=12 foreground). When the user pans the camera, new z=12 tiles
    //   stream in over the network while the global prefetchZoomDelta=4
    //   pre-cached z=8 backup is already there — so the renderer falls
    //   back to z=8 for a few hundred ms before z=12 arrives, producing
    //   a visible "blurry pop" of low-resolution mesh + drape that
    //   resolves to crisp once the network catches up.
    // - This pass `tileLodScale=1.0, MinRadius=2.0` is calibrated to
    //   minimize the parent-fallback resolution gap: variable-zoom only
    //   ever emits down to z=10 (4× gap) in normal viewports, never
    //   z=8 (16× gap). The trade-off is a slightly smaller horizon
    //   extension distance, but the loading transitions are 4× less
    //   visually jarring. The horizon-bleed-through bug stays fixed at
    //   the original target camera (Kebnekaise, pitch 72°, dist 5000m).
    //
    // Cost: hillshade-only styles (no terrain) request a few extra DEM
    // tiles at high pitch — small (~67 KB each) and only at pitch > 60°,
    // since variable-zoom is itself pitch-gated. No effect on non-DEM
    // sources or on the foreground tile zoom.
    TileParameters demParameters = parameters;
    demParameters.tileLodMinRadius = 2.0;
    demParameters.tileLodScale = 1.0;
    // Engage variable-zoom for the whole trail-preview pitch band (46–63°),
    // not just past the stock 60° gate — see klattraEnvTerrainLodPitchDeg.
    demParameters.tileLodPitchThreshold = klattraEnvTerrainLodPitchDeg() * std::numbers::pi / 180.0;
    // Native tile cover historically culled tiles against a flat z=0 plane.
    // That under-selects pitched low-zoom terrain: raised mountains can be
    // visible even when the flat ground plane for the same tile is outside the
    // frustum, leaving the terrain as isolated relief islands in a beige
    // background. GL JS uses terrain-aware tile bounding volumes with DEM
    // min/max. Until Native has per-tile min/max here, use a conservative
    // Sweden-safe elevation envelope so cover errs on the side of drawing a
    // connecting mesh.
    //
    // This envelope is terrain-only. tileCover expands every selected tile by
    // a four-tile safety radius whenever the envelope is non-zero. Applying it
    // to a flat hillshade/color-relief source turned the 390x844 iPhone cover
    // from a handful of on-screen tiles into 90-112 ideal DEM tiles at 0deg
    // pitch, then allowed another 112 transition tiles to be retained. Flat
    // visual layers are drawn on z=0 and need the ordinary frustum cover.
    if (parameters.usedByTerrain) {
        demParameters.tileCoverMinElevationMeters = -6000.0;
        demParameters.tileCoverMaxElevationMeters = 8000.0;
    }
    // Keep the depth-read background fix from exposing an unbounded pitched
    // terrain horizon. Compact viewports retain the phone-accepted 112-tile
    // budget; tablet viewports scale to a bounded 384-tile budget that covers
    // the measured 322-tile wide iPad frustum.
    // The env override remains available for controlled A/Bs:
    //   KLATTRA_TERRAIN_MAX_RENDER_TILES=0   full cover
    //   KLATTRA_TERRAIN_MAX_RENDER_TILES=112 force phone budget
    demParameters.tileCoverMaxTiles =
        klattraTerrainRenderTileCap(parameters.transformState.getSize());

    // Hard floor on the variable-zoom cover: never emit tiles more than 2
    // zoom levels below the camera's ideal zoom for this source. At ideal
    // zoom 12 that caps the worst-case emission at z=10 (4× lower texel
    // density than z=12) instead of going to z=8 (16× lower). When the
    // user pans, the camera-side parent-fallback chain still reaches into
    // cached coarser tiles, but with the cap they never look more than
    // mildly soft — the dramatic blurry pop from a z=8 stand-in is gone.
    //
    // The floor is measured from the COVER zoom — the camera's ideal zoom
    // (via `coveringZoomLevel`) clamped into the tileset range, exactly the
    // `idealZoom` clamp `TilePyramid::update` applies before running the
    // cover — so "two levels below" always means two levels below the mesh
    // detail that is actually rendered. Measuring from the raw camera zoom
    // (previous behaviour) silently disabled variable-zoom in the regime
    // the 40° pitch gate above exists for: trail-preview flyovers run
    // camera zoom ~12.5–13.5, so the raw ideal (12–13) put the floor at
    // 10–11 while the cover itself was pinned at the DEM maxzoom 12 — far
    // tiles could coarsen at most one level, the full-frustum emission
    // stayed in the hundreds, and `tileCoverMaxTiles` cut the far field
    // straight back off. The mesh still ended at the near radius and the
    // frustum-edge voids survived the gate change unchanged (device
    // pause-test 2026-07-04). Clamped, a maxzoom-12 cover floors at z=10
    // and the whole pitched frustum fits comfortably inside the cap.
    // Never below the tileset minzoom either way: emitting below the
    // source range creates empty DEM/drape placeholders that look like
    // beige terrain holes while the main basemap is suppressed by terrain
    // coverage.
    const double demZoom = util::clamp<double>(
        parameters.transformState.getZoom() + parameters.tileLodZoomShift,
        parameters.transformState.getMinZoom(),
        parameters.transformState.getMaxZoom());
    const int32_t demIdealZoom = util::coveringZoomLevel(demZoom, SourceType::RasterDEM, impl().getTileSize());
    const int32_t demCoverZoom = std::clamp<int32_t>(demIdealZoom,
                                                     static_cast<int32_t>(tileset.zoomRange.min),
                                                     static_cast<int32_t>(tileset.zoomRange.max));
    demParameters.tileLodMinZoom = static_cast<uint8_t>(
        std::max<int32_t>(tileset.zoomRange.min, demCoverZoom - 2));

    // Keep the global `prefetchZoomDelta` (= 4 by default). With the
    // GL-JS-style parent-fallback DEM sampling now in place, a cached
    // z=8 backup tile no longer means the mesh briefly renders at z=8
    // density — the terrain drawables are keyed by the IDEAL z=12 tile
    // and sample the z=8 parent via a 1/256-area UV sub-rect, so the
    // mesh keeps z=12 vertex density and just inherits slightly softer
    // texture detail until the exact-zoom DEM arrives. The wider z=8
    // prefetch area is therefore a net win — it catches more pan
    // targets in the cache without the resolution penalty that drove
    // the earlier `delta = 2` cap.

    tilePyramid.update(layers,
                       needsRendering,
                       needsRelayout,
                       demParameters,
                       *baseImpl,
                       impl().getTileSize(),
                       tileset.zoomRange,
                       tileset.bounds,
                       [&](const OverscaledTileID& tileID, TileObserver* observer_) {
                           return std::make_unique<RasterDEMTile>(tileID, baseImpl->id, parameters, tileset, observer_);
                       });
    algorithm::updateTileMasks(tilePyramid.getRenderedTiles());

    // 1 Hz rendered-cover summary at Warning (passes the release log
    // filter). Pairs with [KLATTRA COVER] (emission/cap side) and
    // [KLATTRA TERRAIN] (drawable side): rendered-vs-kept gaps here mean
    // tiles requested but not yet (or never) loaded. Opt in:
    // KLATTRA_LOG_COVER_SUMMARY=1.
    if (klattraLogCoverSummary()) {
        static std::chrono::steady_clock::time_point lastLog{};
        const auto now = std::chrono::steady_clock::now();
        if (now - lastLog >= std::chrono::seconds(1)) {
            lastLog = now;
            std::array<uint32_t, 26> byZ{};
            std::size_t rendered = 0;
            for (const auto& [renderedID, tileRef] : tilePyramid.getRenderedTiles()) {
                (void)tileRef;
                ++byZ[std::min<std::size_t>(renderedID.canonical.z, byZ.size() - 1)];
                ++rendered;
            }
            const std::string message =
                "[KLATTRA DEM] rendered=" + std::to_string(rendered) +
                " byZ=" + klattraZoomHistogramString(byZ) +
                " zoom=" + std::to_string(parameters.transformState.getZoom()) +
                " pitchDeg=" +
                std::to_string(parameters.transformState.getPitch() * 180.0 / std::numbers::pi) +
                " terrain=" + std::to_string(parameters.usedByTerrain ? 1 : 0) +
                " viewport=" + std::to_string(parameters.transformState.getSize().width) + "x" +
                std::to_string(parameters.transformState.getSize().height) +
                " lodFloor=" + std::to_string(static_cast<int>(demParameters.tileLodMinZoom)) +
                " cap=" + std::to_string(demParameters.tileCoverMaxTiles);
            Log::Warning(Event::Render, message);
            if (std::getenv("KLATTRA_TRACE_STDERR") != nullptr) {
                std::fprintf(stderr, "[KLATTRA_TRACE] %s\n", message.c_str());
            }
        }
    }
}

void RenderRasterDEMSource::onTileChanged(Tile& tile) {
    auto& demtile = static_cast<RasterDEMTile&>(tile);

    std::map<DEMTileNeighbors, DEMTileNeighbors> opposites = {
        {DEMTileNeighbors::Left, DEMTileNeighbors::Right},
        {DEMTileNeighbors::Right, DEMTileNeighbors::Left},
        {DEMTileNeighbors::TopLeft, DEMTileNeighbors::BottomRight},
        {DEMTileNeighbors::TopCenter, DEMTileNeighbors::BottomCenter},
        {DEMTileNeighbors::TopRight, DEMTileNeighbors::BottomLeft},
        {DEMTileNeighbors::BottomRight, DEMTileNeighbors::TopLeft},
        {DEMTileNeighbors::BottomCenter, DEMTileNeighbors::TopCenter},
        {DEMTileNeighbors::BottomLeft, DEMTileNeighbors::TopRight}};

    if (tile.isRenderable() && demtile.neighboringTiles != DEMTileNeighbors::Complete) {
        const CanonicalTileID canonical = tile.id.canonical;
        const auto dim = static_cast<uint32_t>(std::pow(2, canonical.z));
        const uint32_t px = (canonical.x - 1 + dim) % dim;
        const int pxw = canonical.x == 0 ? tile.id.wrap - 1 : tile.id.wrap;
        const uint32_t nx = (canonical.x + 1 + dim) % dim;
        const int nxw = (canonical.x + 1 == dim) ? tile.id.wrap + 1 : tile.id.wrap;

        auto getNeighbor = [&](DEMTileNeighbors mask) {
            if (mask == DEMTileNeighbors::Left) {
                return OverscaledTileID(tile.id.overscaledZ, pxw, canonical.z, px, canonical.y);
            } else if (mask == DEMTileNeighbors::Right) {
                return OverscaledTileID(tile.id.overscaledZ, nxw, canonical.z, nx, canonical.y);
            } else if (mask == DEMTileNeighbors::TopLeft) {
                return OverscaledTileID(tile.id.overscaledZ, pxw, canonical.z, px, canonical.y - 1);
            } else if (mask == DEMTileNeighbors::TopCenter) {
                return OverscaledTileID(tile.id.overscaledZ, tile.id.wrap, canonical.z, canonical.x, canonical.y - 1);
            } else if (mask == DEMTileNeighbors::TopRight) {
                return OverscaledTileID(tile.id.overscaledZ, nxw, canonical.z, nx, canonical.y - 1);
            } else if (mask == DEMTileNeighbors::BottomLeft) {
                return OverscaledTileID(tile.id.overscaledZ, pxw, canonical.z, px, canonical.y + 1);
            } else if (mask == DEMTileNeighbors::BottomCenter) {
                return OverscaledTileID(tile.id.overscaledZ, tile.id.wrap, canonical.z, canonical.x, canonical.y + 1);
            } else if (mask == DEMTileNeighbors::BottomRight) {
                return OverscaledTileID(tile.id.overscaledZ, nxw, canonical.z, nx, canonical.y + 1);
            } else {
                throw std::runtime_error("mask is not a valid tile neighbor");
            }
        };

        for (uint8_t i = 0; i < 8; i++) {
            auto mask = DEMTileNeighbors(std::pow(2, i));
            // only backfill if this neighbor has not been previously backfilled
            if ((demtile.neighboringTiles & mask) != mask) {
                OverscaledTileID neighborid = getNeighbor(mask);
                Tile* renderableNeighbor = tilePyramid.getTile(neighborid);
                if (renderableNeighbor != nullptr && renderableNeighbor->isRenderable()) {
                    auto& borderTile = static_cast<RasterDEMTile&>(*renderableNeighbor);
                    demtile.backfillBorder(borderTile, mask);

                    // if the border tile has not been backfilled by a previous
                    // instance of the main tile, backfill its corresponding
                    // neighbor as well.
                    const DEMTileNeighbors& borderMask = opposites[mask];
                    if ((borderTile.neighboringTiles & borderMask) != borderMask) {
                        borderTile.backfillBorder(demtile, borderMask);
                    }
                }
            }
        }
    }
    RenderTileSource::onTileChanged(tile);
}

std::unordered_map<std::string, std::vector<Feature>> RenderRasterDEMSource::queryRenderedFeatures(
    const ScreenLineString&,
    const TransformState&,
    const std::unordered_map<std::string, const RenderLayer*>&,
    const RenderedQueryOptions&,
    const mat4&) const {
    return std::unordered_map<std::string, std::vector<Feature>>{};
}

std::vector<Feature> RenderRasterDEMSource::querySourceFeatures(const SourceQueryOptions&) const {
    return {};
}

} // namespace mbgl
