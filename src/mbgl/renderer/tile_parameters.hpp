#pragma once

#include <mbgl/map/mode.hpp>
#include <mbgl/actor/scheduler.hpp>
#include <mbgl/tile/tile_id.hpp>

#include <memory>
#include <numbers>
#include <cstddef>
#include <optional>
#include <unordered_set>

#include <mapbox/std/weak.hpp>

namespace mbgl {

class TransformState;
class FileSource;
class AnnotationManager;
class ImageManager;
class GlyphManager;

namespace gfx {
class DynamicTextureAtlas;
using DynamicTextureAtlasPtr = std::shared_ptr<gfx::DynamicTextureAtlas>;
} // namespace gfx

class TileParameters {
public:
    const float pixelRatio;
    const MapDebugOptions debugOptions;
    const TransformState& transformState;
    std::shared_ptr<FileSource> fileSource;
    const MapMode mode;
    mapbox::base::WeakPtr<AnnotationManager> annotationManager;
    std::shared_ptr<ImageManager> imageManager;
    std::shared_ptr<GlyphManager> glyphManager;
    // Not const so per-source overrides (e.g. `RenderRasterDEMSource`
    // capping DEM prefetch at delta=2 to avoid 16× resolution drops when
    // tiles are streaming) can copy-and-edit `TileParameters` before
    // passing to `TilePyramid::update`.
    uint8_t prefetchZoomDelta;
    TaggedScheduler threadPool;
    double tileLodMinRadius = 3;
    double tileLodScale = 1;
    double tileLodPitchThreshold = (60.0 / 180.0) * std::numbers::pi;
    double tileLodZoomShift = 0;
    TileLodMode tileLodMode = TileLodMode::Default;
    // Floor on variable-zoom emission. See `TileCoverParameters::tileLodMinZoom`.
    uint8_t tileLodMinZoom = 0;
    // Optional floor for parent tiles rendered while an ideal tile is still
    // loading. Unlike `tileLodMinZoom`, this constrains
    // `algorithm::updateRenderables` rather than the ideal cover itself.
    // A value above the current ideal zoom is clamped back to that ideal so
    // low-zoom cameras retain their normal source coverage.
    uint8_t tileFallbackMinZoom = 0;
    // Conservative vertical tile-cover range for non-flat sources such as
    // raster-dem terrain. Values are metres and default to the flat z=0 plane.
    double tileCoverMinElevationMeters = 0.0;
    double tileCoverMaxElevationMeters = 0.0;
    // Optional cap on visible source tiles. 0 preserves the full cover.
    std::size_t tileCoverMaxTiles = 0;
    // Set per source by RenderOrchestrator when this source contributes to
    // the active terrain surface. The active RasterDEM and the official Topo
    // vector writers use this; flat-only hillshade/color-relief sources do
    // not inherit the terrain mesh's elevation-expanded cover.
    bool usedByTerrain = false;
    // Previous-frame exact terrain mesh cover. Source updates run before the
    // terrain layer rebuilds its current cover, so official Topo vector
    // sources use this one-frame-lagged set to request any mesh slots that
    // their independent camera cover omitted. Null keeps ordinary sources
    // unchanged.
    const std::unordered_set<OverscaledTileID>* terrainCoverTileIDs = nullptr;
    // Optional source zoom at which the previous terrain-leaf union becomes
    // the authoritative ideal cohort. Used by the Android official Topo
    // vectors to keep z10 detail without decoding one vector tile per finer
    // terrain leaf. Null preserves the generic elevation-expanded cover.
    std::optional<uint8_t> terrainCoverCohortZoom;
    // Bound the custom previous-cover continuity state to one current ideal
    // cohort. Ordinary holdForFade tiles remain outside this budget.
    bool boundCoverHoldToIdealCohort = false;
    // Set by RenderOrchestrator while 3D terrain is enabled for the map.
    bool terrainActive = false;
    gfx::DynamicTextureAtlasPtr dynamicTextureAtlas;
    bool isUpdateSynchronous = false;
};

} // namespace mbgl
