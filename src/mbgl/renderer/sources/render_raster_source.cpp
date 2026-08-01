#include <mbgl/renderer/sources/render_raster_source.hpp>
#include <mbgl/renderer/sources/klattra_terrain_cover.hpp>
#include <mbgl/renderer/render_tile.hpp>
#include <mbgl/tile/raster_tile.hpp>
#include <mbgl/algorithm/update_tile_masks.hpp>
#include <mbgl/renderer/tile_parameters.hpp>

#include <mbgl/util/tile_cover.hpp>
#include <mbgl/util/math.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>
#include <unordered_map>

namespace mbgl {

using namespace style;

RenderRasterSource::RenderRasterSource(Immutable<style::TileSource::Impl> impl_, const TaggedScheduler& threadPool_)
    : RenderTileSetSource(std::move(impl_), threadPool_) {}

inline const style::TileSource::Impl& RenderRasterSource::impl() const {
    return static_cast<const style::TileSource::Impl&>(*baseImpl);
}

const std::optional<Tileset>& RenderRasterSource::getTileset() const {
    return impl().tileset;
}

TileParameters terrainAwareRasterTileParameters(const TileParameters& parameters,
                                                const Tileset& tileset,
                                                const uint16_t tileSize) {
    TileParameters rasterParameters = parameters;
    if (!parameters.usedByTerrain) {
        return rasterParameters;
    }

    const char* v = std::getenv("KLATTRA_TERRAIN_LOD_PITCH_DEG");
    double deg = 40.0;
    if (v && *v) {
        char* end = nullptr;
        const double parsed = std::strtod(v, &end);
        if (end != v) deg = std::clamp(parsed, 20.0, 60.0);
    }
    rasterParameters.tileLodPitchThreshold = deg * M_PI / 180.0;

    rasterParameters.tileLodMinRadius = 2.0;
    rasterParameters.tileLodScale = 1.0;
    rasterParameters.tileCoverMinElevationMeters = -6000.0;
    rasterParameters.tileCoverMaxElevationMeters = 8000.0;

    // Match the DEM cover exactly: any mesh emitted beyond the imagery
    // budget would expose the style background through that terrain.
    rasterParameters.tileCoverMaxTiles = klattraTerrainRenderTileCap(parameters.transformState.getSize());

    const double rasterZoom = util::clamp<double>(parameters.transformState.getZoom() + parameters.tileLodZoomShift,
                                                  parameters.transformState.getMinZoom(),
                                                  parameters.transformState.getMaxZoom());
    const int32_t idealZoom = util::coveringZoomLevel(rasterZoom, SourceType::Raster, tileSize);
    const int32_t coverZoom = std::clamp<int32_t>(
        idealZoom, static_cast<int32_t>(tileset.zoomRange.min), static_cast<int32_t>(tileset.zoomRange.max));
    // .65: floor at coverZoom-3, one step below the DEM's -2. The -2
    // mirror was blind to the sources' asymmetry: this source's coverZoom
    // is deeper than the DEM's (256px tiles and maxzoom 15 vs 512px and
    // maxzoom 12), so at flyover the raster floor sat at z11 while the
    // DEM/mesh cover flooring at z10 reached 2-4x farther on the same
    // ranked tile budget. Bound far meshes (z8 canvases in the phone-63
    // census) rendered relief in style-background green with the raster
    // pyramid holding nothing to route or gap-fill from. Floor z10 at
    // flyover matches the DEM's geographic reach per tile, so the imagery
    // cover blankets everything the mesh cover emits — same tile cap,
    // coarser far rows, no extra memory.
    rasterParameters.tileLodMinZoom = static_cast<uint8_t>(std::max<int32_t>(tileset.zoomRange.min, coverZoom - 3));

    return rasterParameters;
}

void RenderRasterSource::updateInternal(const Tileset& tileset,
                                        const std::vector<Immutable<LayerProperties>>& layers,
                                        const bool needsRendering,
                                        const bool needsRelayout,
                                        const TileParameters& parameters) {
    // When this source is routed into terrain, mirror the DEM source's
    // variable-zoom cover tuning (see the long rationale blocks in
    // render_raster_dem_source.cpp) so pitched views serve the far field with
    // a few coarse parent tiles. Flat raster layers render on the z=0 plane
    // and must keep the ordinary cover; applying the terrain elevation halo
    // there inflated a phone's 2D raster cover to roughly 90-112 tiles.
    //
    // The 40° pitch
    // gate alone (6.27.0-traska.18) was NOT enough: with the stock
    // tileLodMinRadius=3/tileLodScale=1 the cover never actually emits
    // any lower-zoom far-field tiles — the exact lesson the DEM source
    // documents — so terrain drape targets beyond the near field had no
    // raster tile to route imagery from and baked base-colour only
    // ("beige wedges"; 2026-07-04 traska.19 probe: drape routing added
    // 124/124 overlap pairs with zero skips while the satellite pyramid
    // held only ~31 near-field tiles, 75/112 targets content-less).
    // Mirrored pieces, all load-bearing:
    //  - tileLodMinRadius/Scale: actually emit coarse rows past the near
    //    full-zoom radius once pitch exceeds the gate.
    //  - elevation envelope: keep raised-but-flat-culled far tiles in
    //    cover (without it the far cover self-truncates over relief).
    //  - tileCoverMaxTiles: bound the emission; the cut is ranked by
    //    zoom-normalized distance so it always drops the farthest first.
    //  - tileLodMinZoom floor measured from the CLAMPED cover zoom:
    //    never coarsen more than 2 levels below what is actually
    //    rendered, and never below the tileset minzoom.
    // Net effect at flyover pitches: near field stays at ideal zoom,
    // far field arrives as z(ideal-1..2) parents instead of either a
    // full-frustum ideal-zoom firehose or nothing — a tile-count and
    // disk-write win on top of the drape fix. Same env knobs as the DEM
    // source (KLATTRA_TERRAIN_LOD_PITCH_DEG, KLATTRA_TERRAIN_MAX_RENDER_TILES).
    const TileParameters rasterParameters = terrainAwareRasterTileParameters(parameters, tileset, impl().getTileSize());
    tilePyramid.update(layers,
                       needsRendering,
                       needsRelayout,
                       rasterParameters,
                       *baseImpl,
                       impl().getTileSize(),
                       tileset.zoomRange,
                       tileset.bounds,
                       [&](const OverscaledTileID& tileID, TileObserver* observer_) {
                           return std::make_unique<RasterTile>(tileID, baseImpl->id, parameters, tileset, observer_);
                       });
    algorithm::updateTileMasks(tilePyramid.getRenderedTiles());

    // Probe: 1 Hz rendered-pyramid histogram for the beige-wedge hunt — is
    // there ANY coarse far-field imagery tile for drape routing to pick up?
    // stderr because the simulator swallows mbgl Log::Warning.
    if (std::getenv("KLATTRA_TRACE_STDERR") != nullptr) {
        // Throttle PER SOURCE: a single shared timestamp let the first
        // source updated each frame (osmSource) claim the 1 Hz slot and
        // permanently starve the others' lines (traska.19 finding).
        static std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastLogBySource;
        auto& lastLog = lastLogBySource[baseImpl->id];
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
            std::string hist;
            for (std::size_t z = 0; z < byZ.size(); ++z) {
                if (!byZ[z]) continue;
                if (!hist.empty()) hist += ' ';
                hist += 'z' + std::to_string(z) + ':' + std::to_string(byZ[z]);
            }
            fprintf(stderr,
                    "[KLATTRA_TRACE] [KLATTRA RASTER] source=%s rendered=%zu byZ=%s zoom=%.2f pitchDeg=%.1f\n",
                    baseImpl->id.c_str(),
                    rendered,
                    hist.empty() ? "-" : hist.c_str(),
                    parameters.transformState.getZoom(),
                    parameters.transformState.getPitch() * 180.0 / M_PI);
        }
    }
}

void RenderRasterSource::prepare(const SourcePrepareParameters& parameters) {
    RenderTileSource::prepare(parameters);
}

void RenderRasterSource::visitRasterTileBuckets(
    const std::function<void(const OverscaledTileID&, RasterBucket&)>& fn) {
    tilePyramid.visitAllTiles([&](const OverscaledTileID& tileID, Tile& tile) {
        // This pyramid only ever holds RasterTiles (see createTile above).
        if (auto* bucket = static_cast<RasterTile&>(tile).getParsedBucket()) {
            fn(tileID, *bucket);
        }
    });
}

std::unordered_map<std::string, std::vector<Feature>> RenderRasterSource::queryRenderedFeatures(
    const ScreenLineString&,
    const TransformState&,
    const std::unordered_map<std::string, const RenderLayer*>&,
    const RenderedQueryOptions&,
    const mat4&) const {
    return std::unordered_map<std::string, std::vector<Feature>>{};
}

std::vector<Feature> RenderRasterSource::querySourceFeatures(const SourceQueryOptions&) const {
    return {};
}

} // namespace mbgl
