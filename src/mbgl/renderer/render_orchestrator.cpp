#include <mbgl/renderer/render_orchestrator.hpp>

#include <mbgl/annotation/annotation_manager.hpp>
#include <mbgl/layermanager/layer_manager.hpp>
#include <mbgl/renderer/change_request.hpp>
#include <mbgl/renderer/renderer_observer.hpp>
#include <mbgl/renderer/render_source.hpp>
#include <mbgl/renderer/render_layer.hpp>
#include <mbgl/renderer/render_target.hpp>
#include <mbgl/style/layers/hillshade_layer_impl.hpp>
#include <mbgl/renderer/render_static_data.hpp>
#include <mbgl/renderer/render_tree.hpp>
#include <mbgl/renderer/render_terrain.hpp>
#include <mbgl/renderer/update_parameters.hpp>
#include <mbgl/renderer/upload_parameters.hpp>
#include <mbgl/renderer/pattern_atlas.hpp>
#include <mbgl/renderer/paint_parameters.hpp>
#include <mbgl/renderer/transition_parameters.hpp>
#include <mbgl/renderer/property_evaluation_parameters.hpp>
#include <mbgl/renderer/tile_parameters.hpp>
#include <mbgl/renderer/render_tile.hpp>
#include <mbgl/renderer/style_diff.hpp>
#include <mbgl/renderer/query.hpp>
#include <mbgl/renderer/image_manager.hpp>
#include <mbgl/geometry/line_atlas.hpp>
#include <mbgl/style/source_impl.hpp>
#include <mbgl/style/transition_options.hpp>
#include <mbgl/text/glyph_manager.hpp>
#include <mbgl/tile/tile.hpp>
#include <mbgl/util/instrumentation.hpp>
#include <mbgl/util/math.hpp>
#include <mbgl/util/string.hpp>
#include <mbgl/util/logging.hpp>

#include <atomic>
#include <chrono>

#include <algorithm>

namespace mbgl {

using namespace style;

LayerRenderItem::LayerRenderItem(RenderLayer& layer_, RenderSource* source_, uint32_t index_)
    : layer(layer_),
      source(source_),
      index(index_) {}

bool LayerRenderItem::hasRenderPass(RenderPass pass) const {
    return layer.get().hasRenderPass(pass);
}
void LayerRenderItem::upload(gfx::UploadPass& pass) const {
    layer.get().upload(pass);
}
void LayerRenderItem::render(PaintParameters& parameters) const {
    layer.get().render(parameters);
}
const std::string& LayerRenderItem::getName() const {
    return layer.get().getID();
}

void LayerRenderItem::updateDebugDrawables(DebugLayerGroupMap&, PaintParameters&) const {};

namespace {

RendererObserver& nullObserver() {
    static RendererObserver observer;
    return observer;
}

class RenderTreeImpl final : public RenderTree {
public:
    RenderTreeImpl(std::unique_ptr<RenderTreeParameters> parameters_,
                   std::set<LayerRenderItem> layerRenderItems_,
                   std::vector<std::unique_ptr<RenderItem>> sourceRenderItems_,
                   LineAtlas& lineAtlas_,
                   PatternAtlas& patternAtlas_,
                   RenderLayerReferences layersNeedPlacement_,
                   Immutable<Placement> placement_,
                   bool updateSymbolOpacities_,
                   double startTime_)
        : RenderTree(std::move(parameters_), startTime_),
          layerRenderItems(std::move(layerRenderItems_)),
          sourceRenderItems(std::move(sourceRenderItems_)),
          lineAtlas(lineAtlas_),
          patternAtlas(patternAtlas_),
          layersNeedPlacement(std::move(layersNeedPlacement_)),
          placement(std::move(placement_)),
          updateSymbolOpacities(updateSymbolOpacities_) {}

    void prepare() override {
        MLN_TRACE_FUNC();

        for (auto it = layersNeedPlacement.rbegin(); it != layersNeedPlacement.rend(); ++it) {
            placement->updateLayerBuckets(*it, parameters->transformParams.state, updateSymbolOpacities);
        }
    }

    const std::set<LayerRenderItem>& getLayerRenderItemMap() const noexcept override { return layerRenderItems; }
    RenderItems getLayerRenderItems() const override { return {layerRenderItems.begin(), layerRenderItems.end()}; }
    RenderItems getSourceRenderItems() const override {
        RenderItems result;
        result.reserve(sourceRenderItems.size());
        for (const auto& item : sourceRenderItems) result.emplace_back(*item);
        return result;
    }
    LineAtlas& getLineAtlas() const override { return lineAtlas; }
    PatternAtlas& getPatternAtlas() const override { return patternAtlas; }

    std::set<LayerRenderItem> layerRenderItems;
    std::vector<std::unique_ptr<RenderItem>> sourceRenderItems;
    std::reference_wrapper<LineAtlas> lineAtlas;
    std::reference_wrapper<PatternAtlas> patternAtlas;
    RenderLayerReferences layersNeedPlacement;
    Immutable<Placement> placement;
    bool updateSymbolOpacities;
};

} // namespace

RenderOrchestrator::RenderOrchestrator(bool backgroundLayerAsColor_,
                                       TaggedScheduler& threadPool_,
                                       const std::optional<std::string>& localFontFamily_)
    : observer(&nullObserver()),
      glyphManager(std::make_unique<GlyphManager>(std::make_unique<LocalGlyphRasterizer>(localFontFamily_))),
      imageManager(ImageManager::create()),
      lineAtlas(std::make_unique<LineAtlas>()),
      patternAtlas(std::make_unique<PatternAtlas>()),
      imageImpls(makeMutable<std::vector<Immutable<style::Image::Impl>>>()),
      sourceImpls(makeMutable<std::vector<Immutable<style::Source::Impl>>>()),
      layerImpls(makeMutable<std::vector<Immutable<style::Layer::Impl>>>()),
      renderLight(makeMutable<Light::Impl>()),
      backgroundLayerAsColor(backgroundLayerAsColor_),
      threadPool(threadPool_) {
    glyphManager->setObserver(this);
    imageManager->setObserver(this);
}

RenderOrchestrator::~RenderOrchestrator() {
    MLN_TRACE_FUNC();

    if (contextLost) {
        // Signal all RenderLayers that the context was lost
        // before cleaning up. At the moment, only CustomLayer is
        // interested whether rendering context is lost. However, it would be
        // beneficial for dynamically loaded or other custom built-in plugins.
        for (const auto& entry : renderLayers) {
            RenderLayer& layer = *entry.second;
            layer.markContextDestroyed();
        }
    }

    // Wait for any deferred cleanup tasks to complete before releasing and potentially
    // destroying the scheduler.  Those cleanup tasks must not hold the final reference
    // to the scheduler because it cannot be destroyed from one of its own pool threads.
    threadPool.waitForEmpty();
}

void RenderOrchestrator::setObserver(RendererObserver* observer_) {
    observer = observer_ ? observer_ : &nullObserver();
}

std::unique_ptr<RenderTree> RenderOrchestrator::createRenderTree(
    const std::shared_ptr<UpdateParameters>& updateParameters, gfx::DynamicTextureAtlasPtr dynamicTextureAtlas) {
    MLN_TRACE_FUNC();

    const auto startTime = util::MonotonicTimer::now().count();

    const bool isMapModeContinuous = updateParameters->mode == MapMode::Continuous;
    if (!isMapModeContinuous) {
        // Reset zoom history state.
        zoomHistory.first = true;
    }

    if (LayerManager::annotationsEnabled) {
        if (auto guard = updateParameters->annotationManager.lock(); updateParameters->annotationManager) {
            updateParameters->annotationManager->updateData();
        }
    }

    const bool zoomChanged = zoomHistory.update(static_cast<float>(updateParameters->transformState.getZoom()),
                                                updateParameters->timePoint);

    const TransitionOptions transitionOptions = isMapModeContinuous ? updateParameters->transitionOptions
                                                                    : TransitionOptions();

    const TransitionParameters transitionParameters{.now = updateParameters->timePoint,
                                                    .transition = transitionOptions};
    const auto transitionDuration = transitionOptions.duration.value_or(
        isMapModeContinuous ? util::DEFAULT_TRANSITION_DURATION : Duration::zero());

    PropertyEvaluationParameters evaluationParameters{zoomHistory, updateParameters->timePoint, transitionDuration};
    evaluationParameters.zoomChanged = zoomChanged;

    TileParameters tileParameters{.pixelRatio = updateParameters->pixelRatio,
                                  .debugOptions = updateParameters->debugOptions,
                                  .transformState = updateParameters->transformState,
                                  .fileSource = updateParameters->fileSource,
                                  .mode = updateParameters->mode,
                                  .annotationManager = updateParameters->annotationManager,
                                  .imageManager = imageManager,
                                  .glyphManager = glyphManager,
                                  .prefetchZoomDelta = updateParameters->prefetchZoomDelta,
                                  .threadPool = threadPool,
                                  .tileLodMinRadius = updateParameters->tileLodMinRadius,
                                  .tileLodScale = updateParameters->tileLodScale,
                                  .tileLodPitchThreshold = updateParameters->tileLodPitchThreshold,
                                  .tileLodZoomShift = updateParameters->tileLodZoomShift,
                                  .tileLodMode = updateParameters->tileLodMode,
                                  .dynamicTextureAtlas = dynamicTextureAtlas};

    glyphManager->setURL(updateParameters->glyphURL);
    glyphManager->setFontFaces(updateParameters->fontFaces);

    // Update light.
    const bool lightChanged = renderLight.impl != updateParameters->light;

    if (lightChanged) {
        renderLight.impl = updateParameters->light;
        renderLight.transition(transitionParameters);
    }

    if (lightChanged || zoomChanged || renderLight.hasTransition()) {
        renderLight.evaluate(evaluationParameters);
    }

    // Update terrain. Tear the old RenderTerrain down through the change
    // queue before destroying/replacing it — its destructor cannot emit
    // change requests, so dropping it directly strands every drape render
    // target (still re-rendered per frame) and leaves its layer group
    // active in the renderer.
    if (updateParameters->terrain) {
        if (!renderTerrain || renderTerrain->getImpl() != *updateParameters->terrain) {
            if (renderTerrain) {
                UniqueChangeRequestVec terrainChanges;
                renderTerrain->teardown(terrainChanges);
                addChanges(terrainChanges);
            }
            renderTerrain = std::make_unique<RenderTerrain>(*updateParameters->terrain);
        }
        renderTerrain->update(*updateParameters);
    } else if (renderTerrain) {
        UniqueChangeRequestVec terrainChanges;
        renderTerrain->teardown(terrainChanges);
        addChanges(terrainChanges);
        renderTerrain.reset();
    }

    const ImageDifference imageDiff = diffImages(imageImpls, updateParameters->images);
    imageImpls = updateParameters->images;

    // Only trigger tile reparse for changed images. Changed images only need a
    // relayout when they have a different size.
    bool hasImageDiff = !imageDiff.removed.empty();

    // Remove removed images from sprite atlas.
    for (const auto& entry : imageDiff.removed) {
        imageManager->removeImage(entry.first);
        patternAtlas->removePattern(entry.first);
    }

    // Add added images to sprite atlas.
    for (const auto& entry : imageDiff.added) {
        imageManager->addImage(entry.second);
    }

    // Update changed images.
    for (const auto& entry : imageDiff.changed) {
        if (imageManager->updateImage(entry.second.after)) {
            patternAtlas->removePattern(entry.first);
            hasImageDiff = true;
        }
    }

    imageManager->notifyIfMissingImageAdded();
    imageManager->setLoaded(updateParameters->spriteLoaded);

    const LayerDifference layerDiff = diffLayers(layerImpls, updateParameters->layers);
    layerImpls = updateParameters->layers;
    const bool layersAddedOrRemoved = !layerDiff.added.empty() || !layerDiff.removed.empty();

    std::vector<std::unique_ptr<ChangeRequest>> changes;

    // Remove render layers for removed layers.
    for (const auto& entry : layerDiff.removed) {
        MLN_TRACE_ZONE(remove layer);
        const auto hit = renderLayers.find(entry.first);
        if (hit != renderLayers.end()) {
            hit->second->layerRemoved(changes);
            renderLayers.erase(hit);
        }
    }

    // Create render layers for newly added layers.
    for (const auto& entry : layerDiff.added) {
        MLN_TRACE_ZONE(add layer);
        auto renderLayer = LayerManager::get()->createRenderLayer(entry.second);
        renderLayer->transition(transitionParameters);
        renderLayers.emplace(entry.first, std::move(renderLayer));
    }

    // Update render layers for changed layers.
    for (const auto& entry : layerDiff.changed) {
        MLN_TRACE_ZONE(change layer);
        if (const auto& renderLayer = renderLayers.at(entry.first)) {
            const auto& newLayer = entry.second.after;

            renderLayer->layerChanged(transitionParameters, newLayer, changes);
            renderLayer->transition(transitionParameters, newLayer);
        }
    }

    if (layersAddedOrRemoved) {
        orderedLayers.clear();
        orderedLayers.reserve(layerImpls->size());
        [[maybe_unused]] int32_t layerIndex = 0;
        for (const auto& layerImpl : *layerImpls) {
            RenderLayer* layer = renderLayers.at(layerImpl->id).get();
            assert(layer);
            orderedLayers.emplace_back(*layer);

            // We're mutating the list of ordered layers and must notify them of their new assigned indices
            layer->layerIndexChanged(layerIndex++, changes);
        }
    }
    assert(orderedLayers.size() == renderLayers.size());

    if (layersAddedOrRemoved || !layerDiff.changed.empty()) {
        glyphManager->evict(fontStacks(*layerImpls));
    }

    // Update layers for class and zoom changes.
    std::unordered_set<std::string> constantsMaskChanged;
    for (RenderLayer& layer : orderedLayers) {
        MLN_TRACE_ZONE(update layer);
        const std::string& id = layer.getID();
        const bool layerAddedOrChanged = layerDiff.added.contains(id) || layerDiff.changed.contains(id);
        evaluationParameters.layerChanged = layerAddedOrChanged;
        evaluationParameters.hasCrossfade = layer.hasCrossfade();

        // Only re-evaluate on change of zoom if the style has some reference to it
        using Dependency = expression::Dependency;
        const bool zoomChangedAndMatters = zoomChanged && !layerAddedOrChanged &&
                                           (layer.getStyleDependencies() & Dependency::Zoom);

        if (layerAddedOrChanged || zoomChangedAndMatters || evaluationParameters.hasCrossfade ||
            layer.hasTransition()) {
            const auto previousMask = layer.evaluatedProperties->constantsMask();
            layer.evaluate(evaluationParameters);
            if (previousMask != layer.evaluatedProperties->constantsMask()) {
                constantsMaskChanged.insert(id);
            }
        }
    }

    const SourceDifference sourceDiff = diffSources(sourceImpls, updateParameters->sources);
    sourceImpls = updateParameters->sources;

    // Remove render layers for removed sources.
    for (const auto& entry : sourceDiff.removed) {
        renderSources.erase(entry.first);
    }

    // Create render sources for newly added sources.
    for (const auto& entry : sourceDiff.added) {
        MLN_TRACE_ZONE(create source);
        std::unique_ptr<RenderSource> renderSource = RenderSource::create(entry.second, threadPool);
        renderSource->setObserver(this);
        renderSource->setCacheEnabled(tileCacheEnabled);
        renderSources.emplace(entry.first, std::move(renderSource));
    }
    transformState = updateParameters->transformState;
    const bool tiltedView = transformState.getPitch() != 0.0f;

    // Create parameters for the render tree.
    auto renderTreeParameters = std::make_unique<RenderTreeParameters>(updateParameters->transformState,
                                                                       updateParameters->mode,
                                                                       updateParameters->debugOptions,
                                                                       updateParameters->timePoint,
                                                                       renderLight.getEvaluated());

    std::set<LayerRenderItem> layerRenderItems;
    layersNeedPlacement.clear();
    auto renderItemsEmplaceHint = layerRenderItems.begin();

    // Reserve size for filteredLayersForSource if there are sources.
    if (!sourceImpls->empty()) {
        filteredLayersForSource.reserve(layerImpls->size());
    }

    // Track which layers are flagged for rendering
    std::vector<bool> updateList(orderedLayers.size());

    // Update all sources and initialize renderItems.
    for (const auto& sourceImpl : *sourceImpls) {
        MLN_TRACE_ZONE(update source);
        MLN_ZONE_STR(sourceImpl->id);

        RenderSource* source = renderSources.at(sourceImpl->id).get();
        bool sourceNeedsRendering = false;
        bool sourceNeedsRelayout = false;

        for (std::size_t index = 0; index < orderedLayers.size(); ++index) {
            RenderLayer& layer = orderedLayers[index];
            const auto* layerInfo = layer.baseImpl->getTypeInfo();
            const bool layerIsVisible = layer.baseImpl->visibility != style::VisibilityType::None;
            const bool zoomFitsLayer = layer.supportsZoom(zoomHistory.lastZoom);
            renderTreeParameters->has3D |= (layerInfo->pass3d == LayerTypeInfo::Pass3D::Required);

            if (layerInfo->source != LayerTypeInfo::Source::NotRequired) {
                if (layer.baseImpl->source == sourceImpl->id) {
                    const std::string& layerId = layer.getID();
                    sourceNeedsRelayout = (sourceNeedsRelayout || hasImageDiff ||
                                           constantsMaskChanged.contains(layerId) ||
                                           hasLayoutDifference(layerDiff, layerId));
                    if (layerIsVisible) {
                        filteredLayersForSource.push_back(layer.evaluatedProperties);
                        if (zoomFitsLayer) {
                            sourceNeedsRendering = true;
                            renderItemsEmplaceHint = layerRenderItems.emplace_hint(
                                renderItemsEmplaceHint, layer, source, static_cast<uint32_t>(index));
                            updateList[index] = true;
                        }
                    }
                }
                continue;
            }

            // Handle layers without source.
            if (layerIsVisible && zoomFitsLayer && sourceImpl.get() == sourceImpls->at(0).get()) {
                // The "background-as-color" optimisation skips RenderBackgroundLayer's
                // update() and just sets the framebuffer clear colour to the background
                // colour. That's fine for the main pass, but the Phase 2 drape pass
                // needs an actual drawable to route into the drape RenderTargets — there's
                // no path for "make this offscreen target's clear colour the layer's
                // colour". When terrain is active, force the slow path so update() runs
                // and emits drape drawables.
                const bool terrainActive = renderTerrain && renderTerrain->isEnabled();
                if (layer.baseImpl == layerImpls->front()) {
                    // Populate the clear colour from the front background
                    // layer UNCONDITIONALLY — with a shared context
                    // (React Native) backgroundLayerAsColor is false and the
                    // old gating left the clear at default black, which is
                    // the backdrop every content gap flashed through.
                    const auto& solidBackground = layer.getSolidBackground();
                    if (solidBackground) {
                        renderTreeParameters->backgroundColor = *solidBackground;
                        lastSolidBackgroundColor = *solidBackground;
                        if (backgroundLayerAsColor && !terrainActive) {
                            continue; // This layer is shown with background color,
                                      // and it shall not be added to render items.
                        }
                    }
                }
                renderItemsEmplaceHint = layerRenderItems.emplace_hint(
                    renderItemsEmplaceHint, layer, nullptr, static_cast<uint32_t>(index));
                updateList[index] = true;
            }
        }
        // Mark the active DEM source as needed for terrain rendering and pass
        // that ownership into the source update. RasterDEM is also valid as a
        // flat hillshade/color-relief source, which must not use the terrain
        // mesh's elevation-expanded cover.
        const bool sourceIsTerrainDEM = renderTerrain && renderTerrain->isEnabled() &&
                                        sourceImpl->id == renderTerrain->getSourceID();
        if (sourceIsTerrainDEM) {
            sourceNeedsRendering = true;
        }

        tileParameters.usedByTerrain = sourceIsTerrainDEM;
        tileParameters.isUpdateSynchronous = sourceImpl->isUpdateSynchronous();
        source->update(sourceImpl, filteredLayersForSource, sourceNeedsRendering, sourceNeedsRelayout, tileParameters);
        filteredLayersForSource.clear();

        addChanges(changes);
    }

    // Update all layers with their new renderability status, if it changed.
    //
    // This sync MUST run after ALL sources have iterated (2026-07-09,
    // traska.26): updateList entries are only set true during the owning
    // source's iteration, so running the sync inside the source loop marked
    // every layer of a later-iterated source non-renderable during each
    // earlier source's pass and renderable again during its own — a
    // remove+add layer-group churn pair for nearly every layer, every
    // frame (~9/s per layer measured on the traska.25 diag; the 2D "black
    // flash" hunt). The pairs usually cancel within processChanges, but
    // any consumer of the intermediate state renders a frame without the
    // affected layer groups — for land-covering layers that is a black
    // flash of the whole viewport.
    {
        UniqueChangeRequestVec changes;
        for (size_t i = 0; i < updateList.size(); i++) {
            if (orderedLayers[i].get().isLayerRenderable() != updateList[i]) {
                // KLATTRA diagnostics (2D black-flash hunt): after the hoist,
                // any transition here is a REAL renderability change — rare
                // enough to log each with its reason tuple. Promoted to
                // Warning 2026-07-11 so DEVICE syslog shows it (stderr is
                // sim-only). Opt out: KLATTRA_LOG_LAYERFLAP=0.
                static const bool layerFlapLog = [] {
                    const char* v = std::getenv("KLATTRA_LOG_LAYERFLAP");
                    return v && !(*v == '0' || *v == 'f' || *v == 'F');
                }();
                if (layerFlapLog) {
                    RenderLayer& flapped = orderedLayers[i].get();
                    const bool vis = flapped.baseImpl->visibility != style::VisibilityType::None;
                    const bool zoomFits = flapped.supportsZoom(zoomHistory.lastZoom);
                    Log::Warning(Event::Render,
                                 "[KLATTRA LAYERFLAP] layer=" + flapped.getID() +
                                     " renderable=" + (updateList[i] ? std::string("1") : std::string("0")) +
                                     " visible=" + (vis ? std::string("1") : std::string("0")) +
                                     " zoomFits=" + (zoomFits ? std::string("1") : std::string("0")) +
                                     " lastZoom=" + std::to_string(zoomHistory.lastZoom) +
                                     " source=" + flapped.baseImpl->source);
                    static const bool klattraTrace = std::getenv("KLATTRA_TRACE_STDERR") != nullptr;
                    if (klattraTrace) {
                        fprintf(stderr,
                                "[KLATTRA_TRACE] [KLATTRA LAYERFLAP] layer=%s renderable=%d visible=%d zoomFits=%d "
                                "lastZoom=%.2f source=%s\n",
                                flapped.getID().c_str(),
                                updateList[i] ? 1 : 0,
                                vis ? 1 : 0,
                                zoomFits ? 1 : 0,
                                zoomHistory.lastZoom,
                                flapped.baseImpl->source.c_str());
                    }
                }
                orderedLayers[i].get().markLayerRenderable(updateList[i], changes);
            }
        }
        addChanges(changes);
    }

    // KLATTRA (2D transition flash, the "black" half): the framebuffer clear
    // colour comes from renderTreeParameters->backgroundColor, which is only
    // assigned when the background-as-color branch above runs — first layer
    // is a solid background, style loaded, first source iterating, terrain
    // off. Any frame that misses the branch (style mid-load at boot, a
    // background transition, mutation instants) cleared to default BLACK,
    // turning every content gap into a black flash on a paper-beige map
    // (device recording 2026-07-10: water/labels painted over pitch-black
    // land at launch, style background #F4EAD0). Reuse the last known solid
    // background instead.
    if (renderTreeParameters->backgroundColor == Color() && lastSolidBackgroundColor) {
        renderTreeParameters->backgroundColor = *lastSolidBackgroundColor;
    }

    // Enable 3D mode if terrain is present
    if (renderTerrain && renderTerrain->isEnabled()) {
        renderTreeParameters->has3D = true;
    }

    renderTreeParameters->loaded = updateParameters->styleLoaded && isLoaded();
    if (!isMapModeContinuous && !renderTreeParameters->loaded) {
        return nullptr;
    }

    // Prepare. Update all matrices and generate data that we should upload to the GPU.
    for (const auto& [name, renderSource] : renderSources) {
        MLN_TRACE_ZONE(prepare source);
        if (renderSource->isEnabled()) {
            renderSource->prepare({.transform = renderTreeParameters->transformParams,
                                   .debugOptions = updateParameters->debugOptions,
                                   .imageManager = *imageManager,
                                   .sourceName = name});
        }
    }

    auto opaquePassCutOffEstimation = layerRenderItems.size();
    for (auto& renderItem : layerRenderItems) {
        RenderLayer& renderLayer = renderItem.layer;
        MLN_TRACE_ZONE(prepare layer);
        MLN_ZONE_STR(renderLayer.getID());

        renderLayer.prepare({.source = renderItem.source,
                             .imageManager = *imageManager,
                             .patternAtlas = *patternAtlas,
                             .lineAtlas = *lineAtlas,
                             .state = updateParameters->transformState});
        if (renderLayer.needsPlacement()) {
            layersNeedPlacement.emplace_back(renderLayer);
        }
        if (renderTreeParameters->opaquePassCutOff == 0) {
            --opaquePassCutOffEstimation;
            if (renderLayer.is3D()) {
                renderTreeParameters->opaquePassCutOff = uint32_t(opaquePassCutOffEstimation);
            }
        }
    }

    // Symbol placement.
    assert((updateParameters->mode == MapMode::Tile) || !placedSymbolDataCollected);
    bool symbolBucketsChanged = false;
    bool symbolBucketsAdded = false;
    std::set<std::string> usedSymbolLayers;
    const auto longitude = static_cast<float>(updateParameters->transformState.getLatLng().longitude());
    for (auto it = layersNeedPlacement.crbegin(); it != layersNeedPlacement.crend(); ++it) {
        MLN_TRACE_ZONE(placement layer);
        RenderLayer& layer = *it;
        auto result = crossTileSymbolIndex.addLayer(layer, longitude);
        if (isMapModeContinuous) {
            usedSymbolLayers.insert(layer.getID());
            symbolBucketsAdded = symbolBucketsAdded || (result & CrossTileSymbolIndex::AddLayerResult::BucketsAdded);
            symbolBucketsChanged = symbolBucketsChanged || (result != CrossTileSymbolIndex::AddLayerResult::NoChanges);
        }
    }

    if (isMapModeContinuous) {
        MLN_TRACE_ZONE(placement);

        std::optional<Duration> placementUpdatePeriodOverride;
        if (symbolBucketsAdded && !tiltedView) {
            // If the view is not tilted, we want *the new* symbols to show up
            // faster, however simple setting `placementChanged` to `true` would
            // initiate placement too often as new buckets usually come from
            // several rendered tiles in a row within a short period of time.
            // Instead, we squeeze placement update period to coalesce buckets
            // updates from several tiles. On contrary, with the tilted view
            // it's more important to make placement rarely for performance
            // reasons and as the new symbols are normally "far away" and the
            // user is not that interested to see them ASAP.
            placementUpdatePeriodOverride = std::optional<Duration>(Milliseconds(30));
        }

        renderTreeParameters->placementChanged = !placementController.placementIsRecent(
            updateParameters->timePoint,
            static_cast<float>(updateParameters->transformState.getZoom()),
            placementUpdatePeriodOverride);
        symbolBucketsChanged |= renderTreeParameters->placementChanged;
        if (renderTreeParameters->placementChanged) {
            Mutable<Placement> placement = Placement::create(updateParameters, placementController.getPlacement());
            placement->placeLayers(layersNeedPlacement);
            placementController.setPlacement(std::move(placement));
            crossTileSymbolIndex.pruneUnusedLayers(usedSymbolLayers);
            for (const auto& entry : renderSources) {
                entry.second->updateFadingTiles();
            }
        } else {
            placementController.setPlacementStale();
        }
        renderTreeParameters->symbolFadeChange = placementController.getPlacement()->symbolFadeChange(
            updateParameters->timePoint);
        renderTreeParameters->needsRepaint = hasTransitions(updateParameters->timePoint);
        // Drape bakes, cap-deferred resizes, and first-bake texture waits
        // only progress on rendered frames. Without this, a static camera
        // (paused flyover) freezes the drape backlog on screen — the holes
        // stayed until an external event (screenshot flash) forced frames
        // (2026-07-04 device finding). The flag self-extinguishes once the
        // backlog drains, so an idle map still idles.
        if (!renderTreeParameters->needsRepaint && renderTerrain && renderTerrain->isEnabled() &&
            renderTerrain->hasPendingDrapeWork()) {
            renderTreeParameters->needsRepaint = true;
        }
    } else {
        MLN_TRACE_ZONE(placement);

        renderTreeParameters->placementChanged = symbolBucketsChanged = !layersNeedPlacement.empty();
        if (renderTreeParameters->placementChanged) {
            Mutable<Placement> placement = Placement::create(updateParameters);
            placement->collectPlacedSymbolData(placedSymbolDataCollected);
            placement->placeLayers(layersNeedPlacement);
            placementController.setPlacement(std::move(placement));
        }
        crossTileSymbolIndex.reset();
        renderTreeParameters->symbolFadeChange = 1.0f;
        renderTreeParameters->needsRepaint = false;
    }

    if (!renderTreeParameters->needsRepaint && renderTreeParameters->loaded) {
        MLN_TRACE_ZONE(reduce);
        // Notify observer about unused images when map is fully loaded
        // and there are no ongoing transitions.
        imageManager->reduceMemoryUseIfCacheSizeExceedsLimit();
    }

    std::vector<std::unique_ptr<RenderItem>> sourceRenderItems;
    for (const auto& entry : renderSources) {
        if (entry.second->isEnabled()) {
            sourceRenderItems.emplace_back(entry.second->createRenderItem());
        }
    }

    return std::make_unique<RenderTreeImpl>(std::move(renderTreeParameters),
                                            std::move(layerRenderItems),
                                            std::move(sourceRenderItems),
                                            *lineAtlas,
                                            *patternAtlas,
                                            std::move(layersNeedPlacement),
                                            placementController.getPlacement(),
                                            symbolBucketsChanged,
                                            startTime);
}

std::vector<Feature> RenderOrchestrator::queryRenderedFeatures(const ScreenLineString& geometry,
                                                               const RenderedQueryOptions& options) const {
    MLN_TRACE_FUNC();

    std::unordered_map<std::string, const RenderLayer*> layers;
    if (options.layerIDs) {
        for (const auto& layerID : *options.layerIDs) {
            if (const RenderLayer* layer = getRenderLayer(layerID)) {
                layers.emplace(layer->getID(), layer);
            }
        }
    } else {
        for (const auto& entry : renderLayers) {
            layers.emplace(entry.second->getID(), entry.second.get());
        }
    }

    return queryRenderedFeatures(geometry, options, layers);
}

void RenderOrchestrator::queryRenderedSymbols(std::unordered_map<std::string, std::vector<Feature>>& resultsByLayer,
                                              const ScreenLineString& geometry,
                                              const std::unordered_map<std::string, const RenderLayer*>& layers,
                                              const RenderedQueryOptions& options) const {
    MLN_TRACE_FUNC();

    const auto hasCrossTileIndex = [](const auto& pair) {
        return pair.second->baseImpl->getTypeInfo()->crossTileIndex == style::LayerTypeInfo::CrossTileIndex::Required;
    };

    std::unordered_map<std::string, const RenderLayer*> crossTileSymbolIndexLayers;
    std::ranges::copy_if(
        layers, std::inserter(crossTileSymbolIndexLayers, crossTileSymbolIndexLayers.begin()), hasCrossTileIndex);

    if (crossTileSymbolIndexLayers.empty()) {
        return;
    }
    const Placement& placement = *placementController.getPlacement();
    auto renderedSymbols = placement.getCollisionIndex().queryRenderedSymbols(geometry);
    std::vector<std::reference_wrapper<const RetainedQueryData>> bucketQueryData;
    bucketQueryData.reserve(renderedSymbols.size());
    for (const auto& entry : renderedSymbols) {
        bucketQueryData.emplace_back(placement.getQueryData(entry.first));
    }
    // Although symbol query is global, symbol results are only sortable within
    // a bucket For a predictable global sort renderItems, we sort the buckets
    // based on their corresponding tile position
    std::ranges::sort(bucketQueryData, [](const RetainedQueryData& a, const RetainedQueryData& b) {
        return std::tie(a.tileID.canonical.z, a.tileID.canonical.y, a.tileID.wrap, a.tileID.canonical.x) <
               std::tie(b.tileID.canonical.z, b.tileID.canonical.y, b.tileID.wrap, b.tileID.canonical.x);
    });

    for (auto wrappedQueryData : bucketQueryData) {
        auto& queryData = wrappedQueryData.get();
        auto bucketSymbols = queryData.featureIndex->lookupSymbolFeatures(renderedSymbols[queryData.bucketInstanceId],
                                                                          options,
                                                                          crossTileSymbolIndexLayers,
                                                                          queryData.tileID,
                                                                          queryData.featureSortOrder);

        for (auto layer : bucketSymbols) {
            auto& resultFeatures = resultsByLayer[layer.first];
            std::ranges::move(layer.second, std::inserter(resultFeatures, resultFeatures.end()));
        }
    }
}

std::vector<Feature> RenderOrchestrator::queryRenderedFeatures(
    const ScreenLineString& geometry,
    const RenderedQueryOptions& options,
    const std::unordered_map<std::string, const RenderLayer*>& layers) const {
    MLN_TRACE_FUNC();

    std::unordered_set<std::string> sourceIDs;
    std::unordered_map<std::string, const RenderLayer*> filteredLayers;
    for (const auto& pair : layers) {
        if (!pair.second->needsRendering() || !pair.second->supportsZoom(zoomHistory.lastZoom)) {
            continue;
        }
        filteredLayers.emplace(pair);
        sourceIDs.emplace(pair.second->baseImpl->source);
    }

    mat4 projMatrix;
    transformState.getProjMatrix(projMatrix);

    std::unordered_map<std::string, std::vector<Feature>> resultsByLayer;
    for (const auto& sourceID : sourceIDs) {
        if (RenderSource* renderSource = getRenderSource(sourceID)) {
            auto sourceResults = renderSource->queryRenderedFeatures(
                geometry, transformState, filteredLayers, options, projMatrix);
            std::ranges::move(sourceResults, std::inserter(resultsByLayer, resultsByLayer.begin()));
        }
    }

    queryRenderedSymbols(resultsByLayer, geometry, filteredLayers, options);

    mbgl::DynamicFeatureIndex dynamicIndex;
    for (const auto& pair : filteredLayers) {
        const RenderLayer* layer = pair.second;
        layer->populateDynamicRenderFeatureIndex(dynamicIndex);
    }
    dynamicIndex.query(resultsByLayer, geometry, transformState);
    std::vector<Feature> result;

    if (resultsByLayer.empty()) {
        return result;
    }

    // Combine all results based on the style layer renderItems.
    for (const auto& pair : filteredLayers) {
        auto it = resultsByLayer.find(pair.second->baseImpl->id);
        if (it != resultsByLayer.end()) {
            std::move(it->second.begin(), it->second.end(), std::back_inserter(result));
        }
    }

    return result;
}

std::vector<Feature> RenderOrchestrator::queryShapeAnnotations(const ScreenLineString& geometry) const {
    MLN_TRACE_FUNC();

    assert(LayerManager::annotationsEnabled);
    std::unordered_map<std::string, const RenderLayer*> shapeAnnotationLayers;
    RenderedQueryOptions options;
    for (const auto& layerImpl : *layerImpls) {
        if (std::mismatch(layerImpl->id.begin(),
                          layerImpl->id.end(),
                          AnnotationManager::ShapeLayerID.begin(),
                          AnnotationManager::ShapeLayerID.end())
                .second == AnnotationManager::ShapeLayerID.end()) {
            if (const RenderLayer* layer = getRenderLayer(layerImpl->id)) {
                shapeAnnotationLayers.emplace(layer->getID(), layer);
            }
        }
    }

    return queryRenderedFeatures(geometry, options, shapeAnnotationLayers);
}

std::vector<Feature> RenderOrchestrator::querySourceFeatures(const std::string& sourceID,
                                                             const SourceQueryOptions& options) const {
    MLN_TRACE_FUNC();

    const RenderSource* source = getRenderSource(sourceID);
    if (!source) return {};

    return source->querySourceFeatures(options);
}

FeatureExtensionValue RenderOrchestrator::queryFeatureExtensions(
    const std::string& sourceID,
    const Feature& feature,
    const std::string& extension,
    const std::string& extensionField,
    const std::optional<std::map<std::string, Value>>& args) const {
    MLN_TRACE_FUNC();

    if (RenderSource* renderSource = getRenderSource(sourceID)) {
        return renderSource->queryFeatureExtensions(feature, extension, extensionField, args);
    }
    return {};
}

void RenderOrchestrator::setFeatureState(const std::string& sourceID,
                                         const std::optional<std::string>& sourceLayerID,
                                         const std::string& featureID,
                                         const FeatureState& state) {
    MLN_TRACE_FUNC();

    if (RenderSource* renderSource = getRenderSource(sourceID)) {
        renderSource->setFeatureState(sourceLayerID, featureID, state);
    }
}

void RenderOrchestrator::getFeatureState(FeatureState& state,
                                         const std::string& sourceID,
                                         const std::optional<std::string>& sourceLayerID,
                                         const std::string& featureID) const {
    MLN_TRACE_FUNC();

    if (RenderSource* renderSource = getRenderSource(sourceID)) {
        renderSource->getFeatureState(state, sourceLayerID, featureID);
    }
}

void RenderOrchestrator::removeFeatureState(const std::string& sourceID,
                                            const std::optional<std::string>& sourceLayerID,
                                            const std::optional<std::string>& featureID,
                                            const std::optional<std::string>& stateKey) {
    MLN_TRACE_FUNC();

    if (RenderSource* renderSource = getRenderSource(sourceID)) {
        renderSource->removeFeatureState(sourceLayerID, featureID, stateKey);
    }
}

void RenderOrchestrator::setTileCacheEnabled(bool enable) {
    tileCacheEnabled = enable;

    for (const auto& entry : renderSources) {
        entry.second->setCacheEnabled(enable);
    }
}

bool RenderOrchestrator::getTileCacheEnabled() const {
    return tileCacheEnabled;
}

void RenderOrchestrator::reduceMemoryUse() {
    reduceMemoryUse(false);
}

void RenderOrchestrator::reduceMemoryUseForMemoryPressure() {
    reduceMemoryUse(true);
}

void RenderOrchestrator::reduceMemoryUse(bool memoryPressure) {
    MLN_TRACE_FUNC();

    filteredLayersForSource.shrink_to_fit();
    for (const auto& entry : renderSources) {
        if (memoryPressure) {
            entry.second->reduceMemoryUseForMemoryPressure();
        } else {
            entry.second->reduceMemoryUse();
        }
    }
    imageManager->reduceMemoryUse();
    if (renderTerrain) {
        UniqueChangeRequestVec changes;
        renderTerrain->reduceMemoryUse(changes);
        addChanges(changes);
    }
    observer->onInvalidate();
}

void RenderOrchestrator::dumpDebugLogs() {
    MLN_TRACE_FUNC();

    for (const auto& entry : renderSources) {
        entry.second->dumpDebugLogs();
    }

    imageManager->dumpDebugLogs();
}

void RenderOrchestrator::collectPlacedSymbolData(bool enable) {
    placedSymbolDataCollected = enable;
}

const std::vector<PlacedSymbolData>& RenderOrchestrator::getPlacedSymbolsData() const {
    return placementController.getPlacement()->getPlacedSymbolsData();
}

RenderLayer* RenderOrchestrator::getRenderLayer(const std::string& id) {
    auto it = renderLayers.find(id);
    return it != renderLayers.end() ? it->second.get() : nullptr;
}

const RenderLayer* RenderOrchestrator::getRenderLayer(const std::string& id) const {
    auto it = renderLayers.find(id);
    return it != renderLayers.end() ? it->second.get() : nullptr;
}

RenderSource* RenderOrchestrator::getRenderSource(const std::string& id) const {
    auto it = renderSources.find(id);
    return it != renderSources.end() ? it->second.get() : nullptr;
}

bool RenderOrchestrator::hasTransitions(TimePoint timePoint) const {
    if (renderLight.hasTransition()) {
        return true;
    }

    for (const auto& entry : renderLayers) {
        if (entry.second->hasTransition()) {
            return true;
        }
    }

    if (placementController.hasTransitions(timePoint)) {
        return true;
    }

    for (const auto& entry : renderSources) {
        if (entry.second->hasFadingTiles()) {
            return true;
        }
    }

    return false;
}

bool RenderOrchestrator::isLoaded() const {
    MLN_TRACE_FUNC();

    // do the simple boolean check before iterating over all the tiles in all the sources
    if (!imageManager->isLoaded()) {
        return false;
    }

    for (const auto& entry : renderSources) {
        if (!entry.second->isLoaded()) {
            return false;
        }
    }

    return true;
}

bool RenderOrchestrator::hillshadeBakesPending() const {
    MLN_TRACE_FUNC();

    // A target that has not completed its first bake samples as neutral
    // (or carried) shading this frame — the relief is still settling.
    for (const auto& renderTarget : renderTargets) {
        if (renderTarget && !renderTarget->hasCompletedRender()) {
            return true;
        }
    }

    // A hillshade layer whose source is still fetching its cover will keep
    // creating fresh prepare targets as tiles land.
    for (const auto& entry : renderLayers) {
        const auto& layer = entry.second;
        if (!layer || layer->baseImpl->getTypeInfo() != style::HillshadeLayer::Impl::staticTypeInfo()) {
            continue;
        }
        const auto source = renderSources.find(layer->baseImpl->source);
        if (source != renderSources.end() && source->second->isEnabled() && !source->second->isLoaded()) {
            return true;
        }
    }

    return false;
}

void RenderOrchestrator::clearData() {
    MLN_TRACE_FUNC();

    if (!sourceImpls->empty()) sourceImpls = makeMutable<std::vector<Immutable<style::Source::Impl>>>();
    if (!layerImpls->empty()) layerImpls = makeMutable<std::vector<Immutable<style::Layer::Impl>>>();
    if (!imageImpls->empty()) imageImpls = makeMutable<std::vector<Immutable<style::Image::Impl>>>();

    UniqueChangeRequestVec changes;
    for (const auto& entry : renderLayers) {
        entry.second->layerRemoved(changes);
    }
    addChanges(changes);

    debugLayerGroups.clear();

    renderSources.clear();
    renderLayers.clear();

    crossTileSymbolIndex.reset();

    if (!lineAtlas->isEmpty()) lineAtlas = std::make_unique<LineAtlas>();
    if (!patternAtlas->isEmpty()) patternAtlas = std::make_unique<PatternAtlas>();

    imageManager->clear();
    glyphManager->evict(fontStacks(*layerImpls));
}

void RenderOrchestrator::addChanges(UniqueChangeRequestVec& changes) {
    pendingChanges.insert(
        pendingChanges.end(), std::make_move_iterator(changes.begin()), std::make_move_iterator(changes.end()));
    changes.clear();
}

void RenderOrchestrator::updateLayerIndex(LayerGroupBasePtr layerGroup, const int32_t newIndex) {
    MLN_TRACE_FUNC();

    if (!layerGroup || layerGroup->getLayerIndex() == newIndex) {
        return;
    }
    const auto range = layerGroupsByLayerIndex.equal_range(layerGroup->getLayerIndex());
    for (auto it = range.first; it != range.second; ++it) {
        if (it->second == layerGroup) {
            layerGroupsByLayerIndex.erase(it);
            layerGroup->updateLayerIndex(newIndex);
            layerGroupsByLayerIndex.insert(std::make_pair(newIndex, std::move(layerGroup)));
            return;
        }
    }
    // We're not tracking the layer, indicating that it's currently disabled, so update it directly.
    layerGroup->updateLayerIndex(newIndex);
}

bool RenderOrchestrator::addLayerGroup(LayerGroupBasePtr layerGroup) {
    MLN_TRACE_FUNC();

    const auto index = layerGroup->getLayerIndex();
    const auto range = layerGroupsByLayerIndex.equal_range(index);
    bool found = false;
    for (auto it = range.first; it != range.second; ++it) {
        if (it->second == layerGroup) {
            assert(layerGroup->getLayerIndex() == it->first);
            found = true;
            // not added
            break;
        }
    }
    if (found) {
        return false; // not added
    } else {
        layerGroupsByLayerIndex.insert(std::make_pair(index, std::move(layerGroup)));
        return true; // added
    }
}

bool RenderOrchestrator::removeLayerGroup(const LayerGroupBasePtr& layerGroup) {
    MLN_TRACE_FUNC();

    if (!layerGroup) {
        return false;
    }
    const auto range = layerGroupsByLayerIndex.equal_range(layerGroup->getLayerIndex());
    for (auto it = range.first; it != range.second; ++it) {
        if (it->second == layerGroup) {
            layerGroupsByLayerIndex.erase(it);
            return true;
        }
    }
    return false;
}

size_t RenderOrchestrator::numLayerGroups() const noexcept {
    return layerGroupsByLayerIndex.size();
}

void RenderOrchestrator::updateLayers(gfx::ShaderRegistry& shaders,
                                      gfx::Context& context,
                                      const TransformState& state,
                                      const std::shared_ptr<UpdateParameters>& updateParameters,
                                      const RenderTree& renderTree) {
    MLN_TRACE_FUNC();

    const bool isMapModeContinuous = updateParameters->mode == MapMode::Continuous;
    const auto transitionOptions = isMapModeContinuous ? updateParameters->transitionOptions
                                                       : style::TransitionOptions();
    const auto defDuration = isMapModeContinuous ? util::DEFAULT_TRANSITION_DURATION : Duration::zero();
    const PropertyEvaluationParameters evalParameters{
        getZoomHistory(),
        updateParameters->timePoint,
        transitionOptions.duration.value_or(defDuration),
    };

    const auto& items = renderTree.getLayerRenderItemMap();

    std::vector<std::unique_ptr<ChangeRequest>> changes;
    changes.reserve(items.size() * 3);

    // Phase 2 scaffolding: run terrain.update() BEFORE the per-layer loop
    // so the drape cache is populated by the time each layer's update()
    // runs and tries to look up its tile's drape target via
    // `activeTerrain->getDrapeTarget(tileID)`. Drawable order within the
    // frame doesn't matter — the GPU passes render drape targets first
    // (filling the per-tile textures) then the main framebuffer (where
    // the terrain mesh samples those textures), regardless of the order
    // in which the CPU-side drawables were created.
    if (renderTerrain && renderTerrain->isEnabled()) {
        renderTerrain->update(*this, shaders, context, state, updateParameters, renderTree, changes);
    }

    RenderTerrain* const activeTerrain = (renderTerrain && renderTerrain->isEnabled())
                                             ? renderTerrain.get()
                                             : nullptr;

    for (const auto& item : items) {
        auto& renderLayer = item.layer.get();
#if MLN_RENDER_BACKEND_OPENGL
        // Android Emulator: Goldfish is *very* broken. This will prevent a crash
        // inside the GL translation layer at the cost of emulator performance.
        if (androidGoldfishMitigationEnabled) {
            renderLayer.removeAllDrawables();
        }
#endif
        // Give the layer access to RenderTerrain only for the duration of
        // its update() call. Cleared afterward so any out-of-band caller
        // can't accidentally use a stale pointer.
        renderLayer.activeTerrain = activeTerrain;
        try {
            renderLayer.update(shaders, context, state, updateParameters, renderTree, changes);
        } catch (...) {
            observer->onRenderError(std::current_exception());
        }
        renderLayer.activeTerrain = nullptr;
    }

    addChanges(changes);
}

void RenderOrchestrator::processChanges() {
    auto localChanges = std::move(pendingChanges);
    for (auto& change : localChanges) {
        change->execute(*this);
    }
}

bool RenderOrchestrator::addRenderTarget(RenderTargetPtr renderTarget, bool atFront) {
    auto it = std::ranges::find(renderTargets, renderTarget);
    if (it == renderTargets.end()) {
        // atFront ensures hillshade-prepare RenderTargets render before
        // terrain drape RenderTargets that sample from them. Without this,
        // the drape pass would sample an undefined offscreen colour buffer
        // on the very first frame after a tile becomes visible (one frame
        // of black mesh), and single-frame headless renders would produce
        // entirely black output.
        if (atFront) {
            renderTargets.emplace(renderTargets.begin(), std::move(renderTarget));
        } else {
            renderTargets.emplace_back(std::move(renderTarget));
        }
        return true;
    } else {
        return false;
    }
}

bool RenderOrchestrator::removeRenderTarget(const RenderTargetPtr& renderTarget) {
    auto it = std::ranges::find(renderTargets, renderTarget);
    if (it != renderTargets.end()) {
        renderTargets.erase(it);
        return true;
    } else {
        return false;
    }
}

void RenderOrchestrator::updateDebugLayerGroups(const RenderTree& renderTree, PaintParameters& parameters) {
    for (const RenderItem& item : renderTree.getSourceRenderItems()) {
        item.updateDebugDrawables(debugLayerGroups, parameters);
    }
}

void RenderOrchestrator::onGlyphsLoaded(const FontStack& fontStack, const GlyphRange& range) {
    observer->onGlyphsLoaded(fontStack, range);
}

void RenderOrchestrator::onGlyphsError(const FontStack& fontStack,
                                       const GlyphRange& glyphRange,
                                       std::exception_ptr error) {
    MLN_TRACE_FUNC();

    std::stringstream ss;
    ss << "Failed to load glyph range ";
    if (glyphRange.type == FontPBF) {
        ss << glyphRange.first << "-" << glyphRange.second;
    } else {
        ss << (int)glyphRange.type << "(font file)";
    }
    ss << " for font stack " << fontStackToString(fontStack) << ":( " << util::toString(error) << ")";
    auto errorDetail = ss.str();
    Log::Error(Event::Style, errorDetail);
    observer->onResourceError(error);
}

void RenderOrchestrator::onGlyphsRequested(const FontStack& fontStack, const GlyphRange& range) {
    observer->onGlyphsRequested(fontStack, range);
}

void RenderOrchestrator::onTileError(RenderSource& source, const OverscaledTileID& tileID, std::exception_ptr error) {
    MLN_TRACE_FUNC();

    Log::Error(Event::Style,
               "Failed to load tile " + util::toString(tileID) + " for source " + source.baseImpl->id + ": " +
                   util::toString(error));
    observer->onResourceError(error);
}

void RenderOrchestrator::onTileChanged(RenderSource&, const OverscaledTileID&) {
    MLN_TRACE_FUNC();

    // KLATTRA diagnostics (2D black-flash hunt): tile completions must wake
    // the renderer; the stuck-black state holds a stale frame while loaded
    // tiles sit unpainted. Throttled 1 Hz counter at Warning (device
    // syslog) — its absence while tiles load, or presence while the screen
    // stays stale, splits the broken link. Opt out: KLATTRA_LOG_WAKE=0.
    static const bool wakeLog = [] {
        const char* v = std::getenv("KLATTRA_LOG_WAKE");
        return v && !(*v == '0' || *v == 'f' || *v == 'F');
    }();
    if (wakeLog) {
        static std::atomic<uint64_t> count{0};
        static std::chrono::steady_clock::time_point lastLog{};
        const auto n = ++count;
        const auto now = std::chrono::steady_clock::now();
        if (now - lastLog >= std::chrono::seconds(1)) {
            lastLog = now;
            Log::Warning(Event::Render, "[KLATTRA WAKE] onTileChanged total=" + std::to_string(n));
        }
    }

    observer->onInvalidate();
}

void RenderOrchestrator::onTileAction(RenderSource&,
                                      TileOperation op,
                                      const OverscaledTileID& id,
                                      const std::string& sourceID) {
    observer->onTileAction(op, id, sourceID);
}

void RenderOrchestrator::onStyleImageMissing(const std::string& id, const std::function<void()>& done) {
    MLN_TRACE_FUNC();

    observer->onStyleImageMissing(id, done);
}

void RenderOrchestrator::onRemoveUnusedStyleImages(const std::vector<std::string>& unusedImageIDs) {
    MLN_TRACE_FUNC();

    observer->onRemoveUnusedStyleImages(unusedImageIDs);
}

} // namespace mbgl
