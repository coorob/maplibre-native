#pragma once

#include <mbgl/map/mode.hpp>
#include <mbgl/actor/scheduler.hpp>
#include <mbgl/style/types.hpp>

#include <memory>
#include <numbers>
#include <cstddef>

#include <mapbox/std/weak.hpp>

namespace mbgl {

namespace tile_policy {

// Recomputed for every source update. The active DEM always feeds terrain;
// Raster sources do so only while a visible layer is being rendered into an
// active terrain drape.
constexpr bool usedByTerrain(const style::SourceType sourceType,
                             const bool terrainEnabled,
                             const bool isActiveTerrainSource,
                             const bool needsRendering) noexcept {
    return terrainEnabled && (isActiveTerrainSource || (sourceType == style::SourceType::Raster && needsRendering));
}

// The enlarged Raster cache exists solely as a terrain-drape gap-fill
// reservoir. Flat raster sources use the ordinary cache budget.
constexpr std::size_t rasterCacheScale(const bool usedByTerrain, const std::size_t terrainScale) noexcept {
    return usedByTerrain ? terrainScale : 1;
}

} // namespace tile_policy

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
    // Conservative vertical tile-cover range for non-flat sources such as
    // raster-dem terrain. Values are metres and default to the flat z=0 plane.
    double tileCoverMinElevationMeters = 0.0;
    double tileCoverMaxElevationMeters = 0.0;
    // Optional cap on visible source tiles. 0 preserves the full cover.
    std::size_t tileCoverMaxTiles = 0;
    // Set per source by RenderOrchestrator when this source is the active
    // terrain DEM or a rendered Raster source routed into the terrain drape.
    // Raster and RasterDEM sources can also be used by flat visual layers;
    // those layers must not inherit terrain-only cover and cache policy.
    bool usedByTerrain = false;
    gfx::DynamicTextureAtlasPtr dynamicTextureAtlas;
    bool isUpdateSynchronous = false;
};

} // namespace mbgl
