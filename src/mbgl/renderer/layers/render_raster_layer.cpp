#include <mbgl/renderer/layers/render_raster_layer.hpp>
#include <chrono>
#include <mbgl/renderer/buckets/raster_bucket.hpp>
#include <mbgl/renderer/render_tile.hpp>
#include <mbgl/renderer/paint_parameters.hpp>
#include <mbgl/renderer/render_static_data.hpp>
#include <mbgl/renderer/sources/render_image_source.hpp>
#include <mbgl/tile/tile.hpp>
#include <mbgl/gfx/context.hpp>
#include <mbgl/gfx/cull_face_mode.hpp>
#include <mbgl/math/angles.hpp>
#include <mbgl/style/layers/raster_layer_impl.hpp>
#include <mbgl/util/logging.hpp>

#include <mbgl/renderer/layers/raster_layer_tweaker.hpp>
#include <mbgl/gfx/image_drawable_data.hpp>
#include <mbgl/gfx/drawable_impl.hpp>
#include <mbgl/gfx/drawable_builder.hpp>
#include <mbgl/renderer/layer_group.hpp>
#include <mbgl/renderer/render_terrain.hpp>
#include <mbgl/renderer/update_parameters.hpp>
#include <mbgl/shaders/shader_program_base.hpp>

#include <cstdlib>
#include <cstring>
#include <unordered_set>

namespace mbgl {

using namespace style;
using namespace shaders;

namespace {

inline const RasterLayer::Impl& impl_cast(const Immutable<style::Layer::Impl>& impl) {
    assert(impl->getTypeInfo() == RasterLayer::Impl::staticTypeInfo());
    return static_cast<const RasterLayer::Impl&>(*impl);
}

bool klattraLogDrapeStale() {
    return std::getenv("KLATTRA_LOG_DRAPE_STALE") != nullptr;
}

bool klattraDisableRasterDrape() {
    static const bool disabled = std::getenv("KLATTRA_DISABLE_RASTER_DRAPE") != nullptr;
    return disabled;
}

std::string klattraDrapeIDString(const OverscaledTileID& id) {
    return "z" + std::to_string(static_cast<int>(id.canonical.z)) + "/" +
           std::to_string(id.canonical.x) + "/" + std::to_string(id.canonical.y);
}

} // namespace

RenderRasterLayer::RenderRasterLayer(Immutable<style::RasterLayer::Impl> _impl)
    : RenderLayer(makeMutable<RasterLayerProperties>(std::move(_impl))),
      unevaluated(impl_cast(baseImpl).paint.untransitioned()) {
    styleDependencies = unevaluated.getDependencies();
}

RenderRasterLayer::~RenderRasterLayer() = default;

void RenderRasterLayer::transition(const TransitionParameters& parameters) {
    unevaluated = impl_cast(baseImpl).paint.transitioned(parameters, std::move(unevaluated));
    styleDependencies = unevaluated.getDependencies();
}

void RenderRasterLayer::evaluate(const PropertyEvaluationParameters& parameters) {
    const auto previousProperties = staticImmutableCast<RasterLayerProperties>(evaluatedProperties);
    auto properties = makeMutable<RasterLayerProperties>(
        staticImmutableCast<RasterLayer::Impl>(baseImpl),
        unevaluated.evaluate(parameters, previousProperties->evaluated));

    passes = properties->evaluated.get<style::RasterOpacity>() > 0 ? RenderPass::Translucent : RenderPass::None;
    properties->renderPasses = mbgl::underlying_type(passes);
    evaluatedProperties = std::move(properties);

    if (layerTweaker) {
        layerTweaker->updateProperties(evaluatedProperties);
    }
    // Mirror to the per-drape-target tweakers — see same pattern in
    // RenderFillLayer / RenderLineLayer.
    for (auto& [_, tw] : drapeLayerTweakers) {
        if (tw) tw->updateProperties(evaluatedProperties);
    }
}

bool RenderRasterLayer::hasTransition() const {
    return unevaluated.hasTransition();
}

bool RenderRasterLayer::hasCrossfade() const {
    return false;
}

void RenderRasterLayer::prepare(const LayerPrepareParameters& params) {
    renderTiles = params.source->getRenderTiles();
    imageData = params.source->getImageRenderData();
    // It is possible image data is not available until the source loads it.
    assert(renderTiles || imageData || !params.source->isEnabled());

    updateRenderTileIDs();
}

void RenderRasterLayer::markLayerRenderable(bool willRender, UniqueChangeRequestVec& changes) {
    RenderLayer::markLayerRenderable(willRender, changes);
    if (imageLayerGroup) {
        activateLayerGroup(imageLayerGroup, willRender, changes);
    }
}

void RenderRasterLayer::layerRemoved(UniqueChangeRequestVec& changes) {
    RenderLayer::layerRemoved(changes);
    if (imageLayerGroup) {
        activateLayerGroup(imageLayerGroup, false, changes);
    }
}

void RenderRasterLayer::layerIndexChanged(int32_t newLayerIndex, UniqueChangeRequestVec& changes) {
    RenderLayer::layerIndexChanged(newLayerIndex, changes);

    changeLayerIndex(imageLayerGroup, newLayerIndex, changes);
}

void RenderRasterLayer::update(gfx::ShaderRegistry& shaders,
                               gfx::Context& context,
                               const TransformState& /*state*/,
                               const std::shared_ptr<UpdateParameters>&,
                               [[maybe_unused]] const RenderTree& renderTree,
                               [[maybe_unused]] UniqueChangeRequestVec& changes) {
    if ((!renderTiles || renderTiles->empty()) && !imageData) {
        if (layerGroup) {
            stats.drawablesRemoved += layerGroup->clearDrawables();
        }
        if (imageLayerGroup) {
            stats.drawablesRemoved += imageLayerGroup->clearDrawables();
        }
        return;
    }

    constexpr auto renderPass = RenderPass::Translucent;

    if (!rasterShader) {
        rasterShader = context.getGenericShader(shaders, "RasterShader");
        if (!rasterShader) {
            return;
        }
    }

    if (!layerTweaker) {
        layerTweaker = std::make_shared<RasterLayerTweaker>(getID(), evaluatedProperties);

        if (layerGroup) {
            layerGroup->addLayerTweaker(layerTweaker);
        }
        if (imageLayerGroup) {
            imageLayerGroup->addLayerTweaker(layerTweaker);
        }
    }

    if (!staticDataVertices) {
        staticDataVertices = std::make_shared<RasterVertexVector>(RenderStaticData::rasterVertices());
    }
    if (!staticDataIndices) {
        staticDataIndices = std::make_shared<TriangleIndexVector>(RenderStaticData::quadTriangleIndices());
    }
    if (!staticDataSegments) {
        staticDataSegments = std::make_shared<SegmentVector>(RenderStaticData::rasterSegments());
    }

    const auto createBuilder = [&] {
        auto builder = context.createDrawableBuilder("raster");
        builder->setShader(rasterShader);
        builder->setRenderPass(renderPass);
        builder->setSubLayerIndex(0);
        builder->setDepthType(gfx::DepthMaskType::ReadOnly);
        builder->setColorMode(gfx::ColorMode::alphaBlended());
        builder->setCullFaceMode(gfx::CullFaceMode::disabled());
        return builder;
    };

    const auto setTextures = [&](gfx::UniqueDrawableBuilder& builder, RasterBucket& bucket) {
        {
            // Create the GPU texture from the CPU image on first use, but
            // REUSE an existing texture even if the image has since been
            // released — requiring the image here silently starved drape
            // routing for long-lived tiles (satellite far field baked pure
            // background, 2026-07-04 "beige wedges").
            if (!bucket.texture2d && bucket.image) {
                if (auto tex = context.createTexture2D()) {
                    tex->setImage(bucket.image);
                    bucket.texture2d = std::move(tex);
                }
            }

            if (bucket.texture2d) {
                const auto& evaluated = static_cast<const RasterLayerProperties&>(*evaluatedProperties).evaluated;
                const bool nearest = evaluated.get<RasterResampling>() == RasterResamplingType::Nearest;
                const auto filter = nearest ? gfx::TextureFilterType::Nearest : gfx::TextureFilterType::Linear;

                bucket.texture2d->setSamplerConfiguration(
                    {.filter = filter, .wrapU = gfx::TextureWrapType::Clamp, .wrapV = gfx::TextureWrapType::Clamp});

                builder->setTexture(bucket.texture2d, idRasterImage0Texture);
                builder->setTexture(bucket.texture2d, idRasterImage1Texture);
            }
        }
    };

    gfx::VertexAttributeArrayPtr staticAttrs;

    // Build vertex attributes and apply them to a drawable or a builder.
    // Populates a drawable xor a drawable builder for creates and updates, respectively.
    // Returns false if the drawable must be re-created.
    const auto buildVertexData =
        [&](const gfx::UniqueDrawableBuilder& builder,
            gfx::Drawable* drawable,
            const RasterBucket& bucket,
            bool freshIndexBuffer = false,
            bool localVertexBuffers = false) {
            // The bucket may later add, remove, or change masking.  In that case, the tile's
            // shared data and segments are not updated, and it needs to be re-created.
            if (drawable && bucket.sharedVertices->isModifiedAfter(drawable->createTime)) {
                return false;
            }

            // The bucket only fills in geometry for masked tiles,
            // otherwise the standard tile extent geometry should be used.
            const bool shared = (!bucket.sharedVertices->empty() && !bucket.sharedTriangles->empty() &&
                                 !bucket.segments.empty());
            const auto& vertices = shared ? bucket.sharedVertices : staticDataVertices;
            const auto& indices = shared ? bucket.sharedTriangles : staticDataIndices;
            const auto drawableIndices =
                freshIndexBuffer && shared
                    ? std::make_shared<gfx::IndexVectorBase>(*indices)
                    : std::static_pointer_cast<gfx::IndexVectorBase>(indices);
            const auto* segments = shared ? &bucket.segments : staticDataSegments.get();

            gfx::VertexAttributeArrayPtr bucketAttrs;
            auto& vertexAttrs = shared ? bucketAttrs : staticAttrs;
            if (localVertexBuffers) {
                vertexAttrs = context.createVertexAttributeArray();
                const auto vertexCount = vertices->elements();
                const auto* vertexData = vertices->data();
                if (vertexData && vertexCount > 0) {
                    if (auto& attr = vertexAttrs->set(
                            idRasterPosVertexAttribute,
                            /*index=*/-1,
                            gfx::AttributeDataType::Short2,
                            vertexCount)) {
                        std::vector<std::uint8_t> raw(vertexCount * sizeof(RasterLayoutVertex::a1));
                        for (std::size_t i = 0; i < vertexCount; ++i) {
                            std::memcpy(raw.data() + (i * sizeof(RasterLayoutVertex::a1)),
                                        &vertexData[i].a1,
                                        sizeof(RasterLayoutVertex::a1));
                        }
                        attr->setRawData(std::move(raw));
                    }

                    if (auto& attr = vertexAttrs->set(
                            idRasterTexturePosVertexAttribute,
                            /*index=*/-1,
                            gfx::AttributeDataType::Short2,
                            vertexCount)) {
                        std::vector<std::uint8_t> raw(vertexCount * sizeof(RasterLayoutVertex::a2));
                        for (std::size_t i = 0; i < vertexCount; ++i) {
                            std::memcpy(raw.data() + (i * sizeof(RasterLayoutVertex::a2)),
                                        &vertexData[i].a2,
                                        sizeof(RasterLayoutVertex::a2));
                        }
                        attr->setRawData(std::move(raw));
                    }
                }
            } else if (!vertexAttrs) {
                vertexAttrs = context.createVertexAttributeArray();

                if (auto& attr = vertexAttrs->set(idRasterPosVertexAttribute)) {
                    attr->setSharedRawData(vertices,
                                           offsetof(RasterLayoutVertex, a1),
                                           /*vertexOffset=*/0,
                                           sizeof(RasterLayoutVertex),
                                           gfx::AttributeDataType::Short2);
                }

                if (auto& attr = vertexAttrs->set(idRasterTexturePosVertexAttribute)) {
                    attr->setSharedRawData(vertices,
                                           offsetof(RasterLayoutVertex, a2),
                                           /*vertexOffset=*/0,
                                           sizeof(RasterLayoutVertex),
                                           gfx::AttributeDataType::Short2);
                }
            }

            assert(!!drawable ^ !!builder);
            if (drawable) {
                drawable->updateVertexAttributes(
                    vertexAttrs,
                    vertices->elements(),
                    gfx::Triangles(),
                    drawableIndices,
                    segments->data(),
                    segments->size());
            } else if (builder) {
                builder->setVertexAttributes(vertexAttrs);
                builder->setRawVertices({}, vertices->elements(), gfx::AttributeDataType::Short2);
                builder->setSegments(gfx::Triangles(), drawableIndices, segments->data(), segments->size());
            }
            return true;
        };

    gfx::UniqueDrawableBuilder builder;
    if (imageData) {
        // TODO: Can we avoid rebuilding drawables each time in this case as well?
        if (imageLayerGroup) {
            stats.drawablesRemoved += imageLayerGroup->clearDrawables();
        }

        RasterBucket& bucket = *imageData->bucket;
        if (!bucket.vertices.empty()) {
            if (!imageLayerGroup) {
                // Set up a layer group
                imageLayerGroup = context.createLayerGroup(layerIndex, /*initialCapacity=*/64, getID());
                imageLayerGroup->addLayerTweaker(layerTweaker);
                activateLayerGroup(imageLayerGroup, isRenderable, changes);
            }

            // Create a drawable for each transformation
            // TODO: Share textures
            builder = createBuilder();
            for (const auto& matrix_ : imageData->matrices) {
                buildVertexData(builder, /*drawable=*/nullptr, bucket);
                setTextures(builder, bucket);

                // finish
                builder->flush(context);

                for (auto& drawable : builder->clearDrawables()) {
                    drawable->setData(std::make_unique<gfx::ImageDrawableData>(matrix_));
                    drawable->setLayerTweaker(layerTweaker);
                    imageLayerGroup->addDrawable(std::move(drawable));
                    ++stats.drawablesAdded;
                }
            }
        }
    } else if (renderTiles) {
        // Remove existing drawables that are no longer in the cover set
        if (layerGroup) {
            stats.drawablesRemoved += removeLayerGroupDrawablesIf(*layerGroup, [&](gfx::Drawable& drawable) {
                return drawable.getTileID() && !hasRenderTile(*drawable.getTileID());
            });
        } else {
            // Set up a tile layer group
            if (auto layerGroup_ = context.createTileLayerGroup(layerIndex, /*initialCapacity=*/64, getID())) {
                layerGroup_->addLayerTweaker(layerTweaker);
                setLayerGroup(std::move(layerGroup_), changes);
            }
        }

        // Mirror the cover-set cleanup for drape groups and prune stale
        // drape tweakers — same pattern as fill / line.
        if (activeTerrain) {
            std::unordered_set<OverscaledTileID> liveDrapeIDs;
            activeTerrain->visitDrapeTargets(
                [&](const OverscaledTileID& drapeID, TerrainDrapeTargetPtr& drapeTarget) {
                    if (!drapeTarget) return;
                    drapeTarget->requireRasterDrapeContent();
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
                                          " kind=raster drape=" + klattraDrapeIDString(drapeID) +
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

        auto* tileLayerGroup = static_cast<TileLayerGroup*>(layerGroup.get());

        // When terrain is active, drop main-pass raster drawables for tiles
        // with DEM coverage — the drape variant emitted further below paints
        // them onto the terrain mesh. Tiles without DEM coverage keep their
        // main-pass drawables so the layer doesn't vanish at zooms below
        // the DEM source's range. See `render_fill_layer.cpp` for the
        // rationale.
        if (activeTerrain) {
            stats.drawablesRemoved += tileLayerGroup->removeDrawablesIf([&](gfx::Drawable& drawable) {
                const auto& tileID = drawable.getTileID();
                return tileID && activeTerrain->hasReadyTerrainCoverage(*tileID);
            });
        }

        // [KLATTRA RASTER_DRAPE] probe counters for this update pass — which
    // exit of the drape-routing pipeline drops the far field?
    std::size_t probeTiles = 0, probeBypassed = 0, probeBackfills = 0, probeBlockTiles = 0,
                probeOverlaps = 0, probeAdded = 0, probeSkipNoTexSrc = 0, probeSkipBuilderTex = 0,
                probeRemovedStale = 0;
    for (const RenderTile& tile : *renderTiles) {
        ++probeTiles;
            const auto& tileID = tile.getOverscaledTileID();

            auto* bucket_ = tile.getBucket(*baseImpl);
            if (!bucket_ || !bucket_->hasData()) {
                removeTile(renderPass, tileID);
                continue;
            }

            bool cleared = false;
            auto& bucket = static_cast<RasterBucket&>(*bucket_);

            if (setRenderTileBucketID(tileID, bucket.getID())) {
                // Bucket ID changed, we need to rebuild the drawables
                removeTile(renderPass, tileID);
                // Also drop drape-pass drawables for this tile (see
                // render_fill_layer.cpp comment).
                if (activeTerrain) {
                    activeTerrain->visitDrapeTargets(
                        [&](const OverscaledTileID&, TerrainDrapeTargetPtr& drapeTarget) {
                            if (!drapeTarget) return;
                            if (auto* drapeGroup = static_cast<TileLayerGroup*>(
                                    drapeTarget->getLayerGroup(layerIndex).get())) {
                                {
                            const auto removedStale = drapeGroup->removeDrawables(renderPass, tileID).size();
                            stats.drawablesRemoved += removedStale;
                            probeRemovedStale += removedStale;
                        }
                            }
                        });
                }
                cleared = true;
            }
            // If the bucket data has changed, rebuild the drawables.
            else if (!bucket.vertices.empty() && !bucket.indices.empty() && !bucket.segments.empty()) {
                // Find the earliest time on existing drawables
                std::optional<std::chrono::duration<double>> tileUpdateTime;
                tileLayerGroup->visitDrawables(renderPass, tileID, [&](const auto& drawable) {
                    if (!tileUpdateTime || drawable.createTime < *tileUpdateTime) {
                        tileUpdateTime = drawable.createTime;
                    }
                });

                if (tileUpdateTime && (bucket.vertices.isModifiedAfter(*tileUpdateTime))) {
                    removeTile(renderPass, tileID);
                    cleared = true;
                }
            }

            if (!cleared) {
                // Update existing drawables
                bool geometryChanged = false;
                auto updateExisting = [&](gfx::Drawable& drawable) {
                    // Only current drawables are updated, ones produced for
                    // a previous style retain the attribute values for that style.
                    if (drawable.getLayerTweaker() != layerTweaker) {
                        return false;
                    }

                    gfx::UniqueDrawableBuilder none;
                    if (!geometryChanged && !buildVertexData(none, &drawable, bucket)) {
                        // Masking changed, need to rebuild this tile
                        geometryChanged = true;
                    }
                    return true;
                };
                // If we update existing drawables, don't build new ones.
                // But if the geometry has changed, we need to drop and re-build them anyway.
                if (updateTile(renderPass, tileID, std::move(updateExisting)) && !geometryChanged) {
                    // Fresh drape targets have no raster copy of this tile yet:
                    // the drape routing below only runs when this tile
                    // rebuilds, so a target created afterwards (leading-edge
                    // cover growth, ring resizes) used to wait for an
                    // unrelated rebuild — the drape baked background-only in
                    // the meantime ("beige/black until the camera moves",
                    // 2026-07-04 device finding). If any overlapping target
                    // lacks this tile's drape drawable, fall through to a
                    // rebuild so the standard routing backfills every target;
                    // this triggers at most once per fresh target.
                    bool drapeBackfillNeeded = false;
                    if (activeTerrain && !klattraDisableRasterDrape() && (bucket.image || bucket.texture2d)) {
                ++probeBlockTiles;
                        activeTerrain->visitDrapeTargets(
                            [&](const OverscaledTileID& drapeID, TerrainDrapeTargetPtr& drapeTarget) {
                                if (drapeBackfillNeeded || !drapeTarget ||
                                    !LayerTweaker::tilesOverlap(tileID, drapeID)) {
                                    return;
                                }
                                auto* drapeGroup = static_cast<TileLayerGroup*>(
                                    drapeTarget->getLayerGroup(layerIndex).get());
                                if (!drapeGroup || drapeGroup->getDrawableCount(renderPass, tileID) == 0) {
                                    drapeBackfillNeeded = true;
                                }
                            });
                    }
                    if (!drapeBackfillNeeded) {
                        ++probeBypassed;
                        continue;
                    }
                    ++probeBackfills;
                    removeTile(renderPass, tileID);
                } else if (geometryChanged) {
                    removeTile(renderPass, tileID);
                }
            }

            // Otherwise, create new ones.
            if (!builder) {
                builder = createBuilder();
            }

            if (bucket.image && !builder->getTexture(idRasterImage0Texture) &&
                !builder->getTexture(idRasterImage1Texture)) {
                setTextures(builder, bucket);
            };

            buildVertexData(builder, /*drawable=*/nullptr, bucket);

            // finish
            builder->flush(context);
            for (auto& drawable : builder->clearDrawables()) {
                if (activeTerrain && activeTerrain->hasReadyTerrainCoverage(tileID)) {
                    // Skip main-pass — terrain mesh drapes raster via the
                    // drape-pass variant emitted just below. Drawable
                    // falls out of scope here and is destroyed.
                    // When this tile has no DEM coverage, fall through to
                    // the main-pass emit so the raster doesn't vanish.
                    continue;
                }
                drawable->setTileID(tileID);
                drawable->setLayerTweaker(layerTweaker);
                tileLayerGroup->addDrawable(renderPass, tileID, std::move(drawable));
                ++stats.drawablesAdded;
            }

            // Phase 2 drape routing: emit a copy of this raster tile into each
            // overlapping DEM drape RenderTarget so the terrain mesh can sample
            // raster basemap content (e.g. satellite tiles) as it displaces.
            if (activeTerrain && !klattraDisableRasterDrape()) {
                activeTerrain->visitDrapeTargets(
                    [&](const OverscaledTileID& drapeID, TerrainDrapeTargetPtr& drapeTarget) {
                        if (!drapeTarget || !LayerTweaker::tilesOverlap(tileID, drapeID)) return;
                        ++probeOverlaps;

                        // Do not bake a raster drawable into terrain until a
                        // texture source exists (fresh image OR the already-
                        // uploaded GPU texture). Otherwise the drape target
                        // can be marked ready while sampling the renderer's
                        // default white texture. Crucially: when NEITHER is
                        // available, leave any existing drape drawable alone —
                        // the previous behaviour REMOVED it whenever the CPU
                        // image had been released, evaporating far-field
                        // imagery from targets on every routing pass
                        // (2026-07-04 satellite beige wedges).
                        if (!bucket.image && !bucket.texture2d) {
                            ++probeSkipNoTexSrc;
                            return;
                        }

                        auto& tw = drapeLayerTweakers[drapeID];
                        if (!tw) {
                            tw = std::make_shared<RasterLayerTweaker>(
                                getID() + "-drape", evaluatedProperties, drapeID);
                        }

                        auto* drapeGroup = static_cast<TileLayerGroup*>(
                            drapeTarget->getLayerGroup(layerIndex).get());
                        if (!drapeGroup) {
                            auto newGroup = context.createTileLayerGroup(
                                layerIndex, /*initialCapacity=*/4, getID() + "-raster-drape");
                            if (!newGroup) return;
                            newGroup->addLayerTweaker(tw);
                            drapeTarget->addLayerGroup(newGroup, /*replace=*/false);
                            drapeGroup = newGroup.get();
                        }

                        // Raster buckets can change their mask geometry while
                        // reusing the same shared index vector. The shared
                        // vector may still point at an older uploaded Metal
                        // buffer, which leaves current segments addressing a
                        // shorter index buffer and produces white terrain
                        // patches. Terrain drape targets are rebuilt here
                        // instead of updated in place so each drawable gets a
                        // fresh index vector/buffer that matches its segments.
                        stats.drawablesRemoved += drapeGroup->removeDrawables(renderPass, tileID).size();

                        auto drapeBuilder = createBuilder();
                        if (!drapeBuilder) return;
                        setTextures(drapeBuilder, bucket);
                        if (!drapeBuilder->getTexture(idRasterImage0Texture) ||
                            !drapeBuilder->getTexture(idRasterImage1Texture)) {
                            ++probeSkipBuilderTex;
                            return;
                        }
                        buildVertexData(drapeBuilder,
                                        /*drawable=*/nullptr,
                                        bucket,
                                        /*freshIndexBuffer=*/true,
                                        /*localVertexBuffers=*/true);
                        drapeBuilder->flush(context);

                        for (auto& drapeDrawable : drapeBuilder->clearDrawables()) {
                            drapeDrawable->setTileID(tileID);
                            drapeDrawable->setLayerTweaker(tw);
                            drapeGroup->addDrawable(renderPass, tileID, std::move(drapeDrawable));
                            ++stats.drawablesAdded;
                            ++probeAdded;
                        }
                    });
            }
        }

        // 1 Hz probe summary (stderr — the simulator swallows Log::Warning).
        if (std::getenv("KLATTRA_TRACE_STDERR") != nullptr &&
            (probeTiles || probeBlockTiles || probeAdded)) {
            static std::chrono::steady_clock::time_point lastProbe{};
            const auto now = std::chrono::steady_clock::now();
            if (now - lastProbe >= std::chrono::milliseconds(900)) {
                lastProbe = now;
                fprintf(stderr,
                        "[KLATTRA_TRACE] [KLATTRA RASTER_DRAPE] layer=%s tiles=%zu bypassed=%zu backfills=%zu "
                        "blockTiles=%zu overlaps=%zu added=%zu skipNoTexSrc=%zu skipBuilderTex=%zu removedStale=%zu\n",
                        getID().c_str(),
                        probeTiles,
                        probeBypassed,
                        probeBackfills,
                        probeBlockTiles,
                        probeOverlaps,
                        probeAdded,
                        probeSkipNoTexSrc,
                        probeSkipBuilderTex,
                        probeRemovedStale);
            }
        }
    }
}

} // namespace mbgl
