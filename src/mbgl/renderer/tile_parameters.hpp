#pragma once

#include <mbgl/map/mode.hpp>
#include <mbgl/actor/scheduler.hpp>

#include <memory>
#include <numbers>
#include <cstddef>

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
    // Conservative vertical tile-cover range for non-flat sources such as
    // raster-dem terrain. Values are metres and default to the flat z=0 plane.
    double tileCoverMinElevationMeters = 0.0;
    double tileCoverMaxElevationMeters = 0.0;
    // Optional cap on visible source tiles. 0 preserves the full cover.
    std::size_t tileCoverMaxTiles = 0;
    gfx::DynamicTextureAtlasPtr dynamicTextureAtlas;
    bool isUpdateSynchronous = false;
};

} // namespace mbgl
