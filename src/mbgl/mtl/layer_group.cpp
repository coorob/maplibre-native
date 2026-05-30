#include <mbgl/mtl/layer_group.hpp>

#include <mbgl/gfx/drawable_tweaker.hpp>
#include <mbgl/gfx/renderable.hpp>
#include <mbgl/gfx/renderer_backend.hpp>
#include <mbgl/gfx/upload_pass.hpp>
#include <mbgl/mtl/context.hpp>
#include <mbgl/mtl/drawable.hpp>
#include <mbgl/mtl/render_pass.hpp>
#include <mbgl/renderer/paint_parameters.hpp>
#include <mbgl/shaders/mtl/shader_program.hpp>
#include <mbgl/util/convert.hpp>

namespace mbgl {
namespace mtl {

LayerGroup::LayerGroup(int32_t layerIndex_, std::size_t initialCapacity, std::string name_)
    : mbgl::LayerGroup(layerIndex_, initialCapacity, std::move(name_)) {}

void LayerGroup::upload(gfx::UploadPass& uploadPass) {
    if (!enabled) {
        return;
    }

#if !defined(NDEBUG)
    const auto debugGroup = uploadPass.createDebugGroup(getName() + "-upload");
#endif

    visitDrawables([&](gfx::Drawable& drawable) {
        if (drawable.getEnabled()) {
            auto& drawableMTL = static_cast<mtl::Drawable&>(drawable);
            drawableMTL.upload(uploadPass);
        }
    });
}

void LayerGroup::render(RenderOrchestrator&, PaintParameters& parameters) {
    if (!enabled || !getDrawableCount() || !parameters.renderPass) {
        return;
    }

#if !defined(NDEBUG)
    const auto debugGroup = parameters.encoder->createDebugGroup(getName() + "-render");
#endif

    auto& context = static_cast<Context&>(parameters.context);
    auto& renderPass = static_cast<RenderPass&>(*parameters.renderPass);
    const auto& encoder = renderPass.getMetalEncoder();
    const auto& renderable = renderPass.getDescriptor().renderable;

    bool features3d = false;
    bool stencil3d = false;
    visitDrawables([&](const gfx::Drawable& drawable) {
        if (drawable.getEnabled() && drawable.getIs3D() && drawable.hasRenderPass(parameters.pass)) {
            features3d = true;
            if (drawable.getEnableStencil()) {
                stencil3d = true;
            }
        }
    });

    gfx::StencilMode stencilMode3d;
    std::optional<MTLDepthStencilStatePtr> stateStencil;
    std::optional<MTLDepthStencilStatePtr> stateDepthStencil;
    const auto getDepthStencilState = [&](bool depth, bool stencil) -> const MTLDepthStencilStatePtr& {
        if (depth) {
            const auto depthMode = parameters.depthModeFor3D();
            if (stencil) {
                if (!stateDepthStencil.has_value()) {
                    stateDepthStencil = context.makeDepthStencilState(depthMode, stencilMode3d, renderable);
                }
                return *stateDepthStencil;
            }
            if (!stateDepth) {
                stateDepth = context.makeDepthStencilState(depthMode, gfx::StencilMode::disabled(), renderable);
            }
            return *stateDepth;
        }

        if (stencil) {
            if (!stateStencil.has_value()) {
                stateStencil = context.makeDepthStencilState(
                    gfx::DepthMode::disabled(), stencilMode3d, renderable);
            }
            return *stateStencil;
        }

        if (!stateNone) {
            stateNone = context.makeDepthStencilState(
                gfx::DepthMode::disabled(), gfx::StencilMode::disabled(), renderable);
        }
        return *stateNone;
    };

    if (features3d && stencil3d) {
        stencilMode3d = parameters.stencilModeFor3D();
        if (encoder) {
            encoder->setStencilReferenceValue(stencilMode3d.ref);
        }
    }

    bool bindUBOs = false;
    visitDrawables([&](gfx::Drawable& drawable) {
        if (!drawable.getEnabled() || !drawable.hasRenderPass(parameters.pass)) {
            return;
        }

        if (!bindUBOs) {
            uniformBuffers.bindMtl(renderPass);
            bindUBOs = true;
        }

        for (const auto& tweaker : drawable.getTweakers()) {
            tweaker->execute(drawable, parameters);
        }

        if (features3d && drawable.getIs3D()) {
            const auto state = getDepthStencilState(drawable.getEnableDepth(), drawable.getEnableStencil());
            renderPass.setDepthStencilState(state);
        }

        drawable.draw(parameters);
    });
}

} // namespace mtl
} // namespace mbgl
