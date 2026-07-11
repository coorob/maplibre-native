#include <mbgl/renderer/renderer_impl.hpp>

#include <mbgl/geometry/line_atlas.hpp>
#include <mbgl/gfx/backend_scope.hpp>
#include <mbgl/gfx/context.hpp>
#include <mbgl/gfx/cull_face_mode.hpp>
#include <mbgl/gfx/render_pass.hpp>
#include <mbgl/gfx/renderer_backend.hpp>
#include <mbgl/gfx/renderable.hpp>
#include <mbgl/gfx/upload_pass.hpp>
#include <mbgl/renderer/paint_parameters.hpp>
#include <mbgl/renderer/pattern_atlas.hpp>
#include <mbgl/renderer/renderer_observer.hpp>
#include <mbgl/renderer/render_static_data.hpp>
#include <mbgl/renderer/render_tree.hpp>
#include <mbgl/renderer/update_parameters.hpp>
#include <mbgl/shaders/program_parameters.hpp>
#include <mbgl/util/convert.hpp>
#include <mbgl/util/string.hpp>
#include <mbgl/util/logging.hpp>
#include <mbgl/util/instrumentation.hpp>

#include <cstdio>
#include <cstdlib>

#include <mbgl/gfx/drawable_tweaker.hpp>
#include <mbgl/renderer/layer_tweaker.hpp>
#include <mbgl/renderer/render_target.hpp>
#include <mbgl/renderer/render_terrain.hpp>
#include <mbgl/renderer/layers/terrain_layer_tweaker.hpp>

#if MLN_RENDER_BACKEND_METAL
#include <mbgl/mtl/renderer_backend.hpp>
#include <Metal/MTLCaptureManager.hpp>
#include <Metal/MTLCaptureScope.hpp>
#include <Foundation/NSURL.hpp>
#include <cstdlib>
#include <optional>
/// Enable programmatic Metal frame captures for specific frame numbers.
/// Requries iOS 13
constexpr auto EnableMetalCapture = 0;
constexpr auto CaptureFrameStart = 0; // frames are 0-based
constexpr auto CaptureFrameCount = 1;
#elif MLN_RENDER_BACKEND_OPENGL
#include <mbgl/gl/defines.hpp>
#include <mbgl/gl/drawable_gl.hpp>
#endif // !MLN_RENDER_BACKEND_METAL

namespace mbgl {

using namespace style;

namespace {

RendererObserver& nullObserver() {
    static RendererObserver observer;
    return observer;
}

bool klattraDisablePrebakeGate() {
    static const bool disabled = std::getenv("KLATTRA_DISABLE_PREBAKE_GATE") != nullptr;
    return disabled;
}

// Warning for the device syslog AND the stderr trace path for the
// simulator, where mbgl Log::Warning never reaches the unified log.
void klattraPrebakeEmit(const std::string& message) {
    Log::Warning(Event::Render, message);
    static const bool stderrTrace = std::getenv("KLATTRA_TRACE_STDERR") != nullptr;
    if (stderrTrace) {
        std::fprintf(stderr, "[KLATTRA_TRACE] %s\n", message.c_str());
    }
}

double klattraPrebakeGateTimeoutMs() {
    static const double ms = [] {
        if (const char* v = std::getenv("KLATTRA_PREBAKE_GATE_TIMEOUT_MS")) {
            return std::strtod(v, nullptr);
        }
        return 2500.0;
    }();
    return ms;
}

} // namespace

Renderer::Impl::Impl(gfx::RendererBackend& backend_,
                     float pixelRatio_,
                     const std::optional<std::string>& localFontFamily_)
    : orchestrator(!backend_.contextIsShared(), backend_.getThreadPool(), localFontFamily_),
      backend(backend_),
      observer(&nullObserver()),
      pixelRatio(pixelRatio_) {}

Renderer::Impl::~Impl() {
    assert(gfx::BackendScope::exists());
};

void Renderer::Impl::onPreCompileShader(shaders::BuiltIn shaderID,
                                        gfx::Backend::Type type,
                                        const std::string& additionalDefines) {
    observer->onPreCompileShader(shaderID, type, additionalDefines);
}

void Renderer::Impl::onPostCompileShader(shaders::BuiltIn shaderID,
                                         gfx::Backend::Type type,
                                         const std::string& additionalDefines) {
    observer->onPostCompileShader(shaderID, type, additionalDefines);
}

void Renderer::Impl::onShaderCompileFailed(shaders::BuiltIn shaderID,
                                           gfx::Backend::Type type,
                                           const std::string& additionalDefines) {
    observer->onShaderCompileFailed(shaderID, type, additionalDefines);
}

void Renderer::Impl::onRenderError(std::exception_ptr error) {
    observer->onRenderError(error);
}

void Renderer::Impl::setObserver(RendererObserver* observer_) {
    observer = observer_ ? observer_ : &nullObserver();
}

void Renderer::Impl::render(const RenderTree& renderTree, const std::shared_ptr<UpdateParameters>& updateParameters) {
    MLN_TRACE_FUNC();
    auto& context = backend.getContext();
    context.setObserver(this);

    assert(updateParameters);

#if MLN_RENDER_BACKEND_METAL
#if MLN_CREATE_AUTORELEASEPOOL
    NS::SharedPtr pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
#endif

    if constexpr (EnableMetalCapture) {
        const auto& mtlBackend = static_cast<mtl::RendererBackend&>(backend);

        const auto& mtlDevice = mtlBackend.getDevice();

        if (!commandCaptureScope) {
            if (const auto& cmdQueue = mtlBackend.getCommandQueue()) {
                if (const auto captureManager = NS::RetainPtr(MTL::CaptureManager::sharedCaptureManager())) {
                    // NOLINTNEXTLINE(bugprone-assignment-in-if-condition)
                    if ((commandCaptureScope = NS::TransferPtr(captureManager->newCaptureScope(cmdQueue.get())))) {
                        const auto label = "Renderer::Impl frame=" + util::toString(frameCount);
                        commandCaptureScope->setLabel(NS::String::string(label.c_str(), NS::UTF8StringEncoding));
                        captureManager->setDefaultCaptureScope(commandCaptureScope.get());
                    }
                }
            }
        }

        // "When you capture a frame programmatically, you can capture Metal commands that span multiple
        //  frames by using a custom capture scope. For example, by calling begin() at the start of frame
        //  1 and end() after frame 3, the trace will contain command data from all the buffers that were
        //  committed in the three frames."
        // https://developer.apple.com/documentation/metal/debugging_tools/capturing_gpu_command_data_programmatically
        if constexpr (0 < CaptureFrameStart && 0 < CaptureFrameCount) {
            if (commandCaptureScope) {
                const auto captureManager = NS::RetainPtr(MTL::CaptureManager::sharedCaptureManager());
                if (frameCount == CaptureFrameStart) {
                    // Save to a .gputrace document inside the app's Documents
                    // directory. We pull this out via `xcrun simctl get_app_container`
                    // and open in Xcode for offline inspection — works without
                    // attaching a debugger.
                    constexpr auto captureDest = MTL::CaptureDestination::CaptureDestinationGPUTraceDocument;
                    if (captureManager && !captureManager->isCapturing() &&
                        captureManager->supportsDestination(captureDest)) {
                        if (auto captureDesc = NS::TransferPtr(MTL::CaptureDescriptor::alloc()->init())) {
                            captureDesc->setCaptureObject(mtlDevice.get());
                            captureDesc->setDestination(captureDest);
                            // Save to $HOME/Documents/klattra-frame-<frame>.gputrace.
                            // On iOS simulator, HOME is the app container; we pull
                            // the trace out via `xcrun simctl get_app_container`
                            // after the app exits.
                            if (const char* home = std::getenv("HOME")) {
                                const std::string path = std::string(home) + "/Documents/klattra-frame-" +
                                                         util::toString(frameCount) + ".gputrace";
                                auto pathStr = NS::String::string(path.c_str(), NS::UTF8StringEncoding);
                                if (auto outURL = NS::URL::fileURLWithPath(pathStr)) {
                                    captureDesc->setOutputURL(outURL);
                                    Log::Warning(Event::Render, std::string("Capture output: ") + path);
                                }
                            }
                            NS::Error* errorPtr = nullptr;
                            if (captureManager->startCapture(captureDesc.get(), &errorPtr)) {
                                Log::Warning(Event::Render, "Capture Started");
                            } else {
                                std::string errStr = "<none>";
                                if (auto error = NS::TransferPtr(errorPtr)) {
                                    if (auto str = error->localizedDescription()) {
                                        if (auto cstr = str->utf8String()) {
                                            errStr = cstr;
                                        }
                                    }
                                }
                                Log::Warning(Event::Render, "Capture Failed: " + errStr);
                            }
                        }
                    }
                }
            }
        }
        if (commandCaptureScope) {
            commandCaptureScope->beginScope();

            const auto captureManager = NS::RetainPtr(MTL::CaptureManager::sharedCaptureManager());
            if (captureManager->isCapturing()) {
                Log::Info(Event::Render, "Capturing frame " + util::toString(frameCount));
            }
        }
    }
#endif // MLN_RENDER_BACKEND_METAL

    // Blocks execution until the renderable is available.
    backend.getDefaultRenderable().wait();
    context.beginFrame();

    if (!staticData) {
        staticData = std::make_unique<RenderStaticData>(std::make_unique<gfx::ShaderRegistry>());

        // Initialize shaders for drawables
        const auto programParameters = ProgramParameters{pixelRatio, false};
        backend.initShaders(*staticData->shaders, programParameters);

        // Notify post-shader registration
        observer->onRegisterShaders(*staticData->shaders);
    }

    const auto& renderTreeParameters = renderTree.getParameters();
    staticData->has3D = renderTreeParameters.has3D;
    staticData->backendSize = backend.getDefaultRenderable().getSize();

    if (renderState == RenderState::Never) {
        observer->onWillStartRenderingMap();
    }

    observer->onWillStartRenderingFrame();

    const TransformState& state = renderTreeParameters.transformParams.state;
    const Size& size = staticData->backendSize;
    const EdgeInsets& frustumOffset = state.getFrustumOffset();
    const gfx::ScissorRect scissorRect = {
        .x = static_cast<int32_t>(frustumOffset.left() * pixelRatio),
#if MLN_RENDER_BACKEND_OPENGL
        .y = static_cast<int32_t>(frustumOffset.bottom() * pixelRatio),
#else
        .y = static_cast<int32_t>(frustumOffset.top() * pixelRatio),
#endif
        .width = size.width - static_cast<uint32_t>((frustumOffset.left() + frustumOffset.right()) * pixelRatio),
        .height = size.height - static_cast<uint32_t>((frustumOffset.top() + frustumOffset.bottom()) * pixelRatio),
    };

    PaintParameters parameters{
        context,
        pixelRatio,
        backend,
        renderTreeParameters.light,
        renderTreeParameters.mapMode,
        renderTreeParameters.debugOptions,
        renderTreeParameters.timePoint,
        renderTreeParameters.transformParams,
        *staticData,
        renderTree.getLineAtlas(),
        renderTree.getPatternAtlas(),
        frameCount,
        updateParameters->tileLodMinRadius,
        updateParameters->tileLodScale,
        updateParameters->tileLodPitchThreshold,
        updateParameters->tileLodMode,
        scissorRect,
    };

    parameters.symbolFadeChange = renderTreeParameters.symbolFadeChange;
    parameters.opaquePassCutoff = renderTreeParameters.opaquePassCutOff;

    // Expose the orchestrator's RenderTerrain to tweakers via PaintParameters.
    // Only set when terrain is actually enabled (matches the gating the
    // orchestrator does internally before setting each layer's activeTerrain).
    if (auto* terrain = orchestrator.getRenderTerrain(); terrain && terrain->isEnabled()) {
        parameters.activeTerrain = terrain;
    }
    const auto& sourceRenderItems = renderTree.getSourceRenderItems();

    const auto& layerRenderItems = renderTree.getLayerRenderItemMap();

    // - UPLOAD PASS -------------------------------------------------------------------------------
    // Uploads all required buffers and images before we do any actual rendering.
    {
        const auto uploadPass = parameters.encoder->createUploadPass("upload",
                                                                     parameters.backend.getDefaultRenderable());
#if !defined(NDEBUG)
        const auto debugGroup = uploadPass->createDebugGroup("upload");
#endif

        // Update all clipping IDs + upload buckets.
        for (const RenderItem& item : sourceRenderItems) {
            item.upload(*uploadPass);
        }
        for (const RenderItem& item : layerRenderItems) {
            item.upload(*uploadPass);
        }
        staticData->upload(*uploadPass);
        renderTree.getLineAtlas().upload(*uploadPass);
        renderTree.getPatternAtlas().upload(*uploadPass);
    }

    // - LAYER GROUP UPDATE ------------------------------------------------------------------------
    // Updates all layer groups and process changes
    if (staticData && staticData->shaders) {
        orchestrator.updateLayers(
            *staticData->shaders, context, renderTreeParameters.transformParams.state, updateParameters, renderTree);
    }

    orchestrator.processChanges();

    // Upload layer groups
    {
        const auto uploadPass = parameters.encoder->createUploadPass("layerGroup-upload",
                                                                     parameters.backend.getDefaultRenderable());
#if !defined(NDEBUG)
        const auto debugGroup = uploadPass->createDebugGroup("layerGroup-upload");
#endif

        // Update the debug layer groups
        orchestrator.updateDebugLayerGroups(renderTree, parameters);

        // Tweakers are run in the upload pass so they can set up uniforms.
        parameters.currentLayer = 0;
        orchestrator.visitLayerGroups([&](LayerGroupBase& layerGroup) {
            layerGroup.runTweakers(renderTree, parameters);
            parameters.currentLayer++;
        });

        // Run terrain tweaker if terrain is enabled
        if (auto* terrain = orchestrator.getRenderTerrain()) {
            if (auto* terrainTweaker = terrain->getTweaker()) {
                if (const auto& layerGroup = terrain->getLayerGroup()) {
                    terrainTweaker->execute(*layerGroup, parameters);
                }
            }
        }

        parameters.currentLayer = 0;
        orchestrator.visitDebugLayerGroups([&](LayerGroupBase& layerGroup) {
            layerGroup.runTweakers(renderTree, parameters);
            parameters.currentLayer++;
        });

        // Give the layers a chance to upload
        orchestrator.visitLayerGroups([&](LayerGroupBase& layerGroup) { layerGroup.upload(*uploadPass); });

        // Give the render targets a chance to upload
        orchestrator.visitRenderTargets([&](RenderTarget& renderTarget) { renderTarget.upload(*uploadPass); });

        // Upload the Debug layer group
        orchestrator.visitDebugLayerGroups([&](LayerGroupBase& layerGroup) { layerGroup.upload(*uploadPass); });
    }

    const Size atlasSize = parameters.patternAtlas.getPixelSize();
    const auto& worldSize = parameters.staticData.backendSize;
    const shaders::GlobalPaintParamsUBO globalPaintParamsUBO = {
        .pattern_atlas_texsize = {static_cast<float>(atlasSize.width), static_cast<float>(atlasSize.height)},
        .units_to_pixels = {1.0f / parameters.pixelsToGLUnits[0], 1.0f / parameters.pixelsToGLUnits[1]},
        .world_size = {static_cast<float>(worldSize.width), static_cast<float>(worldSize.height)},
        .camera_to_center_distance = parameters.state.getCameraToCenterDistance(),
        .symbol_fade_change = parameters.symbolFadeChange,
        .aspect_ratio = parameters.state.getSize().aspectRatio(),
        .pixel_ratio = parameters.pixelRatio,
        .map_zoom = static_cast<float>(parameters.state.getZoom()),
        .pad1 = 0,
    };
    auto& globalUniforms = context.mutableGlobalUniformBuffers();
    globalUniforms.createOrUpdate(shaders::idGlobalPaintParamsUBO, &globalPaintParamsUBO, context);

    // - 3D PASS
    // -------------------------------------------------------------------------------------
    // Renders any 3D layers bottom-to-top to unique FBOs with texture
    // attachments, but share the same depth rbo between them.
    const auto common3DPass = [&] {
        if (parameters.staticData.has3D) {
            parameters.staticData.backendSize = parameters.backend.getDefaultRenderable().getSize();

            const auto debugGroup(parameters.encoder->createDebugGroup("common-3d"));
            parameters.pass = RenderPass::Pass3D;

            // TODO is this needed?
            // if (!parameters.staticData.depthRenderbuffer ||
            //    parameters.staticData.depthRenderbuffer->getSize() != parameters.staticData.backendSize) {
            //    parameters.staticData.depthRenderbuffer =
            //        parameters.context.createRenderbuffer<gfx::RenderbufferPixelType::Depth>(
            //            parameters.staticData.backendSize);
            //}
            // parameters.staticData.depthRenderbuffer->setShouldClear(true);
        }
    };

    const auto drawable3DPass = [&] {
        const auto debugGroup(parameters.encoder->createDebugGroup("drawables-3d"));
        assert(parameters.pass == RenderPass::Pass3D);

        // draw layer groups, 3D pass
        parameters.currentLayer = static_cast<uint32_t>(orchestrator.numLayerGroups()) - 1;
        orchestrator.visitLayerGroups([&](LayerGroupBase& layerGroup) {
            layerGroup.render(orchestrator, parameters);
            if (parameters.currentLayer > 0) {
                parameters.currentLayer--;
            }
        });
    };

    const auto drawableTargetsPass = [&] {
        // draw render targets
        orchestrator.visitRenderTargets(
            [&](RenderTarget& renderTarget) { renderTarget.render(orchestrator, renderTree, parameters); });
    };

    const auto commonClearPass = [&] {
        // - CLEAR
        // -------------------------------------------------------------------------------------
        // Renders the backdrop of the OpenGL view. This also paints in areas where
        // we don't have any tiles whatsoever.
        {
            std::optional<Color> color;
            if (parameters.debugOptions & MapDebugOptions::Overdraw) {
                color = Color::black();
            } else {
                // Always clear (KLATTRA, 2D black flash): with a shared
                // context this used to skip the clear (LoadActionLoad), but
                // CAMetalLayer drawable contents are UNDEFINED after
                // presentation, so the frame base loaded garbage/black. Any
                // frame whose background tile quads miss (cover fragments,
                // style mid-load at boot) painted over a black base — the
                // flash. Clearing to the style background makes the base
                // match the map, and clears are cheaper than loads on TBDR.
                color = renderTreeParameters.backgroundColor;
                // .58 horizon void retint: with terrain active, everything
                // beyond the mesh's far cover shows this clear colour — as
                // the style background it read as dark-green void bands at
                // the top of flyover frames (Robert's screenshots,
                // 2026-07-12). The .50 haze already fades the far mesh
                // toward the #8FC3DE sky/sea backdrop tone precisely "so
                // the horizon merges into the backdrop" — this finishes
                // that design by making the backdrop actually BE that tone
                // whenever the terrain mesh is on screen. 2D rendering is
                // untouched.
                static const bool voidRetint = std::getenv("KLATTRA_DISABLE_VOID_RETINT") == nullptr;
                if (voidRetint && parameters.activeTerrain) {
                    color = Color{0.56078431f, 0.76470588f, 0.87058824f, 1.0f};
                }
            }
            parameters.renderPass = parameters.encoder->createRenderPass(
                "main buffer",
                {.renderable = parameters.backend.getDefaultRenderable(),
                 .clearColor = color,
                 .clearDepth = 1.0f,
                 .clearStencil = 0});
        }
    };

    // Actually render the layers
    // Drawables
    const auto drawableOpaquePass = [&] {
        const auto debugGroup(parameters.renderPass->createDebugGroup("drawables-opaque"));
        parameters.pass = RenderPass::Opaque;
        parameters.depthRangeSize = 1 - (orchestrator.numLayerGroups() + 2) * PaintParameters::numSublayers *
                                            PaintParameters::depthEpsilon;

        // draw layer groups, opaque pass
        parameters.currentLayer = 0;
        orchestrator.visitLayerGroupsReversed([&](LayerGroupBase& layerGroup) {
            layerGroup.render(orchestrator, parameters);
            parameters.currentLayer++;
        });
    };

    const auto drawableTranslucentPass = [&] {
        const auto debugGroup(parameters.renderPass->createDebugGroup("drawables-translucent"));
        parameters.pass = RenderPass::Translucent;
        parameters.depthRangeSize = 1 - (orchestrator.numLayerGroups() + 2) * PaintParameters::numSublayers *
                                            PaintParameters::depthEpsilon;

        // draw layer groups, translucent pass
        auto* terrain = orchestrator.getRenderTerrain();
        const auto terrainLayerGroup = terrain ? terrain->getLayerGroup() : nullptr;
        parameters.currentLayer = static_cast<uint32_t>(orchestrator.numLayerGroups()) - 1;
        orchestrator.visitLayerGroups([&](LayerGroupBase& layerGroup) {
            if (terrainLayerGroup && terrainLayerGroup.get() == &layerGroup) {
                if (parameters.currentLayer > 0) {
                    parameters.currentLayer--;
                }
                return;
            }
            layerGroup.render(orchestrator, parameters);
            if (parameters.currentLayer > 0) {
                parameters.currentLayer--;
            }
        });

        // Finally, render any legacy layers which have not been converted to drawables.
        // Note that they may be out of order, this is just a temporary fix for `RenderLocationIndicatorLayer` (#2216)
        parameters.depthRangeSize = 1 - (layerRenderItems.size() + 2) * PaintParameters::numSublayers *
                                            PaintParameters::depthEpsilon;
        int32_t i = static_cast<int32_t>(layerRenderItems.size()) - 1;
        for (auto it = layerRenderItems.begin(); it != layerRenderItems.end() && i >= 0; ++it, --i) {
            parameters.currentLayer = i;
            const RenderItem& item = *it;
            if (item.hasRenderPass(parameters.pass)) {
                item.render(parameters);
            }
        }
    };

    const auto drawableTerrainPass = [&] {
        auto* terrain = orchestrator.getRenderTerrain();
        if (!terrain || !terrain->isEnabled() || !terrain->getLayerGroup()) {
            return;
        }

        // Terrain needs normal 3D depth testing against itself, but the
        // flat map passes populate the main buffer depth plane first. Draw
        // terrain in a tiny second pass that preserves color and resets only
        // depth, so hills can occlude valleys without being hidden by the
        // 2D basemap.
        parameters.renderPass.reset();
        parameters.renderPass = parameters.encoder->createRenderPass(
            "terrain buffer",
            {.renderable = parameters.backend.getDefaultRenderable(),
             .clearColor = std::nullopt,
             .clearDepth = 1.0f,
             .clearStencil = std::nullopt});
        context.bindGlobalUniformBuffers(*parameters.renderPass);

        const auto debugGroup(parameters.renderPass->createDebugGroup("terrain-final"));
        parameters.pass = RenderPass::Translucent;
        parameters.depthRangeSize = 1.0f;
        parameters.currentLayer = 0;
        terrain->getLayerGroup()->render(orchestrator, parameters);
    };

    const auto drawableDebugOverlays = [&] {
        // Renders debug overlays.
        {
            const auto debugGroup(parameters.renderPass->createDebugGroup("debug"));
            parameters.currentLayer = 0;
            orchestrator.visitDebugLayerGroups([&](LayerGroupBase& layerGroup) {
                layerGroup.render(orchestrator, parameters);
                parameters.currentLayer++;
            });
        }
    };

    // KLATTRA prebake gate (launch shading settle): until the initial
    // relief cover has baked, present clear-only "paper" frames. The
    // prepare targets still render and flush below, so bakes land while
    // nothing half-shaded is ever shown; the first content frame then
    // arrives with relief already bound. Launch-window only: the gate
    // opens once — settled, timed out, or not applicable — and never
    // holds again.
    bool holdMapContent = false;
    if (!prebakeGateOpen) {
        if (renderTreeParameters.mapMode != MapMode::Continuous || klattraDisablePrebakeGate()) {
            prebakeGateOpen = true;
        } else {
            const double nowSeconds = util::MonotonicTimer::now().count();
            if (!prebakeGateStart) {
                prebakeGateStart = nowSeconds;
            }
            const double elapsedMs = (nowSeconds - *prebakeGateStart) * 1000.0;
            const bool pending = !updateParameters->styleLoaded || orchestrator.hillshadeBakesPending();
            if (!pending || elapsedMs >= klattraPrebakeGateTimeoutMs()) {
                prebakeGateOpen = true;
                klattraPrebakeEmit("[KLATTRA PREBAKE] gate-open reason=" +
                                   std::string(pending ? "timeout" : "settled") +
                                   " heldFrames=" + util::toString(prebakeGateHeldFrames) +
                                   " elapsedMs=" + util::toString(static_cast<uint64_t>(elapsedMs)));
            } else {
                holdMapContent = true;
                ++prebakeGateHeldFrames;
            }
        }
    }

    if (parameters.staticData.has3D && !holdMapContent) {
        common3DPass();
        drawable3DPass();
    }
    drawableTargetsPass();
    // Submit the batched offscreen (drape) bakes before encoding the main
    // pass: one commit for all targets, ordered ahead of the main pass by
    // queue commit order (the main pass buffer is committed at present).
    context.flushOffscreenRenderWork();
    commonClearPass();
    context.bindGlobalUniformBuffers(*parameters.renderPass);
    if (!holdMapContent) {
        drawableOpaquePass();
        drawableTranslucentPass();
        drawableTerrainPass();
        drawableDebugOverlays();
    }

    // Give the layers a chance to do cleanup
    orchestrator.visitLayerGroups([&](LayerGroupBase& layerGroup) { layerGroup.postRender(orchestrator, parameters); });
    context.unbindGlobalUniformBuffers(*parameters.renderPass);

    // Ends the RenderPass
    parameters.renderPass.reset();

    const auto startRendering = util::MonotonicTimer::now().count();
    // present submits render commands
    parameters.encoder->present(parameters.backend.getDefaultRenderable());
    context.renderingStats().renderingTime = util::MonotonicTimer::now().count() - startRendering;

    parameters.encoder.reset();
    context.endFrame();

#if MLN_RENDER_BACKEND_METAL
    if constexpr (EnableMetalCapture) {
        if (commandCaptureScope) {
            commandCaptureScope->endScope();

            const auto captureManager = NS::RetainPtr(MTL::CaptureManager::sharedCaptureManager());
            if (frameCount == CaptureFrameStart + CaptureFrameCount - 1 && captureManager->isCapturing()) {
                captureManager->stopCapture();
            }
        }
    }
#endif // MLN_RENDER_BACKEND_METAL

    context.renderingStats().encodingTime = renderTree.getElapsedTime() - context.renderingStats().renderingTime;

    observer->onDidFinishRenderingFrame(
        renderTreeParameters.loaded ? RendererObserver::RenderMode::Full : RendererObserver::RenderMode::Partial,
        // Held paper frames must keep the render loop alive: nothing else
        // is guaranteed to invalidate between the last bake completing and
        // the gate opening (or timing out).
        renderTreeParameters.needsRepaint || holdMapContent,
        renderTreeParameters.placementChanged,
        context.threadSafeCopyRenderingStats());

    if (!renderTreeParameters.loaded) {
        renderState = RenderState::Partial;
    } else if (renderState != RenderState::Fully) {
        renderState = RenderState::Fully;
        observer->onDidFinishRenderingMap();
    }

    frameCount += 1;
    MLN_END_FRAME();
}

void Renderer::Impl::reduceMemoryUse() {
    assert(gfx::BackendScope::exists());
    backend.getContext().reduceMemoryUsage();
}

} // namespace mbgl
