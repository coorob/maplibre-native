#pragma once

#include <mbgl/renderer/layer_tweaker.hpp>

#include <memory>

namespace mbgl {

namespace gfx {
class ShaderProgramBase;
class UniformBuffer;

using ShaderProgramBasePtr = std::shared_ptr<ShaderProgramBase>;
using UniformBufferPtr = std::shared_ptr<UniformBuffer>;
} // namespace gfx

/**
    Background layer specific tweaker.

    When `drapeMode` is set (used for the terrain drape pass), the
    per-drawable matrix is computed as a fixed tile-local-to-NDC ortho
    projection rather than the camera's `getTileMatrix(...)`. This makes
    the drawable cover the entire drape RenderTarget's texture rather
    than appearing at its real on-screen tile position. Mirrors the
    pattern in `HillshadePrepareLayerTweaker`.
 */
class BackgroundLayerTweaker : public LayerTweaker {
public:
    BackgroundLayerTweaker(std::string id_,
                           Immutable<style::LayerProperties> properties,
                           bool drapeMode_ = false)
        : LayerTweaker(std::move(id_), properties),
          drapeMode(drapeMode_) {}

public:
    ~BackgroundLayerTweaker() override = default;

    void execute(LayerGroupBase&, const PaintParameters&) override;

protected:
    bool drapeMode = false;
#if MLN_UBO_CONSOLIDATION
    gfx::UniformBufferPtr drawableUniformBuffer;
#endif
};

} // namespace mbgl
