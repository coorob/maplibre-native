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
#include <mbgl/util/logging.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

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

namespace {
// KLATTRA diagnostics (2D black-flash hunt) — own copy per file, see
// tile_layer_group.cpp for the rationale. Single-drawable groups
// (background!) matter most here: Robert's flash screenshot lost exactly
// the background + fill stack while lines/symbols kept drawing.
void klattraDiagGroupPresence(
    const void* group, const std::string& name, uint64_t frame, std::size_t drawableCount, bool groupEnabled) {
    static const bool trace = std::getenv("KLATTRA_TRACE_STDERR") != nullptr;
    if (!trace || frame == 0) return;
    struct State {
        uint64_t lastFrame = 0;
        std::size_t lastCount = 0;
    };
    static std::unordered_map<const void*, State> states;
    auto& st = states[group];
    if (st.lastFrame && frame != st.lastFrame) {
        if (frame > st.lastFrame + 1 && st.lastCount > 0) {
            fprintf(stderr,
                    "[KLATTRA_TRACE] [KLATTRA GROUPGAP] group=%s unvisitedFrames=%llu-%llu lastDrawables=%zu\n",
                    name.c_str(),
                    static_cast<unsigned long long>(st.lastFrame + 1),
                    static_cast<unsigned long long>(frame - 1),
                    st.lastCount);
        }
        if (st.lastCount > 0 && (drawableCount == 0 || !groupEnabled)) {
            fprintf(stderr,
                    "[KLATTRA_TRACE] [KLATTRA GROUPEMPTY] group=%s frame=%llu prevDrawables=%zu count=%zu enabled=%d\n",
                    name.c_str(),
                    static_cast<unsigned long long>(frame),
                    st.lastCount,
                    drawableCount,
                    groupEnabled ? 1 : 0);
        }
    }
    if (frame != st.lastFrame) {
        st.lastFrame = frame;
        st.lastCount = groupEnabled ? drawableCount : 0;
    }
}
// KLATTRA diagnostics (2D black-flash hunt): device-visible drawn-count
// tracking — own copy per file, see tile_layer_group.cpp for rationale.
void klattraDiagLandDraw(const void* group,
                         const std::string& name,
                         uint64_t frame,
                         int pass,
                         std::size_t drawn,
                         std::size_t skippedPass,
                         std::size_t skippedDisabled) {
    static const bool enabled = [] {
        const char* v = std::getenv("KLATTRA_LOG_LANDDRAW");
        return !(v && (*v == '0' || *v == 'f' || *v == 'F'));
    }();
    if (!enabled) return;
    const bool watched = name.find("land") != std::string::npos || name.find("Land") != std::string::npos ||
                         name.find("topoColorRelief") != std::string::npos ||
                         name.find("vatten") != std::string::npos || name.find("background") != std::string::npos;
    if (!watched) return;
    struct State {
        std::size_t lastDrawn = SIZE_MAX;
    };
    static std::unordered_map<const void*, std::array<State, 4>> states;
    const std::size_t slot = pass == 1 ? 0 : pass == 2 ? 1 : pass == 4 ? 2 : 3;
    auto& st = states[group][slot];
    if (st.lastDrawn != drawn) {
        Log::Warning(Event::Render,
                     "[KLATTRA LANDDRAW] group=" + name + " grp=" + std::to_string(reinterpret_cast<uintptr_t>(group)) +
                         " pass=" + std::to_string(pass) + " frame=" +
                         std::to_string(frame) + " drawn=" + std::to_string(drawn) + " skippedPass=" +
                         std::to_string(skippedPass) + " skippedDisabled=" + std::to_string(skippedDisabled) +
                         " prev=" + (st.lastDrawn == SIZE_MAX ? std::string("-") : std::to_string(st.lastDrawn)));
        static const bool traceStderr = std::getenv("KLATTRA_TRACE_STDERR") != nullptr;
        if (traceStderr) {
            fprintf(stderr,
                    "[KLATTRA_TRACE] [KLATTRA LANDDRAW] group=%s pass=%d frame=%llu drawn=%zu skippedPass=%zu "
                    "skippedDisabled=%zu\n",
                    name.c_str(),
                    pass,
                    static_cast<unsigned long long>(frame),
                    drawn,
                    skippedPass,
                    skippedDisabled);
        }
        st.lastDrawn = drawn;
    }
}
} // namespace

void LayerGroup::render(RenderOrchestrator&, PaintParameters& parameters) {
    klattraDiagGroupPresence(this,
                             getName(),
                             static_cast<Context&>(parameters.context).diagFrameIndex(),
                             getDrawableCount(),
                             enabled);
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
    std::size_t drawnCount = 0, skippedPass = 0, skippedDisabled = 0;
    visitDrawables([&](gfx::Drawable& drawable) {
        if (!drawable.getEnabled()) {
            ++skippedDisabled;
            return;
        }
        if (!drawable.hasRenderPass(parameters.pass)) {
            ++skippedPass;
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
        ++drawnCount;
    });

    // KLATTRA diagnostics (2D black-flash hunt, device-visible): see
    // tile_layer_group.cpp — the background group lives here, and it is the
    // bottom of the land stack that goes missing in the flash frame.
    klattraDiagLandDraw(this,
                        getName(),
                        static_cast<Context&>(parameters.context).diagFrameIndex(),
                        static_cast<int>(parameters.pass),
                        drawnCount,
                        skippedPass,
                        skippedDisabled);
}

} // namespace mtl
} // namespace mbgl
