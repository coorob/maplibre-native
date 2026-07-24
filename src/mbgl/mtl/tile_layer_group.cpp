#include <mbgl/mtl/tile_layer_group.hpp>

#include <mbgl/gfx/drawable_tweaker.hpp>
#include <mbgl/gfx/renderable.hpp>
#include <mbgl/gfx/renderer_backend.hpp>
#include <mbgl/gfx/upload_pass.hpp>
#include <mbgl/mtl/context.hpp>
#include <mbgl/mtl/drawable.hpp>
#include <mbgl/mtl/render_pass.hpp>
#include <mbgl/renderer/paint_parameters.hpp>
#include <mbgl/util/convert.hpp>
#include <mbgl/util/logging.hpp>

#include <Metal/Metal.hpp>

#include <mbgl/tile/tile_id.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>

namespace mbgl {
namespace mtl {

TileLayerGroup::TileLayerGroup(int32_t layerIndex_, std::size_t initialCapacity, std::string name_)
    : mbgl::TileLayerGroup(layerIndex_, initialCapacity, std::move(name_)) {}

TileLayerGroup::~TileLayerGroup() {
    // KLATTRA diagnostics (2D black-flash hunt): pairs with the create log
    // in Context::createTileLayerGroup — create-without-destroy imbalance
    // during pinch churn is the 3.3 GB jetsam's leak accounting.
    static const bool grouplife = [] {
        const char* v = std::getenv("KLATTRA_LOG_GROUPLIFE");
        return v && !(*v == '0' || *v == 'f' || *v == 'F');
    }();
    if (grouplife) {
        const auto& n = getName();
        const bool watched = n.find("land") != std::string::npos || n.find("topoColorRelief") != std::string::npos ||
                             n.find("vatten") != std::string::npos || n.find("background") != std::string::npos;
        if (watched) {
            Log::Warning(Event::Render,
                         "[KLATTRA GROUPLIFE] destroy name=" + n + " grp=" +
                             std::to_string(reinterpret_cast<uintptr_t>(this)));
        }
    }
}

void TileLayerGroup::upload(gfx::UploadPass& uploadPass) {
    if (!enabled || !getDrawableCount()) {
        return;
    }

#if !defined(NDEBUG)
    const auto debugGroup = uploadPass.createDebugGroup(getName() + "-upload");
#endif

    visitDrawables([&](gfx::Drawable& drawable) {
        if (drawable.getEnabled()) {
            auto& drawableMTL = static_cast<Drawable&>(drawable);
            drawableMTL.upload(uploadPass);
        }
    });
}

namespace {
void klattraSea3DTileClippingEmitOnce(std::size_t tileCount) {
    static const bool trace = [] {
        const char* v = std::getenv("KLATTRA_LOG_SEA_TILE_CLIP");
        return v && !(*v == '0' || *v == 'f' || *v == 'F');
    }();
    static bool emitted = false;
    if (trace && !emitted) {
        emitted = true;
        Log::Warning(Event::Render,
                     "[KLATTRA SEA-TILE-CLIP] kind=sea-3d-tile-clipped tiles=" +
                         std::to_string(tileCount));
    }
}

// KLATTRA diagnostics (2D black-flash hunt): log when a layer group that
// was rendering drawables goes empty or unvisited for a frame — the
// symptom-level signature of the one-frame black land flash (background +
// fill groups missing while lines/symbols still draw). First render() call
// of each frame records; GROUPGAP fires retroactively when a group was not
// visited at all for one or more frames.
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
} // namespace

namespace {
// KLATTRA diagnostics (2D black-flash hunt): device-visible drawn-count
// tracking for the land watchlist. Logs at Warning (passes the release
// filter and DEVICE syslog, unlike the sim-only stderr probes) whenever a
// watched group's drawn count changes for a pass. Water is on the list as
// the control — in the flash frame water draws while land fills do not.
void klattraDiagLandDraw(const void* group,
                         const std::string& name,
                         uint64_t frame,
                         int pass,
                         std::size_t drawn,
                         std::size_t skippedPass,
                         std::size_t skippedDisabled) {
    static const bool enabled = [] {
        const char* v = std::getenv("KLATTRA_LOG_LANDDRAW");
        return v && !(*v == '0' || *v == 'f' || *v == 'F');
    }();
    if (!enabled) return;
    const bool watched = name.find("land") != std::string::npos || name.find("Land") != std::string::npos ||
                         name.find("topoColorRelief") != std::string::npos ||
                         name.find("vatten") != std::string::npos || name.find("background") != std::string::npos;
    if (!watched) return;
    struct State {
        std::size_t lastDrawn = SIZE_MAX;
    };
    static std::unordered_map<const void*, std::array<State, 4>> states; // per render pass slot
    // RenderPass values are bit flags (Opaque=1, Translucent=2, Pass3D=4).
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

// KLATTRA diagnostics (2D transition flash, post-.33): the rendered TILE SET
// stays geometrically covered through the flash (COVERHOLD ≈ silent on .33)
// yet land drawables still vanish — so the gap is between renderedTiles and
// what actually paints. Log the DISTINCT tile IDs drawn by the watched land
// group whenever the set changes; diff against the SRCTILES DIP id list to
// name the rendered-but-unpainted tiles. Opt out: KLATTRA_LOG_TILEPAINT=0.
bool klattraTilePaintEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("KLATTRA_LOG_TILEPAINT");
        return v && !(*v == '0' || *v == 'f' || *v == 'F');
    }();
    return enabled;
}

void klattraDiagTilePaint(const void* group,
                          const std::string& name,
                          uint64_t frame,
                          int pass,
                          std::size_t drawn,
                          const std::vector<OverscaledTileID>& paintedIDs) {
    struct State {
        std::vector<OverscaledTileID> last;
        std::size_t maxSeen = 0;
        bool logged = false;
    };
    static std::unordered_map<const void*, std::array<State, 4>> states;
    const std::size_t slot = pass == 1 ? 0 : pass == 2 ? 1 : pass == 4 ? 2 : 3;
    auto& st = states[group][slot];
    st.maxSeen = std::max(st.maxSeen, paintedIDs.size());
    // Skip the single-drawable prepare/drape groups that share the layer name;
    // once a group has ever painted >1 tile, follow every change (including
    // the collapse to 1 or 0).
    if (st.maxSeen <= 1) return;
    if (st.logged && st.last == paintedIDs) return;
    st.last = paintedIDs;
    st.logged = true;
    std::string ids;
    for (const auto& id : paintedIDs) {
        if (!ids.empty()) ids += ' ';
        ids += std::to_string(id.canonical.z) + ":" + std::to_string(id.canonical.x) + "," +
               std::to_string(id.canonical.y);
    }
    Log::Warning(Event::Render,
                 "[KLATTRA TILEPAINT] group=" + name + " grp=" + std::to_string(reinterpret_cast<uintptr_t>(group)) +
                     " pass=" + std::to_string(pass) + " frame=" + std::to_string(frame) +
                     " tiles=" + std::to_string(paintedIDs.size()) + " drawn=" + std::to_string(drawn) +
                     " ids=" + ids);
    static const bool traceStderr = std::getenv("KLATTRA_TRACE_STDERR") != nullptr;
    if (traceStderr) {
        fprintf(stderr,
                "[KLATTRA_TRACE] [KLATTRA TILEPAINT] group=%s pass=%d frame=%llu tiles=%zu drawn=%zu ids=%s\n",
                name.c_str(),
                pass,
                static_cast<unsigned long long>(frame),
                paintedIDs.size(),
                drawn,
                ids.c_str());
    }
}
} // namespace

void TileLayerGroup::render(RenderOrchestrator&, PaintParameters& parameters) {
    klattraDiagGroupPresence(this,
                             getName(),
                             static_cast<Context&>(parameters.context).diagFrameIndex(),
                             getDrawableCount(),
                             enabled);
    if (!enabled || !getDrawableCount() || !parameters.renderPass) {
        return;
    }

    auto& context = static_cast<Context&>(parameters.context);
    auto& renderPass = static_cast<RenderPass&>(*parameters.renderPass);
    const auto& renderable = renderPass.getDescriptor().renderable;

    // `stencilModeFor3D` uses a different stencil mask value each time its called, so if the
    // drawables in this layer use 3D stencil mode, we need to set it up here so that all the
    // drawables end up using the same mode value.
    // 2D and 3D features in the same layer group is not supported.
    bool features3d = false;
    bool stencil3d = false;
    gfx::StencilMode stencilMode3d;

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
    const bool tileClipped3d = features3d && tileClippingFor3D;

#if !defined(NDEBUG)
    const auto debugGroupRender = parameters.encoder->createDebugGroup(getName() + "-render");
#endif

    // If we're doing 3D stenciling and have any features to draw, set up the single-value stencil mask.
    // If we're doing 2D stenciling and have any drawables with tile IDs, render each tile into the stencil buffer with
    // a different value.
    // We can keep the depth-based descriptors, but the stencil-based ones can change
    // every time, as a new value is assigned in each call to `stencilModeFor3D`.
    std::optional<MTLDepthStencilStatePtr> stateStencil, stateDepthStencil;
    std::optional<MTLDepthStencilStatePtr> stateTileClip, stateDepthTileClip;
    std::function<const MTLDepthStencilStatePtr&(bool, bool)> getDepthStencilState;
    if (features3d) {
        // If we're using group-wide states, build only the ones that actually get used
        getDepthStencilState = [&](bool depth, bool stencil) -> const MTLDepthStencilStatePtr& {
            if (depth) {
                // We assume this doesn't change over the lifetime of a layer group.
                const auto depthMode = parameters.depthModeFor3D();
                if (stencil) {
                    if (!stateDepthStencil.has_value()) {
                        stateDepthStencil = context.makeDepthStencilState(depthMode, stencilMode3d, renderable);
                    }
                    return *stateDepthStencil;
                } else {
                    if (!stateDepth) {
                        stateDepth = context.makeDepthStencilState(depthMode, gfx::StencilMode::disabled(), renderable);
                    }
                    return *stateDepth;
                }
            } else {
                if (stencil) {
                    if (!stateStencil.has_value()) {
                        stateStencil = context.makeDepthStencilState(
                            gfx::DepthMode::disabled(), stencilMode3d, renderable);
                    }
                    return *stateStencil;
                } else {
                    if (!stateNone) {
                        stateNone = context.makeDepthStencilState(
                            gfx::DepthMode::disabled(), gfx::StencilMode::disabled(), renderable);
                    }
                    return *stateNone;
                }
            }
        };

        if (tileClipped3d) {
            // The post-terrain sea layer may contain retained parent and child
            // vector tiles simultaneously. Preserve 3D depth testing, but
            // restore the ordinary tile masks so detailed children replace
            // generalized parent coastlines instead of painting their union.
            parameters.renderTileClippingMasksFor3D(
                stencilTiles,
                tileClippingFor3DVerticalOffset + tileClippingFor3DSurfaceHeight);
            klattraSea3DTileClippingEmitOnce(stencilTiles->size());
        } else if (stencil3d) {
            stencilMode3d = parameters.stencilModeFor3D();
            renderPass.setStencilReference(stencilMode3d.ref);
        }
    } else if (stencilTiles && !stencilTiles->empty()) {
        parameters.renderTileClippingMasks(stencilTiles);
    }

    bool bindUBOs = false;
    std::size_t drawnCount = 0, skippedPass = 0, skippedDisabled = 0;
    const bool tilePaintWatched = klattraTilePaintEnabled() &&
                                  (getName().find("topoColorRelief") != std::string::npos ||
                                   getName().find("land") != std::string::npos ||
                                   getName().find("Land") != std::string::npos ||
                                   tileClipped3d);
    std::vector<OverscaledTileID> paintedIDs;
    visitDrawables([&](gfx::Drawable& drawable) {
        if (!drawable.getEnabled()) {
            ++skippedDisabled;
            return;
        }
        if (!drawable.hasRenderPass(parameters.pass)) {
            ++skippedPass;
            return;
        }
        if (tilePaintWatched && drawable.getTileID() &&
            std::find(paintedIDs.begin(), paintedIDs.end(), *drawable.getTileID()) == paintedIDs.end()) {
            paintedIDs.push_back(*drawable.getTileID());
        }

        if (!bindUBOs) {
            uniformBuffers.bindMtl(renderPass);
            bindUBOs = true;
        }

        for (const auto& tweaker : drawable.getTweakers()) {
            tweaker->execute(drawable, parameters);
        }

        // For layer groups with 3D features, enable either the single-value
        // stencil mode for features with stencil enabled or disable stenciling.
        // 2D drawables will set their own stencil mode within `draw`.
        if (features3d) {
            if (tileClipped3d && drawable.getEnableStencil() && drawable.getTileID()) {
                const auto stencilMode =
                    parameters.stencilModeForClipping(drawable.getTileID()->toUnwrapped());
                auto& state = drawable.getEnableDepth() ? stateDepthTileClip : stateTileClip;
                if (!state) {
                    const auto depthMode = drawable.getEnableDepth()
                                               ? parameters.depthModeFor3D()
                                               : gfx::DepthMode::disabled();
                    state = context.makeDepthStencilState(depthMode, stencilMode, renderable);
                }
                renderPass.setDepthStencilState(*state);
                renderPass.setStencilReference(stencilMode.ref);
            } else {
                const auto& state =
                    getDepthStencilState(drawable.getEnableDepth(), drawable.getEnableStencil());
                renderPass.setDepthStencilState(state);
            }
        }

        drawable.draw(parameters);
        ++drawnCount;
    });

    // KLATTRA diagnostics (2D black-flash hunt, device-visible): the flash
    // frame draws lines/symbols from a tile while the SAME tile's land fill
    // layers contribute nothing. Log drawn-count changes for the land
    // watchlist (+ water as the control) at Warning so DEVICE syslog shows
    // them (stderr probes are sim-only). Opt out: KLATTRA_LOG_LANDDRAW=0.
    klattraDiagLandDraw(this,
                        getName(),
                        static_cast<Context&>(parameters.context).diagFrameIndex(),
                        static_cast<int>(parameters.pass),
                        drawnCount,
                        skippedPass,
                        skippedDisabled);

    if (tilePaintWatched) {
        std::sort(paintedIDs.begin(), paintedIDs.end());
        klattraDiagTilePaint(this,
                             getName(),
                             static_cast<Context&>(parameters.context).diagFrameIndex(),
                             static_cast<int>(parameters.pass),
                             drawnCount,
                             paintedIDs);
    }
}

} // namespace mtl
} // namespace mbgl
