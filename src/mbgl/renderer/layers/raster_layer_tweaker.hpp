#pragma once

#include <mbgl/renderer/layer_tweaker.hpp>
#include <mbgl/tile/tile_id.hpp>

#include <optional>

namespace mbgl {

namespace gfx {
class UniformBuffer;
using UniformBufferPtr = std::shared_ptr<UniformBuffer>;
} // namespace gfx

/**
    Raster layer tweaker
 */
class RasterLayerTweaker : public LayerTweaker {
public:
    RasterLayerTweaker(std::string id_,
                       Immutable<style::LayerProperties> properties,
                       std::optional<OverscaledTileID> drapeTargetID_ = std::nullopt)
        : LayerTweaker(std::move(id_), properties),
          drapeTargetID(drapeTargetID_) {}

public:
    ~RasterLayerTweaker() override = default;

    void execute(LayerGroupBase&, const PaintParameters&) override;

protected:
    gfx::UniformBufferPtr evaluatedPropsUniformBuffer = nullptr;

#if MLN_UBO_CONSOLIDATION
    gfx::UniformBufferPtr drawableUniformBuffer;
#endif

    // Phase 2: drape mode — when set, the tile drawable's matrix swaps to
    // LayerTweaker::getDrapeMatrix(sourceID, drapeTargetID) and feeds into
    // the terrain drape RenderTarget instead of the camera.
    std::optional<OverscaledTileID> drapeTargetID;
};

} // namespace mbgl
