#include <mbgl/renderer/sources/render_raster_dem_source.hpp>
#include <mbgl/renderer/render_tile.hpp>
#include <mbgl/tile/raster_dem_tile.hpp>
#include <mbgl/algorithm/update_tile_masks.hpp>
#include <mbgl/geometry/dem_data.hpp>
#include <mbgl/renderer/buckets/hillshade_bucket.hpp>
#include <mbgl/renderer/tile_parameters.hpp>
#include <mbgl/util/tile_cover.hpp>
#include <mbgl/util/math.hpp>

namespace mbgl {

using namespace style;

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
    // Native tile cover historically culled tiles against a flat z=0 plane.
    // That under-selects pitched low-zoom terrain: raised mountains can be
    // visible even when the flat ground plane for the same tile is outside the
    // frustum, leaving the terrain as isolated relief islands in a beige
    // background. GL JS uses terrain-aware tile bounding volumes with DEM
    // min/max. Until Native has per-tile min/max here, use a conservative
    // Sweden-safe elevation envelope so cover errs on the side of drawing a
    // connecting mesh.
    demParameters.tileCoverMinElevationMeters = -6000.0;
    demParameters.tileCoverMaxElevationMeters = 8000.0;

    // Hard floor on the variable-zoom cover: never emit tiles more than 2
    // zoom levels below the camera's ideal zoom for this source. At ideal
    // zoom 12 that caps the worst-case emission at z=10 (4× lower texel
    // density than z=12) instead of going to z=8 (16× lower). When the
    // user pans, the camera-side parent-fallback chain still reaches into
    // cached coarser tiles, but with the cap they never look more than
    // mildly soft — the dramatic blurry pop from a z=8 stand-in is gone.
    //
    // The floor is computed against the camera's ideal zoom (via
    // `coveringZoomLevel`, mirroring `TilePyramid::update`'s own
    // calculation) rather than the per-source maxZoom so it scales with
    // how the user is looking at the map: zoomed in tight (z=14 ideal)
    // caps at z=12; pulled out (z=10 ideal) caps at z=8 — but never below
    // the DEM tileset's own minzoom. Emitting below the source range creates
    // empty DEM/drape placeholders that look like beige terrain holes while
    // the main basemap is suppressed by terrain coverage.
    const double demZoom = util::clamp<double>(
        parameters.transformState.getZoom() + parameters.tileLodZoomShift,
        parameters.transformState.getMinZoom(),
        parameters.transformState.getMaxZoom());
    const int32_t demIdealZoom = util::coveringZoomLevel(demZoom, SourceType::RasterDEM, impl().getTileSize());
    demParameters.tileLodMinZoom = static_cast<uint8_t>(
        std::max<int32_t>(tileset.zoomRange.min, demIdealZoom - 2));

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
