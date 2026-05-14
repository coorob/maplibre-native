#pragma once

#include <mbgl/renderer/layer_tweaker.hpp>
#include <mbgl/tile/tile_id.hpp>

#include <optional>
#include <string>

namespace mbgl {

/**
    Fill layer specific tweaker.

    When `drapeTargetID` is set, this tweaker is being used for the terrain
    drape pass: instead of the camera's `getTileMatrix`, each drawable gets
    a `getDrapeMatrix(drawable.tileID, drapeTargetID)` matrix that projects
    source-tile geometry into the drape RenderTarget's coordinate space.
 */
class FillLayerTweaker : public LayerTweaker {
public:
    FillLayerTweaker(std::string id_,
                     Immutable<style::LayerProperties> properties,
                     std::optional<OverscaledTileID> drapeTargetID_ = std::nullopt)
        : LayerTweaker(std::move(id_), properties),
          drapeTargetID(drapeTargetID_) {}

public:
    ~FillLayerTweaker() override = default;

    void execute(LayerGroupBase&, const PaintParameters&) override;

private:
    gfx::UniformBufferPtr evaluatedPropsUniformBuffer;

#if MLN_UBO_CONSOLIDATION
    gfx::UniformBufferPtr drawableUniformBuffer;
    gfx::UniformBufferPtr tilePropsUniformBuffer;
#endif

    std::optional<OverscaledTileID> drapeTargetID;
};

} // namespace mbgl
