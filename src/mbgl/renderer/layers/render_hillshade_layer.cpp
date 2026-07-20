#include <mbgl/renderer/layers/render_hillshade_layer.hpp>

#include <algorithm>
#include <mbgl/renderer/buckets/hillshade_bucket.hpp>
#include <mbgl/renderer/render_tile.hpp>
#include <mbgl/renderer/sources/render_raster_dem_source.hpp>
#include <mbgl/renderer/paint_parameters.hpp>
#include <mbgl/renderer/render_static_data.hpp>
#include <mbgl/tile/tile.hpp>
#include <mbgl/style/layers/hillshade_layer_impl.hpp>
#include <mbgl/gfx/cull_face_mode.hpp>
#include <mbgl/gfx/offscreen_texture.hpp>
#include <mbgl/gfx/render_pass.hpp>
#include <mbgl/math/angles.hpp>
#include <mbgl/util/geo.hpp>

#include <mbgl/renderer/layers/hillshade_layer_tweaker.hpp>
#include <mbgl/renderer/layers/hillshade_prepare_layer_tweaker.hpp>
#include <mbgl/renderer/layer_group.hpp>
#include <mbgl/renderer/render_target.hpp>
#include <mbgl/renderer/render_terrain.hpp>
#include <mbgl/renderer/update_parameters.hpp>
#include <mbgl/shaders/shader_program_base.hpp>
#include <mbgl/shaders/hillshade_layer_ubo.hpp>
#include <mbgl/gfx/drawable_builder.hpp>
#include <mbgl/gfx/drawable_impl.hpp>
#include <mbgl/gfx/hillshade_prepare_drawable_data.hpp>

#include <cstring>
#include <mbgl/gfx/shader_group.hpp>
#include <mbgl/gfx/shader_registry.hpp>

#include <cstdlib>
#include <unordered_set>

namespace mbgl {

using namespace style;
using namespace shaders;

namespace {

inline const HillshadeLayer::Impl& impl_cast(const Immutable<style::Layer::Impl>& impl) {
    assert(impl->getTypeInfo() == HillshadeLayer::Impl::staticTypeInfo());
    return static_cast<const HillshadeLayer::Impl&>(*impl);
}

bool klattraLogDrapeStale() {
    return std::getenv("KLATTRA_LOG_DRAPE_STALE") != nullptr;
}

bool klattraDisableHillshadeDrape() {
    static const bool disabled = std::getenv("KLATTRA_DISABLE_HILLSHADE_DRAPE") != nullptr;
    return disabled;
}

bool klattraHillshadeDrapeSameZoomOnly() {
    static const bool enabled = std::getenv("KLATTRA_HILLSHADE_DRAPE_SAME_ZOOM") != nullptr;
    return enabled;
}

bool klattraDisableBakeCarry() {
    static const bool disabled = std::getenv("KLATTRA_DISABLE_BAKE_CARRY") != nullptr;
    return disabled;
}

std::string klattraDrapeIDString(const OverscaledTileID& id) {
    return "z" + std::to_string(static_cast<int>(id.canonical.z)) + "/" +
           std::to_string(id.canonical.x) + "/" + std::to_string(id.canonical.y);
}

} // namespace

RenderHillshadeLayer::RenderHillshadeLayer(Immutable<style::HillshadeLayer::Impl> _impl)
    : RenderLayer(makeMutable<HillshadeLayerProperties>(std::move(_impl))),
      unevaluated(impl_cast(baseImpl).paint.untransitioned()) {
    styleDependencies = unevaluated.getDependencies();
}

RenderHillshadeLayer::~RenderHillshadeLayer() = default;

std::array<float, 2> RenderHillshadeLayer::getLatRange(const UnwrappedTileID& id) {
    const LatLng latlng0 = LatLng(id);
    const LatLng latlng1 = LatLng(UnwrappedTileID(id.canonical.z, id.canonical.x, id.canonical.y + 1));
    return {{static_cast<float>(latlng0.latitude()), static_cast<float>(latlng1.latitude())}};
}

// Keep old function for backward compatibility during transition
std::array<float, 2> RenderHillshadeLayer::getLight(const PaintParameters& parameters) {
    const auto& evaluated = static_cast<const HillshadeLayerProperties&>(*evaluatedProperties).evaluated;

    // Get first element from vectors (for backward compatibility)
    std::vector<float> directions = evaluated.get<HillshadeIlluminationDirection>();
    float azimuthal = util::deg2radf(directions.empty() ? 335.0f : directions[0]);

    if (evaluated.get<HillshadeIlluminationAnchor>() == HillshadeIlluminationAnchorType::Viewport)
        azimuthal = azimuthal - static_cast<float>(parameters.state.getBearing());
    return {{evaluated.get<HillshadeExaggeration>(), azimuthal}};
}

void RenderHillshadeLayer::transition(const TransitionParameters& parameters) {
    unevaluated = impl_cast(baseImpl).paint.transitioned(parameters, std::move(unevaluated));
    styleDependencies = unevaluated.getDependencies();
}

void RenderHillshadeLayer::layerChanged(const TransitionParameters& parameters,
                                        const Immutable<style::Layer::Impl>& impl,
                                        UniqueChangeRequestVec& changes) {
    RenderLayer::layerChanged(parameters, impl, changes);
    prepareLayerTweaker.reset();
}

void RenderHillshadeLayer::evaluate(const PropertyEvaluationParameters& parameters) {
    const auto previousProperties = staticImmutableCast<HillshadeLayerProperties>(evaluatedProperties);
    auto properties = makeMutable<HillshadeLayerProperties>(
        staticImmutableCast<HillshadeLayer::Impl>(baseImpl),
        unevaluated.evaluate(parameters, previousProperties->evaluated));

    passes = (properties->evaluated.get<style::HillshadeExaggeration>() > 0)
                 ? (RenderPass::Translucent | RenderPass::Pass3D)
                 : RenderPass::None;
    properties->renderPasses = mbgl::underlying_type(passes);
    evaluatedProperties = std::move(properties);
    if (layerTweaker) {
        layerTweaker->updateProperties(evaluatedProperties);
    }
    if (prepareLayerTweaker) {
        prepareLayerTweaker->updateProperties(evaluatedProperties);
    }
    // Mirror to per-drape-target tweakers — same pattern as
    // RenderRasterLayer / RenderFillLayer / RenderLineLayer.
    for (auto& [_, tw] : drapeLayerTweakers) {
        if (tw) tw->updateProperties(evaluatedProperties);
    }
}

bool RenderHillshadeLayer::hasTransition() const {
    return unevaluated.hasTransition();
}

bool RenderHillshadeLayer::hasCrossfade() const {
    return false;
}

void RenderHillshadeLayer::prepare(const LayerPrepareParameters& params) {
    renderTiles = params.source->getRenderTiles();
    maxzoom = params.source->getMaxZoom();

    updateRenderTileIDs();
}

namespace {
void activateRenderTarget(const RenderTargetPtr& renderTarget_, bool activate, UniqueChangeRequestVec& changes) {
    if (renderTarget_) {
        if (activate) {
            // The RenderTree has determined this render target should be included in the renderable set for a frame.
            // atFront ensures the hillshade prepare pass runs before any drape RenderTargets that sample its output.
            changes.emplace_back(std::make_unique<AddRenderTargetRequest>(renderTarget_, /*atFront=*/true));
        } else {
            // The RenderTree is informing us we should not render anything
            changes.emplace_back(std::make_unique<RemoveRenderTargetRequest>(renderTarget_));
        }
    }
}
} // namespace

void RenderHillshadeLayer::markLayerRenderable(bool willRender, UniqueChangeRequestVec& changes) {
    RenderLayer::markLayerRenderable(willRender, changes);
    removeRenderTargets(changes);
}

void RenderHillshadeLayer::layerRemoved(UniqueChangeRequestVec& changes) {
    RenderLayer::layerRemoved(changes);
    removeRenderTargets(changes);
}

void RenderHillshadeLayer::addRenderTarget(const RenderTargetPtr& renderTarget, UniqueChangeRequestVec& changes) {
    activateRenderTarget(renderTarget, true, changes);
    activatedRenderTargets.emplace_back(renderTarget);
}

void RenderHillshadeLayer::removeRenderTargets(UniqueChangeRequestVec& changes) {
    for (const auto& renderTarget : activatedRenderTargets) {
        activateRenderTarget(renderTarget, false, changes);
    }
    activatedRenderTargets.clear();
}

static const std::string HillshadePrepareShaderGroupName = "HillshadePrepareShader";
static const std::string HillshadeShaderGroupName = "HillshadeShader";

void RenderHillshadeLayer::update(gfx::ShaderRegistry& shaders,
                                  gfx::Context& context,
                                  [[maybe_unused]] const TransformState& state,
                                  const std::shared_ptr<UpdateParameters>&,
                                  [[maybe_unused]] const RenderTree& renderTree,
                                  UniqueChangeRequestVec& changes) {
    // Prepare targets are one-shot: each renders its DEM→hillshade pass once,
    // and afterwards only the bucket's texture reference matters. Retire them
    // from the orchestrator's render list as soon as that pass has run.
    // Previously they were removed ONLY on renderability transitions, which
    // (a) leaked every target once the per-frame renderability churn was
    // fixed (traska.26 hoist; 1,066 live targets with textures = the 3.3 GB
    // per-process-limit jetsam on pinch-hold, 2026-07-10), and (b) could
    // remove a target BEFORE its first render when a transition raced the
    // prepare — an undefined texture that draws as black tiles.
    activatedRenderTargets.erase(std::remove_if(activatedRenderTargets.begin(),
                                                activatedRenderTargets.end(),
                                                [&](const RenderTargetPtr& target) {
                                                    if (target && target->hasCompletedRender()) {
                                                        activateRenderTarget(target, false, changes);
                                                        return true;
                                                    }
                                                    return false;
                                                }),
                                 activatedRenderTargets.end());

    if (!renderTiles || renderTiles->empty()) {
        carriedBakeTextures.clear();
        removeAllDrawables();
        return;
    }

    // Set up a layer group
    if (!layerGroup) {
        if (auto layerGroup_ = context.createTileLayerGroup(layerIndex, /*initialCapacity=*/64, getID())) {
            setLayerGroup(std::move(layerGroup_), changes);
        } else {
            return;
        }
    }

    auto* tileLayerGroup = static_cast<TileLayerGroup*>(layerGroup.get());

    if (!layerTweaker) {
        layerTweaker = std::make_shared<HillshadeLayerTweaker>(getID(), evaluatedProperties);
        layerGroup->addLayerTweaker(layerTweaker);
    }

    if (!hillshadePrepareShader) {
        hillshadePrepareShader = context.getGenericShader(shaders, HillshadePrepareShaderGroupName);
    }

    if (!hillshadeShader) {
        hillshadeShader = context.getGenericShader(shaders, HillshadeShaderGroupName);
    }

    if (!hillshadePrepareShader || !hillshadeShader) {
        removeAllDrawables();
        return;
    }

    auto renderPass = RenderPass::Translucent;
    if (!(mbgl::underlying_type(renderPass) & evaluatedProperties->renderPasses)) {
        return;
    }

    stats.drawablesRemoved += tileLayerGroup->removeDrawablesIf(
        [&](gfx::Drawable& drawable) { return drawable.getTileID() && !hasRenderTile(*drawable.getTileID()); });

    // Drop carried bakes for tiles that left the cover.
    for (auto it = carriedBakeTextures.begin(); it != carriedBakeTextures.end();) {
        if (!hasRenderTile(it->first)) {
            it = carriedBakeTextures.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = drapeTileMasks.begin(); it != drapeTileMasks.end();) {
        if (!hasRenderTile(it->first)) {
            it = drapeTileMasks.erase(it);
        } else {
            ++it;
        }
    }

    // When terrain is active, drop main-pass hillshade drawables for tiles
    // with DEM coverage — they would render at z=0 and bleed past the
    // terrain mesh edges. The drape variants below paint hillshade onto
    // the mesh for those tiles. Tiles without DEM coverage keep their
    // main-pass drawables so hillshade doesn't vanish at zooms below the
    // DEM range. See `render_fill_layer.cpp` for the matching rationale.
    if (activeTerrain) {
        stats.drawablesRemoved += tileLayerGroup->removeDrawablesIf([&](gfx::Drawable& drawable) {
            const auto& tileID = drawable.getTileID();
            return tileID && activeTerrain->hasElevationCoverage(*tileID);
        });
    }

    // Mirror the cover-set cleanup for drape groups and prune stale drape
    // tweakers — same pattern as RenderRasterLayer / RenderFillLayer /
    // RenderLineLayer.
    if (activeTerrain) {
        std::unordered_set<OverscaledTileID> liveDrapeIDs;
        activeTerrain->visitDrapeTargets(
            [&](const OverscaledTileID& drapeID, TerrainDrapeTargetPtr& drapeTarget) {
                if (!drapeTarget) return;
                liveDrapeIDs.insert(drapeID);
                if (auto* drapeGroup = static_cast<TileLayerGroup*>(
                        drapeTarget->getLayerGroup(layerIndex).get())) {
                    std::size_t removedNotInCover = 0;
                    std::size_t removedNoOverlap = 0;
                    const auto removed = drapeGroup->removeDrawablesIf(
                        [&](gfx::Drawable& drawable) {
                            const auto& dID = drawable.getTileID();
                            if (!dID) return false;
                            if (!hasRenderTile(*dID)) {
                                removedNotInCover++;
                                return true;
                            }
                            if (!LayerTweaker::tilesOverlap(*dID, drapeID)) {
                                removedNoOverlap++;
                                return true;
                            }
                            return false;
                        });
                    stats.drawablesRemoved += removed;
                    if (removed && klattraLogDrapeStale()) {
                        Log::Info(Event::Render,
                                  "[Klättra DRAPE_STALE] layer=" + getID() +
                                      " kind=hillshade drape=" + klattraDrapeIDString(drapeID) +
                                      " removed=" + std::to_string(removed) +
                                      " not-in-cover=" + std::to_string(removedNotInCover) +
                                      " no-overlap=" + std::to_string(removedNoOverlap));
                    }
                }
            });
        for (auto it = drapeLayerTweakers.begin(); it != drapeLayerTweakers.end();) {
            if (liveDrapeIDs.find(it->first) == liveDrapeIDs.end()) {
                it = drapeLayerTweakers.erase(it);
            } else {
                ++it;
            }
        }
    }

    if (!staticDataSharedVertices) {
        staticDataSharedVertices = std::make_shared<HillshadeVertexVector>(RenderStaticData::rasterVertices());
    }
    const auto staticDataIndices = RenderStaticData::quadTriangleIndices();
    const auto staticDataSegments = RenderStaticData::rasterSegments();

    std::unique_ptr<gfx::DrawableBuilder> hillshadeBuilder;
    std::unique_ptr<gfx::DrawableBuilder> hillshadePrepareBuilder;

    gfx::VertexAttributeArrayPtr hillshadePrepareVertexAttrs;
    const auto getPrepareVertexAttributes = [&] {
        if (!hillshadePrepareVertexAttrs) {
            hillshadePrepareVertexAttrs = context.createVertexAttributeArray();

            if (const auto& attr = hillshadePrepareVertexAttrs->set(idHillshadePosVertexAttribute)) {
                attr->setSharedRawData(staticDataSharedVertices,
                                       offsetof(HillshadeLayoutVertex, a1),
                                       0,
                                       sizeof(HillshadeLayoutVertex),
                                       gfx::AttributeDataType::Short2);
            }
            if (const auto& attr = hillshadePrepareVertexAttrs->set(idHillshadeTexturePosVertexAttribute)) {
                attr->setSharedRawData(staticDataSharedVertices,
                                       offsetof(HillshadeLayoutVertex, a2),
                                       0,
                                       sizeof(HillshadeLayoutVertex),
                                       gfx::AttributeDataType::Short2);
            }
        }
        return hillshadePrepareVertexAttrs;
    };

    // Flat-neutral prepared texture: RG=128 decodes to deriv=0 in the
    // hillshade shader, so a tile whose prepare target has not baked yet
    // shades exactly like flat terrain — instead of sampling undefined
    // (black) memory (the launch/churn black flash) or blinking off
    // entirely (the .42 enable-gate).
    if (!neutralPrepareTexture) {
        auto img = std::make_shared<PremultipliedImage>(Size{2, 2});
        constexpr uint8_t flatPixel[4] = {128, 128, 0, 255};
        for (std::size_t i = 0; i + 3 < img->bytes(); i += 4) {
            std::memcpy(img->data.get() + i, flatPixel, 4);
        }
        neutralPrepareTexture = context.createTexture2D();
        if (neutralPrepareTexture) {
            neutralPrepareTexture->setImage(std::move(img));
        }
    }

    for (const RenderTile& tile : *renderTiles) {
        const auto& tileID = tile.getOverscaledTileID();

        auto* bucket_ = tile.getBucket(*baseImpl);
        if (!bucket_ || !bucket_->hasData()) {
            removeTile(renderPass, tileID);
            continue;
        }

        auto& bucket = static_cast<HillshadeBucket&>(*bucket_);

        // RasterDEMTile::setMask can change a HillshadeBucket's geometry
        // without changing the bucket identity. The main-pass path updates
        // that geometry, but terrain drape drawables used to hit the
        // getDrawableCount guard below and retain their old footprint. During
        // cover transitions that leaves stale parents painted under current
        // children, so translucent hillshade is applied twice and appears to
        // flash darker/lighter as the camera moves. Invalidate only this
        // source tile's drape copies when its disjoint TileMask changes; the
        // normal routing below recreates them with the current geometry in
        // the same update.
        bool drapeMaskChanged = false;
        if (activeTerrain) {
            const auto maskIt = drapeTileMasks.find(tileID);
            if (maskIt == drapeTileMasks.end()) {
                drapeTileMasks.emplace(tileID, bucket.mask);
            } else if (maskIt->second != bucket.mask) {
                maskIt->second = bucket.mask;
                drapeMaskChanged = true;
            }
        }
        if (drapeMaskChanged) {
            std::size_t removedForMaskChange = 0;
            activeTerrain->visitDrapeTargets(
                [&](const OverscaledTileID&, TerrainDrapeTargetPtr& drapeTarget) {
                    if (!drapeTarget) return;
                    if (auto* drapeGroup = static_cast<TileLayerGroup*>(
                            drapeTarget->getLayerGroup(layerIndex).get())) {
                        removedForMaskChange += drapeGroup->removeDrawables(renderPass, tileID).size();
                    }
                });
            stats.drawablesRemoved += removedForMaskChange;
            if (removedForMaskChange && klattraLogDrapeStale()) {
                Log::Info(Event::Render,
                          "[Klättra DRAPE_STALE] layer=" + getID() +
                              " kind=hillshade-mask-resync tile=" + klattraDrapeIDString(tileID) +
                              " removed=" + std::to_string(removedForMaskChange) +
                              " mask-cells=" + std::to_string(bucket.mask.size()));
            }
        }

        const auto prevBucketID = getRenderTileBucketID(tileID);
        if (prevBucketID != util::SimpleIdentity::Empty && prevBucketID != bucket.getID()) {
            // This tile was previously set up from a different bucket, drop and re-create any drawables for it.
            removeTile(renderPass, tileID);
            // Also drop drape-pass drawables for this tile (see
            // render_fill_layer.cpp comment).
            if (activeTerrain) {
                activeTerrain->visitDrapeTargets(
                    [&](const OverscaledTileID&, TerrainDrapeTargetPtr& drapeTarget) {
                        if (!drapeTarget) return;
                        if (auto* drapeGroup = static_cast<TileLayerGroup*>(
                                drapeTarget->getLayerGroup(layerIndex).get())) {
                            stats.drawablesRemoved += drapeGroup->removeDrawables(renderPass, tileID).size();
                        }
                    });
            }
        }
        setRenderTileBucketID(tileID, bucket.getID());

        // Carry the completed bake for this tile. On a re-parse frame the
        // fresh bucket's target is unbaked, so the entry still holds the
        // previous bucket's bake until the replacement completes and
        // overwrites it here.
        if (!klattraDisableBakeCarry() && bucket.renderTarget && bucket.renderTarget->hasCompletedRender()) {
            carriedBakeTextures[tileID] = bucket.renderTarget->getTexture();
        }

        if (!bucket.renderTargetPrepared) {
            // Set up tile render target
            const uint16_t tilesize = bucket.getDEMData().dim;
            auto renderTarget = context.createRenderTarget({tilesize, tilesize},
                                                           gfx::TextureChannelDataType::UnsignedByte);
            if (!renderTarget) {
                continue;
            }
            renderTarget->setDebugName("hillshade-prep " + klattraDrapeIDString(tileID));
            bucket.renderTarget = renderTarget;
            bucket.renderTargetPrepared = true;
            addRenderTarget(renderTarget, changes);

            // Force the offscreen colour texture to allocate its underlying GPU
            // texture immediately. Without this the drape pass (which renders
            // before the prepare pass on the very first frame the bucket is
            // alive) would try to bind a `textureDirty == true` Texture2D and
            // hit `assert(!textureDirty)` in mtl::Texture2D::bind. The texture
            // contents are undefined until the prepare pass runs once, but
            // sampling an empty colour buffer is recoverable; sampling an
            // unallocated one is a crash.
            if (const auto& tex = renderTarget->getTexture()) {
                tex->create();
            }

            auto singleTileLayerGroup = context.createTileLayerGroup(0, /*initialCapacity=*/1, getID());
            if (!singleTileLayerGroup) {
                return;
            }
            renderTarget->addLayerGroup(singleTileLayerGroup, /*replace=*/true);

            if (!prepareLayerTweaker) {
                prepareLayerTweaker = std::make_shared<HillshadePrepareLayerTweaker>(getID(), evaluatedProperties);
            }
            singleTileLayerGroup->addLayerTweaker(prepareLayerTweaker);

            hillshadePrepareBuilder = context.createDrawableBuilder("hillshadePrepare");
            hillshadePrepareBuilder->setShader(hillshadePrepareShader);
            hillshadePrepareBuilder->setDepthType(gfx::DepthMaskType::ReadOnly);
            hillshadePrepareBuilder->setColorMode(gfx::ColorMode::unblended());
            hillshadePrepareBuilder->setCullFaceMode(gfx::CullFaceMode::disabled());
            hillshadePrepareBuilder->setRenderPass(renderPass);
            hillshadePrepareBuilder->setVertexAttributes(getPrepareVertexAttributes());
            hillshadePrepareBuilder->setRawVertices(
                {}, staticDataSharedVertices->elements(), gfx::AttributeDataType::Short2);
            hillshadePrepareBuilder->setSegments(
                gfx::Triangles(), staticDataIndices.vector(), staticDataSegments.data(), staticDataSegments.size());

            std::shared_ptr<gfx::Texture2D> texture = context.createTexture2D();
            texture->setImage(bucket.getDEMData().getImagePtr());
            // Use Nearest filtering to match GL JS behavior - the Sobel kernel samples exact pixel values
            texture->setSamplerConfiguration({.filter = gfx::TextureFilterType::Nearest,
                                              .wrapU = gfx::TextureWrapType::Clamp,
                                              .wrapV = gfx::TextureWrapType::Clamp});
            hillshadePrepareBuilder->setTexture(texture, idHillshadeImageTexture);

            hillshadePrepareBuilder->flush(context);

            for (auto& drawable : hillshadePrepareBuilder->clearDrawables()) {
                drawable->setTileID(tileID);
                drawable->setLayerTweaker(prepareLayerTweaker);
                drawable->setData(std::make_unique<gfx::HillshadePrepareDrawableData>(
                    bucket.getDEMData().stride, bucket.getDEMData().encoding, maxzoom));
                singleTileLayerGroup->addDrawable(renderPass, tileID, std::move(drawable));
                ++stats.drawablesAdded;
            }
        }

        // While this tile's target bakes (fresh tile, or a bucket re-parse
        // that recreated it), sample the carried previous bake if there is
        // one, else the flat-neutral texture.
        gfx::Texture2DPtr unbakedFallback = neutralPrepareTexture;
        if (!klattraDisableBakeCarry()) {
            if (const auto carried = carriedBakeTextures.find(tileID);
                carried != carriedBakeTextures.end() && carried->second) {
                unbakedFallback = carried->second;
            }
        }

        // Set up tile drawable
        std::shared_ptr<HillshadeVertexVector> vertices;
        std::shared_ptr<gfx::IndexVector<gfx::Triangles>> indices;
        auto* segments = &staticDataSegments;

        if (!bucket.vertices.empty() && !bucket.indices.empty() && !bucket.segments.empty()) {
            vertices = bucket.sharedVertices;
            indices = bucket.sharedIndices;
            segments = &bucket.segments;
        } else {
            vertices = staticDataSharedVertices;
            indices = std::make_shared<gfx::IndexVector<gfx::Triangles>>(staticDataIndices);
        }

        if (!hillshadeBuilder) {
            hillshadeBuilder = context.createDrawableBuilder("hillshade");
        }

        gfx::VertexAttributeArrayPtr hillshadeVertexAttrs;
        auto buildVertexAttributes = [&] {
            if (!hillshadeVertexAttrs) {
                hillshadeVertexAttrs = context.createVertexAttributeArray();

                if (const auto& attr = hillshadeVertexAttrs->set(idHillshadePosVertexAttribute)) {
                    attr->setSharedRawData(vertices,
                                           offsetof(HillshadeLayoutVertex, a1),
                                           0,
                                           sizeof(HillshadeLayoutVertex),
                                           gfx::AttributeDataType::Short2);
                }
                if (const auto& attr = hillshadeVertexAttrs->set(idHillshadeTexturePosVertexAttribute)) {
                    attr->setSharedRawData(vertices,
                                           offsetof(HillshadeLayoutVertex, a2),
                                           0,
                                           sizeof(HillshadeLayoutVertex),
                                           gfx::AttributeDataType::Short2);
                }
            }
            return hillshadeVertexAttrs;
        };

        // Phase 2 drape routing: emit a copy of this hillshade tile's prepared
        // colour quad into each overlapping DEM drape RenderTarget so the
        // terrain mesh can sample hillshade as it displaces. Runs every frame
        // (guarded by getDrawableCount) so a newly-allocated drape target
        // picks up its drawable even when the source tile drawable below is
        // updated in place. Done before the updateTile call because the
        // updateExisting lambda may std::move(indices).
        if (activeTerrain && bucket.renderTarget &&
            !bucket.renderTarget->hasCompletedRender() &&
            klattraLogDrapeStale()) {
            Log::Info(Event::Render,
                      "[Klättra DRAPE_STALE] layer=" + getID() +
                          " kind=hillshade-source-not-ready tile=" + klattraDrapeIDString(tileID) +
                          " target=" + bucket.renderTarget->getDebugName() +
                          " action=skip-drape-route");
        }
        if (activeTerrain && klattraDisableHillshadeDrape()) {
            if (klattraLogDrapeStale()) {
                Log::Info(Event::Render,
                          "[Klättra DRAPE_STALE] layer=" + getID() +
                              " kind=hillshade-drape-disabled tile=" + klattraDrapeIDString(tileID));
            }
        } else if (activeTerrain && bucket.renderTarget && bucket.renderTarget->hasCompletedRender()) {
            activeTerrain->visitDrapeTargets(
                [&](const OverscaledTileID& drapeID, TerrainDrapeTargetPtr& drapeTarget) {
                    if (!drapeTarget || !LayerTweaker::tilesOverlap(tileID, drapeID)) return;
                    if (klattraHillshadeDrapeSameZoomOnly() && tileID.canonical.z != drapeID.canonical.z) {
                        if (klattraLogDrapeStale()) {
                            Log::Info(Event::Render,
                                      "[Klättra DRAPE_STALE] layer=" + getID() +
                                          " kind=hillshade-crosszoom-skip tile=" + klattraDrapeIDString(tileID) +
                                          " drape=" + klattraDrapeIDString(drapeID));
                        }
                        return;
                    }

                    auto& tw = drapeLayerTweakers[drapeID];
                    if (!tw) {
                        tw = std::make_shared<HillshadeLayerTweaker>(
                            getID() + "-drape", evaluatedProperties, drapeID);
                    }

                    auto* drapeGroup = static_cast<TileLayerGroup*>(
                        drapeTarget->getLayerGroup(layerIndex).get());
                    if (!drapeGroup) {
                        auto newGroup = context.createTileLayerGroup(
                            layerIndex, /*initialCapacity=*/4, getID() + "-drape");
                        if (!newGroup) return;
                        newGroup->addLayerTweaker(tw);
                        drapeTarget->addLayerGroup(newGroup, /*replace=*/false);
                        drapeGroup = newGroup.get();
                    }

                    if (drapeGroup->getDrawableCount(renderPass, tileID) > 0) return;

                    auto drapeBuilder = context.createDrawableBuilder("hillshade-drape");
                    if (!drapeBuilder) return;
                    drapeBuilder->setShader(hillshadeShader);
                    drapeBuilder->setDepthType(gfx::DepthMaskType::ReadOnly);
                    drapeBuilder->setColorMode(gfx::ColorMode::alphaBlended());
                    drapeBuilder->setCullFaceMode(gfx::CullFaceMode::disabled());
                    drapeBuilder->setRenderPass(renderPass);
                    drapeBuilder->setVertexAttributes(buildVertexAttributes());
                    drapeBuilder->setRawVertices({}, vertices->elements(), gfx::AttributeDataType::Short2);
                    drapeBuilder->setSegments(
                        gfx::Triangles(), indices->vector(), segments->data(), segments->size());
                    drapeBuilder->setTexture(bucket.renderTarget->getTexture(), idHillshadeImageTexture);
                    drapeBuilder->flush(context);

                    for (auto& drapeDrawable : drapeBuilder->clearDrawables()) {
                        drapeDrawable->setTileID(tileID);
                        drapeDrawable->setLayerTweaker(tw);
                        // Parent and child masks should be disjoint. Keep a
                        // deterministic fallback order at their shared edges:
                        // coarse coverage first, finer relief last, matching
                        // the raster terrain-drape path.
                        drapeDrawable->setDrawPriority(static_cast<gfx::DrawPriority>(tileID.canonical.z));
                        drapeGroup->addDrawable(renderPass, tileID, std::move(drapeDrawable));
                        ++stats.drawablesAdded;
                    }
                });
        }

        // When terrain is active and this tile has DEM coverage, hillshade
        // is consumed exclusively via the drape RenderTarget — emitting
        // the main 2D tile drawable as well would render hillshade as a
        // flat translucent overlay on top of the extruded terrain mesh
        // (terrain renders LAST in the opaque pass, but hillshade is
        // translucent and renders after that), hiding the 3D effect. Skip
        // the main drawable in that case; the drape drawable above already
        // routed the prepared colour into the per-DEM-tile drape target.
        //
        // When this tile has no DEM coverage, fall through to the main-pass
        // emit so hillshade still renders flat.
        if (activeTerrain && activeTerrain->hasElevationCoverage(tileID)) {
            removeTile(renderPass, tileID);
            continue;
        }

        const auto updateExisting = [&](gfx::Drawable& drawable) {
            // Only current drawables are updated, ones produced for
            // a previous style retain the attribute values for that style.
            if (drawable.getLayerTweaker() != layerTweaker) {
                return false;
            }

            drawable.updateVertexAttributes(buildVertexAttributes(),
                                            vertices->elements(),
                                            gfx::Triangles(),
                                            std::move(indices),
                                            segments->data(),
                                            segments->size());
            // KLATTRA (2D black flash, the painted-black half): a freshly
            // created prepare target holds UNDEFINED memory until its bake
            // runs — sampling it painted the tile black (viewport-wide at
            // boot when every target is new; per new-tile batch during zoom
            // churn; water survives because the style re-draws it above the
            // relief). Until the bake completes, sample the carried previous
            // bake (bucket re-parse) or the flat-neutral texture (fresh
            // tile); the real shading swaps in the frame after the bake
            // (updateExisting runs every frame).
            const bool prepared = bucket.renderTarget && bucket.renderTarget->hasCompletedRender();
            drawable.setTexture(prepared ? bucket.renderTarget->getTexture() : unbakedFallback,
                                idHillshadeImageTexture);
            drawable.setEnabled(prepared || unbakedFallback != nullptr);

            return true;
        };
        if (updateTile(renderPass, tileID, std::move(updateExisting))) {
            continue;
        }

        hillshadeBuilder->setShader(hillshadeShader);
        hillshadeBuilder->setEnableDepth(false);
        hillshadeBuilder->setColorMode(gfx::ColorMode::alphaBlended());
        hillshadeBuilder->setCullFaceMode(gfx::CullFaceMode::disabled());
        hillshadeBuilder->setRenderPass(renderPass);
        hillshadeBuilder->setVertexAttributes(buildVertexAttributes());
        hillshadeBuilder->setRawVertices({}, vertices->elements(), gfx::AttributeDataType::Short2);
        hillshadeBuilder->setSegments(gfx::Triangles(), indices->vector(), segments->data(), segments->size());
        const bool bucketPrepared = bucket.renderTarget && bucket.renderTarget->hasCompletedRender();
        hillshadeBuilder->setTexture(
            bucketPrepared ? bucket.renderTarget->getTexture() : unbakedFallback, idHillshadeImageTexture);

        hillshadeBuilder->flush(context);

        for (auto& drawable : hillshadeBuilder->clearDrawables()) {
            if (activeTerrain && activeTerrain->hasElevationCoverage(tileID)) {
                // Skip main-pass — terrain mesh drapes hillshade via the
                // drape-pass variant emitted below. See cleanup-on-terrain
                // block above. Drawable falls out of scope here and is
                // destroyed. When this tile has no DEM coverage, fall
                // through to the main-pass emit so hillshade still renders.
                continue;
            }
            drawable->setTileID(tileID);
            drawable->setLayerTweaker(layerTweaker);
            // See updateExisting above: carried/neutral texture until baked.
            drawable->setEnabled(bucketPrepared || unbakedFallback != nullptr);

            tileLayerGroup->addDrawable(renderPass, tileID, std::move(drawable));
            ++stats.drawablesAdded;
        }
    }
}

} // namespace mbgl
