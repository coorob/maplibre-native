#pragma once

#include <mbgl/renderer/layer_tweaker.hpp>
#include <mbgl/tile/tile_id.hpp>

#include <optional>

namespace mbgl {

/**
    Hillshade layer specific tweaker
 */
class HillshadeLayerTweaker : public LayerTweaker {
public:
    HillshadeLayerTweaker(std::string id_,
                          Immutable<style::LayerProperties> properties,
                          std::optional<OverscaledTileID> drapeTargetID_ = std::nullopt)
        : LayerTweaker(std::move(id_), properties),
          drapeTargetID(drapeTargetID_) {}

public:
    ~HillshadeLayerTweaker() override = default;

    void execute(LayerGroupBase&, const PaintParameters&) override;

protected:
    gfx::UniformBufferPtr evaluatedPropsUniformBuffer;

#if MLN_UBO_CONSOLIDATION
    gfx::UniformBufferPtr drawableUniformBuffer;
    gfx::UniformBufferPtr tilePropsUniformBuffer;
#endif

    // Phase 2: drape mode — when set, the tile drawable's matrix swaps to
    // LayerTweaker::getDrapeMatrix(sourceID, drapeTargetID) so the prepared
    // hillshade quad renders into the terrain drape RenderTarget instead of
    // the camera. Mirrors RasterLayerTweaker's drape branch.
    std::optional<OverscaledTileID> drapeTargetID;
};

} // namespace mbgl
