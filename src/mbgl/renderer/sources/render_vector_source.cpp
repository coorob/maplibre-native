#include <mbgl/renderer/sources/render_vector_source.hpp>

#include <mbgl/renderer/render_tile.hpp>
#include <mbgl/renderer/sources/klattra_terrain_cover.hpp>
#include <mbgl/renderer/topo_vector_cohort.hpp>
#include <mbgl/renderer/topo_drape_detail_policy.hpp>
#include <mbgl/renderer/paint_parameters.hpp>
#include <mbgl/renderer/tile_parameters.hpp>
#include <mbgl/tile/vector_mlt_tile.hpp>
#include <mbgl/tile/vector_mvt_tile.hpp>
#include <mbgl/util/math.hpp>
#include <mbgl/util/tile_cover.hpp>

#include <algorithm>
#include <cstdlib>
#include <numbers>

namespace mbgl {

using namespace style;

namespace {

std::size_t klattraTerrainTopoVectorTileCap(const Size& viewport) noexcept {
    // The DEM cap bounds mesh demand. The vector writer has to cover every
    // resulting visible drape leaf, including masked descendants, so sharing
    // the compact 112 DEM ceiling can leave a 144-leaf mesh with only ~110
    // publishable Topo surfaces. Match the renderer's stable drape budget on
    // phones while preserving the wider tablet budget.
#if MLN_RENDER_BACKEND_OPENGL
    // Android uses the exact z10 terrain cohort below once it is available.
    // Bound only the first-frame generic fallback to the OpenGL stable target
    // population; tablets retain their larger viewport-derived cover.
    const std::size_t fallback = std::max<std::size_t>(160, klattraTerrainRenderTileCap(viewport));
#else
    const std::size_t fallback = std::max<std::size_t>(240, klattraTerrainRenderTileCap(viewport));
#endif
    const char* value = std::getenv("KLATTRA_TOPO_VECTOR_MAX_RENDER_TILES");
    if (!value || !*value) return fallback;

    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value) return fallback;
    return static_cast<std::size_t>(std::clamp<unsigned long>(parsed, 0, 512));
}

} // namespace

RenderVectorSource::RenderVectorSource(Immutable<style::TileSource::Impl> impl_, const TaggedScheduler& threadPool_)
    : RenderTileSetSource(std::move(impl_), threadPool_) {}

const std::optional<Tileset>& RenderVectorSource::getTileset() const {
    return static_cast<const style::TileSource::Impl&>(*baseImpl).tileset;
}

void RenderVectorSource::updateInternal(const Tileset& tileset,
                                        const std::vector<Immutable<style::LayerProperties>>& layers,
                                        const bool needsRendering,
                                        const bool needsRelayout,
                                        const TileParameters& parameters) {
    TileParameters vectorParameters = parameters;
    if (parameters.usedByTerrain) {
        // The terrain mesh, raster imagery, and official Topo vectors must
        // cover the same elevated pitched frustum. Stock vector cover is
        // flat-z=0 and only enters variable LOD above 60 degrees; at the
        // product's 58-degree flyover pitch it selected three source tiles
        // for 197 terrain targets, leaving large target-shaped regions with
        // only the background colour.
        double pitchGateDegrees = 40.0;
        if (const char* value = std::getenv("KLATTRA_TERRAIN_LOD_PITCH_DEG"); value && *value) {
            char* end = nullptr;
            const double parsed = std::strtod(value, &end);
            if (end != value) pitchGateDegrees = std::clamp(parsed, 20.0, 60.0);
        }
        vectorParameters.tileLodPitchThreshold = pitchGateDegrees * std::numbers::pi / 180.0;
        vectorParameters.tileLodMinRadius = 2.0;
        vectorParameters.tileLodScale = 1.0;
        vectorParameters.tileCoverMinElevationMeters = -6000.0;
        vectorParameters.tileCoverMaxElevationMeters = 8000.0;
        vectorParameters.tileCoverMaxTiles =
            klattraTerrainTopoVectorTileCap(parameters.transformState.getSize());

        // Swedish T50 detailed land cover begins at z10; Norwegian contours
        // and detailed N50 surface data do too. Coarser parents are valid 2D
        // overview tiles, but cannot stand in for a 3D terrain canvas because
        // they legitimately omit the contours, streams, and detailed fills
        // that make Topo readable. The cover helper clamps this floor to the
        // current ideal zoom for cameras below z10.
        if (topo_vector::isOfficialSourceID(baseImpl->id)) {
            constexpr uint8_t detailedTopoMinZoom = 10;
            const uint8_t terrainTopoFloor = std::clamp<uint8_t>(
                detailedTopoMinZoom, tileset.zoomRange.min, tileset.zoomRange.max);
            vectorParameters.tileLodMinZoom = terrainTopoFloor;
            vectorParameters.tileFallbackMinZoom = terrainTopoFloor;
        }
#if MLN_RENDER_BACKEND_OPENGL
        // An official vector tile can paint every finer drape target inside
        // it. Make the previous terrain-leaf union authoritative instead of
        // decoding the generic 240-tile elevated cover plus exact finer gaps.
        // Source updates precede terrain, so an empty first frame deliberately
        // falls back to the bounded generic cover above.
        if (topo_vector::isOfficialSourceID(baseImpl->id)) {
            // Match the terrain's diagnostic detail policy. The generic
            // first-frame floor stays at z10, but must not coarsen exact z12
            // source targets once the terrain publishes its authoritative IDs.
            vectorParameters.terrainCoverCohortZoom = std::clamp<uint8_t>(
                topo_vector::drapeZoomPolicy().detailZoom, tileset.zoomRange.min, tileset.zoomRange.max);
            vectorParameters.boundCoverHoldToIdealCohort = true;
        }
#if defined(__ANDROID__)
        else if (topo_vector::isAuthoritativeFillSourceID(baseImpl->id, true)) {
            // Finland's complete base has useful overview tiles down to z0.
            // Request each natural drape target at its own zoom (clamped only
            // by the source maximum), without the Swedish/Norwegian z10 floor
            // or shared-canvas policy. Making this exact source cover primary
            // prevents a full generic cover from consuming the gap-fill budget.
            vectorParameters.terrainCoverCohortZoom = tileset.zoomRange.max;
            vectorParameters.boundCoverHoldToIdealCohort = true;
        }
#endif
#endif
    }

    tilePyramid.update(
        layers,
        needsRendering,
        needsRelayout,
        vectorParameters,
        *baseImpl,
        util::tileSize_I,
        tileset.zoomRange,
        tileset.bounds,
        [&](const OverscaledTileID& tileID, TileObserver* observer_) -> std::unique_ptr<VectorTile> {
            if (!isMLT.has_value()) {
                auto impl = staticImmutableCast<style::TileSource::Impl>(baseImpl);
                assert(impl->tileset); // we should have one by now
                if (impl->tileset) {
                    isMLT = (impl->tileset->vectorEncoding == Tileset::VectorEncoding::MLT);
                }
            }
            if (isMLT && *isMLT) {
                return std::make_unique<VectorMLTTile>(tileID, baseImpl->id, parameters, tileset, observer_);
            } else {
                return std::make_unique<VectorMVTTile>(tileID, baseImpl->id, parameters, tileset, observer_);
            }
        });
}

} // namespace mbgl
