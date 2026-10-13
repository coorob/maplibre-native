#include <mbgl/gl/layer_group_gl.hpp>

#include <mbgl/gfx/drawable_tweaker.hpp>
#include <mbgl/gfx/render_pass.hpp>
#include <mbgl/gfx/renderable.hpp>
#include <mbgl/gfx/renderer_backend.hpp>
#include <mbgl/gfx/upload_pass.hpp>
#include <mbgl/gl/drawable_gl.hpp>
#include <mbgl/renderer/paint_parameters.hpp>
#include <mbgl/shaders/gl/shader_program_gl.hpp>
#include <mbgl/util/convert.hpp>
#include <mbgl/util/instrumentation.hpp>
#include <mbgl/util/logging.hpp>

#include <unordered_map>

namespace mbgl {
namespace gl {

using namespace platform;

TileLayerGroupGL::TileLayerGroupGL(int32_t layerIndex_, std::size_t initialCapacity, std::string name_)
    : TileLayerGroup(layerIndex_, initialCapacity, std::move(name_)) {}

void TileLayerGroupGL::upload(gfx::UploadPass& uploadPass) {
    MLN_TRACE_FUNC();
    MLN_ZONE_STR(name);

    if (!enabled) {
        return;
    }

    visitDrawables([&](gfx::Drawable& drawable) {
        if (!drawable.getEnabled()) {
            return;
        }

        auto& drawableGL = static_cast<gl::DrawableGL&>(drawable);

#if !defined(NDEBUG)
        std::string label;
        if (const auto& tileID = drawable.getTileID()) {
            label = drawable.getName() + "/" + util::toString(*tileID);
        }
        const auto labelPtr = (label.empty() ? drawable.getName() : label).c_str();
        const auto debugGroup = uploadPass.createDebugGroup(labelPtr);
#endif

        drawableGL.upload(uploadPass);
    });
}

namespace {
// KLATTRA GLRELIEF (diagnostic build): which relief tiles each relief layer group
// actually paints, with vertex counts (4 = full quad, more = masked). Logged when a
// group's painted set changes. Overlap = drawn pairs where one tile contains another.
void klattraGlRelief(const void* group, const std::string& name, int pass,
                     const std::vector<std::pair<OverscaledTileID, std::size_t>>& drawn) {
    if (name.find("Relief") == std::string::npos && name.find("illshade") == std::string::npos) return;
    std::string sig;
    std::size_t overlaps = 0;
    for (std::size_t i = 0; i < drawn.size(); ++i) {
        const auto& a = drawn[i].first.canonical;
        sig += std::to_string(a.z) + ":" + std::to_string(a.x) + "," + std::to_string(a.y) + "/v" +
               std::to_string(drawn[i].second) + " ";
        for (std::size_t j = 0; j < drawn.size(); ++j) {
            const auto& b = drawn[j].first.canonical;
            if (i != j && a.z < b.z && b.isChildOf(a)) ++overlaps;
        }
    }
    static std::unordered_map<const void*, std::string> last;
    auto& prev = last[group];
    if (prev == sig) return;
    prev = sig;
    Log::Warning(Event::Render,
                 "[KLATTRA GLRELIEF] grp=" + std::to_string(reinterpret_cast<uintptr_t>(group)) + " name=" + name +
                     " pass=" + std::to_string(pass) + " n=" + std::to_string(drawn.size()) +
                     " overlaps=" + std::to_string(overlaps) + " tiles=" + sig);
}
} // namespace

void TileLayerGroupGL::render(RenderOrchestrator&, PaintParameters& parameters) {
    if (!enabled) {
        return;
    }

    MLN_TRACE_FUNC();

    auto& context = static_cast<gl::Context&>(parameters.context);

    // `stencilModeFor3D` uses a different stencil mask value each time its called, so if the
    // drawables in this layer use 3D stencil mode, we need to set it up here so that all the
    // drawables end up using the same mode value.
    // 2D and 3D features in the same layer group is not supported.
    bool features3d = false;
    bool stencil3d = false;
    gfx::StencilMode stencilMode3d;

    if (getDrawableCount()) {
        MLN_TRACE_ZONE(clip masks);
#if !defined(NDEBUG)
        const auto label_clip = getName() + (getName().empty() ? "" : "-") + "tile-clip-masks";
        const auto debugGroupClip = parameters.encoder->createDebugGroup(label_clip.c_str());
#endif

        // If we're using stencil clipping, we need to handle 3D features separately
        if (stencilTiles && !stencilTiles->empty()) {
            visitDrawables([&](const gfx::Drawable& drawable) {
                if (drawable.getEnabled() && drawable.getIs3D() && drawable.hasRenderPass(parameters.pass)) {
                    features3d = true;
                    if (drawable.getEnableStencil()) {
                        stencil3d = true;
                    }
                }
            });
        }

        // If we're doing 3D stenciling and have any features
        // to draw, set up the single-value stencil mask.
        // If we're doing 2D stenciling and have any drawables with tile IDs,
        // render each tile into the stencil buffer with a different value.
        if (features3d) {
            stencilMode3d = stencil3d ? parameters.stencilModeFor3D() : gfx::StencilMode::disabled();
        } else if (stencilTiles && !stencilTiles->empty()) {
            parameters.renderTileClippingMasks(stencilTiles);
        }
    }

#if !defined(NDEBUG)
    const auto label_render = getName() + (getName().empty() ? "" : "-") + "render";
    const auto debugGroupRender = parameters.encoder->createDebugGroup(label_render.c_str());
#endif

    std::vector<std::pair<OverscaledTileID, std::size_t>> klattraDrawn;
    bool bindUBOs = false;
    visitDrawables([&](gfx::Drawable& drawable) {
        if (!drawable.getEnabled() || !drawable.hasRenderPass(parameters.pass)) {
            return;
        }
        if (const auto& tileID = drawable.getTileID()) {
            klattraDrawn.emplace_back(*tileID, static_cast<gl::DrawableGL&>(drawable).diagVertexCount());
        }

#if !defined(NDEBUG)
        std::string label_tile;
        if (const auto& tileID = drawable.getTileID()) {
            label_tile = util::toString(drawable.getID().id()) + "/" + drawable.getName() + "/" +
                         util::toString(*tileID);
        }
        const auto labelPtr = (label_tile.empty() ? drawable.getName() : label_tile).c_str();
        const auto debugGroupTile = parameters.encoder->createDebugGroup(labelPtr);
#endif

        if (!bindUBOs) {
            uniformBuffers.bind();
            bindUBOs = true;
        }

        for (const auto& tweaker : drawable.getTweakers()) {
            tweaker->execute(drawable, parameters);
        }

        // For layer groups with 3D features, enable either the single-value
        // stencil mode for features with stencil enabled or disable stenciling.
        // 2D drawables will set their own stencil mode within `draw`.
        if (features3d) {
            context.setStencilMode(drawable.getEnableStencil() ? stencilMode3d : gfx::StencilMode::disabled());
        }

        drawable.draw(parameters);
    });

    if (bindUBOs) {
        uniformBuffers.unbind();
    }
    klattraGlRelief(this, getName(), static_cast<int>(parameters.pass), klattraDrawn);
}

LayerGroupGL::LayerGroupGL(int32_t layerIndex_, std::size_t initialCapacity, std::string name_)
    : LayerGroup(layerIndex_, initialCapacity, std::move(name_)) {}

void LayerGroupGL::upload(gfx::UploadPass& uploadPass) {
    if (!enabled) {
        return;
    }

    visitDrawables([&](gfx::Drawable& drawable) {
        if (!drawable.getEnabled()) {
            return;
        }

        auto& drawableGL = static_cast<gl::DrawableGL&>(drawable);

#if !defined(NDEBUG)
        const auto debugGroup = uploadPass.createDebugGroup(drawable.getName().c_str());
#endif

        drawableGL.upload(uploadPass);
    });
}

void LayerGroupGL::render(RenderOrchestrator&, PaintParameters& parameters) {
    if (!enabled) {
        return;
    }

    bool bindUBOs = false;
    visitDrawables([&](gfx::Drawable& drawable) {
        if (!drawable.getEnabled() || !drawable.hasRenderPass(parameters.pass)) {
            return;
        }

#if !defined(NDEBUG)
        const auto debugGroup = parameters.encoder->createDebugGroup(drawable.getName().c_str());
#endif
        if (!bindUBOs) {
            uniformBuffers.bind();
            bindUBOs = true;
        }

        for (const auto& tweaker : drawable.getTweakers()) {
            tweaker->execute(drawable, parameters);
        }

        drawable.draw(parameters);
    });

    if (bindUBOs) {
        uniformBuffers.unbind();
    }
}

} // namespace gl
} // namespace mbgl
