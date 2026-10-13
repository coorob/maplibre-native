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
#include <mbgl/renderer/raster_drape_cohort.hpp>
#include <mbgl/renderer/render_terrain.hpp>
#include <mbgl/renderer/update_parameters.hpp>
#include <mbgl/shaders/shader_program_base.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <unordered_map>
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

// .55: keep stale raster drape content until replacement content routes
// (fix for the flyover green plates / readiness revocations). Kill switch
// reverts to the remove-on-cover-exit behaviour for A/B on sim.
bool klattraKeepStaleRasterDrape() {
    static const bool enabled = std::getenv("KLATTRA_DISABLE_STALE_KEEP") == nullptr;
    return enabled;
}

// .57: drape drawables render the full tile extent instead of the
// main-pass mask geometry, with drawPriority = canonical z for painter's
// order. Kill switch reverts to masked drape geometry for A/B.
bool klattraDrapeFullQuad() {
    static const bool enabled = std::getenv("KLATTRA_DISABLE_DRAPE_FULLQUAD") == nullptr;
    return enabled;
}

// Metal needs per-drawable attribute storage here because a raster bucket can
// outlive the uploaded shared buffer it originally referenced. OpenGL's raw
// attribute path, however, does not preserve the interleaved raster layout and
// silently draws no pixels into terrain drape targets. Keep the Metal safety
// workaround while using the normal shared raster vertices on OpenGL.
constexpr bool klattraDrapeUsesLocalVertexBuffers() {
#if MLN_RENDER_BACKEND_OPENGL
    return false;
#else
    return true;
#endif
}

// .63: raster drape gap-fill. A drape canvas recreated by a ring resize
// (~490 per flight) can only re-acquire raster content from tiles in the
// CURRENT render set — behind the advancing camera the raster cover has
// moved on, so the emit path below never repaints it and the canvas
// renders its cleared background through the relief shader forever
// (phone-62 rasterEmpty census: 19–48 bound canvases per instant, stable
// ids, revokes=0, tiles curl-verified present at the provider — the
// content is never revoked, it dies with the pre-resize canvas and
// nothing re-routes). Far-field canvases born outside the raster cover
// (standing waitCover cohort) are the same gap from the other side.
// Gap-fill routes the deepest loaded ancestor-or-equal tile from the
// source's FULL pyramid (active set + retired cache, ~350 tiles) into
// any live canvas with no raster drape content. Painter's order and the
// .55/.57 supersede machinery replace it the moment fresher content
// routes. Kill switch for A/B.
bool klattraDrapeGapfillEnabled() {
    static const bool enabled = std::getenv("KLATTRA_DISABLE_DRAPE_GAPFILL") == nullptr;
    return enabled;
}

// Deepest zoom, relative to a drape target, that a non-authoritative raster
// mosaic routes. Three levels lets the near z12 DEM targets take the z15
// aerial tiles loaded around a close camera.
uint8_t klattraDrapeMosaicMaxDetailDepth() {
    static const uint8_t depth = [] {
        const char* v = std::getenv("KLATTRA_DRAPE_MOSAIC_DEPTH");
        return static_cast<uint8_t>(v && *v ? std::clamp(std::atoi(v), 0, 4) : 3);
    }();
    return depth;
}

bool klattraDrapeHasRasterFloor(TileLayerGroup& group, const OverscaledTileID& drapeID) {
    bool hasFloor = false;
    visitLayerGroupDrawables(group, [&](gfx::Drawable& drawable) {
        const auto& tileID = drawable.getTileID();
        if (tileID && tileID->wrap == drapeID.wrap && tileID->canonical.z <= drapeID.canonical.z &&
            LayerTweaker::tilesOverlap(*tileID, drapeID)) {
            hasFloor = true;
        }
    });
    return hasFloor;
}

// Device-visible probe emitter (Warning reaches the phone syslog; stderr
// covers the simulator, which swallows mbgl Log::Warning).
void klattraGapfillEmit(const std::string& message) {
    Log::Warning(Event::Render, message);
    if (std::getenv("KLATTRA_TRACE_STDERR") != nullptr) {
        fprintf(stderr, "[KLATTRA_TRACE] %s\n", message.c_str());
    }
}

std::string klattraDrapeIDString(const OverscaledTileID& id) {
    return "z" + std::to_string(static_cast<int>(id.canonical.z)) + "/" + std::to_string(id.canonical.x) + "/" +
           std::to_string(id.canonical.y);
}

std::string klattraRasterZoomHistogramString(const std::array<uint32_t, 26>& counts) {
    std::string out;
    for (std::size_t z = 0; z < counts.size(); ++z) {
        if (!counts[z]) continue;
        if (!out.empty()) out += ' ';
        out += 'z' + std::to_string(z) + ':' + std::to_string(counts[z]);
    }
    return out.empty() ? std::string("-") : out;
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
    // .63: drape gap-fill routes from the source's full pyramid, not just
    // the render set. Same lifetime as renderTiles (re-captured per frame).
    drapeGapfillSource = params.source;

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
    const bool authoritativeTerrainRaster = raster_drape::isAuthoritativeTerrainRaster(getID(), baseImpl->source);
    if (activeTerrain && authoritativeTerrainRaster) {
        activeTerrain->visitDrapeTargets([&](const OverscaledTileID&, TerrainDrapeTargetPtr& target) {
            if (target) target->requireRasterDrapeContent(layerIndex, baseImpl->source);
        });
    }

    if ((!renderTiles || renderTiles->empty()) && !imageData) {
        if (layerGroup) {
            stats.drawablesRemoved += layerGroup->clearDrawables();
        }
        if (imageLayerGroup) {
            stats.drawablesRemoved += imageLayerGroup->clearDrawables();
        }
        // A terrain target can outlive the raster source's instantaneous
        // render cover. Continue into the cache-backed recovery transaction;
        // otherwise a resized target can never reacquire its floor while the
        // camera is paused or looking behind the raster cover.
        if (!activeTerrain || !drapeGapfillSource) return;
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
    const auto buildVertexData = [&](const gfx::UniqueDrawableBuilder& builder,
            gfx::Drawable* drawable,
            const RasterBucket& bucket,
            bool freshIndexBuffer = false,
            bool localVertexBuffers = false,
            bool fullTileQuad = false) {
            // The bucket may later add, remove, or change masking.  In that case, the tile's
            // shared data and segments are not updated, and it needs to be re-created.
            if (drawable && bucket.sharedVertices->isModifiedAfter(drawable->createTime)) {
                return false;
            }

            // The bucket only fills in geometry for masked tiles,
            // otherwise the standard tile extent geometry should be used.
            // .57: drape drawables force the full-extent quad. The mask is a
            // MAIN-pass overdraw optimisation computed against the global
            // rendered set — inside a drape canvas it left every masked-out
            // parent region unwritten whenever the corresponding child's
            // drape drawable wasn't in THIS canvas yet, and unwritten canvas
            // pixels render the style-background fallback (the flyover's
            // green patches). Painter's order (drawPriority = z, set at the
            // emit site) keeps coarse under fine, so full-quad overdraw is
            // visually correct and costs one extra textured quad per parent.
        const bool shared = !fullTileQuad && (!bucket.sharedVertices->empty() && !bucket.sharedTriangles->empty() &&
                                 !bucket.segments.empty());
            const auto& vertices = shared ? bucket.sharedVertices : staticDataVertices;
            const auto& indices = shared ? bucket.sharedTriangles : staticDataIndices;
        const auto drawableIndices = freshIndexBuffer && shared
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
                if (auto& attr = vertexAttrs->set(idRasterPosVertexAttribute,
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

                if (auto& attr = vertexAttrs->set(idRasterTexturePosVertexAttribute,
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
            drawable->updateVertexAttributes(vertexAttrs,
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
            activeTerrain->visitDrapeTargets([&](const OverscaledTileID& drapeID, TerrainDrapeTargetPtr& drapeTarget) {
                    if (!drapeTarget) return;
                if (authoritativeTerrainRaster) {
                    drapeTarget->requireRasterDrapeContent(layerIndex, baseImpl->source);
                }
                    liveDrapeIDs.insert(drapeID);
                if (auto* drapeGroup = static_cast<TileLayerGroup*>(drapeTarget->getLayerGroup(layerIndex).get())) {
                        std::size_t removedNotInCover = 0;
                        std::size_t removedNoOverlap = 0;
                    const auto removed = drapeGroup->removeDrawablesIf([&](gfx::Drawable& drawable) {
                                const auto& dID = drawable.getTileID();
                                if (!dID) return false;
                                if (!hasRenderTile(*dID)) {
                                    // .55: KEEP content whose raster tile left
                                    // the raster cover. The drape cover is
                                    // wider than the raster cover, so fringe
                                    // canvases have no replacement — removing
                                    // here un-readied them permanently (the
                                    // phone-54 flight: 1187 revocations, ~100
                                    // canvases stuck plating = the green
                                    // patches; same bug class as the
                                    // 2026-07-04 evaporating far-field fix).
                                    // Stale drawables are superseded at emit
                                    // time the moment replacement content
                                    // routes; canvas prune still releases
                                    // everything when the canvas dies.
                                    if (!klattraKeepStaleRasterDrape()) {
                                        removedNotInCover++;
                                        return true;
                                    }
                                    return false;
                                }
                                if (!LayerTweaker::tilesOverlap(*dID, drapeID)) {
                                    removedNoOverlap++;
                                    return true;
                                }
                                return false;
                            });
                        stats.drawablesRemoved += removed;
                        // .54 stage diag: cover-shift revocation — the prime
                        // readiness-regression suspect.
                        activeTerrain->diagNoteRasterRevoked(drapeID, removed);
                        if (removed && klattraLogDrapeStale()) {
                            Log::Info(Event::Render,
                                  "[Klättra DRAPE_STALE] layer=" + getID() + " kind=raster drape=" +
                                      klattraDrapeIDString(drapeID) + " removed=" + std::to_string(removed) +
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
        std::size_t probeTiles = 0, probeBypassed = 0, probeBackfills = 0, probeBlockTiles = 0, probeOverlaps = 0,
                    probeAdded = 0, probeSkipNoTexSrc = 0, probeSkipBuilderTex = 0, probeRemovedStale = 0;
    for (const RenderTile& tile : *renderTiles) {
        ++probeTiles;
            const auto& tileID = tile.getOverscaledTileID();

            auto* bucket_ = tile.getBucket(*baseImpl);
            if (!bucket_) {
                removeTile(renderPass, tileID);
                continue;
            }

            bool cleared = false;
            auto& bucket = static_cast<RasterBucket&>(*bucket_);
            if (!raster_drape::bucketHasTextureSource(bucket)) {
                removeTile(renderPass, tileID);
                continue;
            }

            if (setRenderTileBucketID(tileID, bucket.getID())) {
                // Bucket ID changed, we need to rebuild the drawables
                removeTile(renderPass, tileID);
                // Also drop drape-pass drawables for this tile (see
                // render_fill_layer.cpp comment). .62: only when keep-stale
                // is off — with it on, the emit path's same-id removal
                // supersedes these drawables the moment the rebuilt bucket
                // routes, so dropping them here just opens a contentless
                // window (and if the new bucket never becomes paintable, a
                // permanent one). Content leaves a canvas only when
                // replaced.
                if (activeTerrain && !klattraKeepStaleRasterDrape()) {
                    activeTerrain->visitDrapeTargets(
                        [&](const OverscaledTileID& drapeID, TerrainDrapeTargetPtr& drapeTarget) {
                            if (!drapeTarget) return;
                            if (auto* drapeGroup = static_cast<TileLayerGroup*>(
                                    drapeTarget->getLayerGroup(layerIndex).get())) {
                                {
                            const auto removedStale = drapeGroup->removeDrawables(renderPass, tileID).size();
                            stats.drawablesRemoved += removedStale;
                            probeRemovedStale += removedStale;
                            // .54 stage diag: stale-drop revocation.
                            activeTerrain->diagNoteRasterRevoked(drapeID, removedStale);
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
                        activeTerrain->visitDrapeTargets([&](const OverscaledTileID& drapeID,
                                                             TerrainDrapeTargetPtr& drapeTarget) {
                            if (drapeBackfillNeeded || !drapeTarget || !LayerTweaker::tilesOverlap(tileID, drapeID)) {
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

            // Terrain raster routing is finalized below from one full-pyramid
            // snapshot. Per-tile mutation here can publish 1-3 detail children
            // before their siblings arrive, which is the diagonal mixed-LOD
            // plate this transaction deliberately prevents.
                        }

        // Raster publication transaction. Every target retains one complete
        // ancestor/equal floor. z+1 detail is staged as exactly four canonical
        // children and replaces the prior detail cohort only after every builder
        // succeeds. No render frame can therefore observe a 1-3 child mosaic.
    std::size_t probeGapsNow = 0, probeGapFilledNow = 0, probeGapDryNow = 0;
    std::size_t probeFloorNeededNow = 0, probeFloorRoutedNow = 0, probeFloorDryNow = 0;
    if (activeTerrain && !klattraDisableRasterDrape() && klattraDrapeGapfillEnabled() && drapeGapfillSource) {
            std::vector<std::pair<OverscaledTileID, RasterBucket*>> candidates;
            std::vector<OverscaledTileID> candidateIDs;
            drapeGapfillSource->visitRasterTileBuckets([&](const OverscaledTileID& tileID, RasterBucket& bucket) {
                if (!raster_drape::bucketHasTextureSource(bucket)) return;
                candidates.emplace_back(tileID, &bucket);
                candidateIDs.push_back(tileID);
            });
            std::vector<OverscaledTileID> publicationReadyTargetIDs;
            std::unordered_map<OverscaledTileID, RasterBucket*> bucketByID;
            bucketByID.reserve(candidates.size());
            for (const auto& [candidateID, bucket] : candidates) {
                bucketByID.emplace(candidateID, bucket);
            }
            const uint8_t mosaicMaxDetailDepth = klattraDrapeMosaicMaxDetailDepth();

            const auto bucketForID = [&](const OverscaledTileID& tileID) -> RasterBucket* {
                for (const auto& [candidateID, bucket] : candidates) {
                    if (candidateID == tileID) return bucket;
                }
                return nullptr;
        };

        activeTerrain->visitDrapeTargets([&](const OverscaledTileID& drapeID, TerrainDrapeTargetPtr& drapeTarget) {
            if (!drapeTarget) return;
                if (authoritativeTerrainRaster) {
                    drapeTarget->requireRasterDrapeContent(layerIndex, baseImpl->source);
                }
                auto& tw = drapeLayerTweakers[drapeID];
                if (!tw) {
                    tw = std::make_shared<RasterLayerTweaker>(getID() + "-drape", evaluatedProperties, drapeID);
                }

                auto* drapeGroup = static_cast<TileLayerGroup*>(drapeTarget->getLayerGroup(layerIndex).get());
                const bool wasEmpty = !drapeGroup || drapeGroup->getDrawableCount() == 0;
                if (wasEmpty) ++probeGapsNow;

                const auto selection = raster_drape::selectCohort(drapeID, candidateIDs);
                const auto stageDrawables = [&](const OverscaledTileID& tileID,
                                                RasterBucket& bucket) -> std::vector<gfx::UniqueDrawable> {
                    auto stagedBuilder = createBuilder();
                    if (!stagedBuilder) return {};
                    setTextures(stagedBuilder, bucket);
                    if (!stagedBuilder->getTexture(idRasterImage0Texture) ||
                        !stagedBuilder->getTexture(idRasterImage1Texture)) {
                        ++probeSkipBuilderTex;
                        return {};
                }
                    buildVertexData(stagedBuilder,
                                /*drawable=*/nullptr,
                                bucket,
                                /*freshIndexBuffer=*/true,
                                    /*localVertexBuffers=*/klattraDrapeUsesLocalVertexBuffers(),
                                /*fullTileQuad=*/klattraDrapeFullQuad());
                    stagedBuilder->flush(context);
                    auto drawables = stagedBuilder->clearDrawables();
                    for (auto& drawable : drawables) {
                        drawable->setTileID(tileID);
                        drawable->setLayerTweaker(tw);
                        drawable->setDrawPriority(static_cast<gfx::DrawPriority>(tileID.canonical.z));
                    }
                    return drawables;
                };
                const auto ensureGroup = [&]() -> TileLayerGroup* {
                    if (drapeGroup) return drapeGroup;
                    auto newGroup = context.createTileLayerGroup(
                        layerIndex, /*initialCapacity=*/5, getID() + "-raster-drape");
                    if (!newGroup) return nullptr;
                    newGroup->addLayerTweaker(tw);
                    drapeTarget->addLayerGroup(newGroup, /*replace=*/false);
                    drapeGroup = newGroup.get();
                    return drapeGroup;
                };
                const auto groupUsesCurrentTexture = [&](const OverscaledTileID& tileID,
                                                         const RasterBucket& bucket) -> bool {
                    if (!drapeGroup || !bucket.texture2d) return false;
                    bool found = false;
                    bool allCurrent = true;
                    visitLayerGroupDrawables(*drapeGroup, [&](gfx::Drawable& drawable) {
                        if (!drawable.getTileID() || *drawable.getTileID() != tileID) return;
                        found = true;
                        allCurrent = allCurrent &&
                                     raster_drape::textureIdentityMatches(
                                         bucket.texture2d.get(), drawable.getTexture(idRasterImage0Texture).get()) &&
                                     raster_drape::textureIdentityMatches(
                                         bucket.texture2d.get(), drawable.getTexture(idRasterImage1Texture).get());
                    });
                    return found && allCurrent;
                };

                if (!authoritativeTerrainRaster) {
                    // Only the authoritative terrain raster runs the floor +
                    // quartet transaction below. Every other raster layer
                    // (the native aerial blend, country overviews) routes a
                    // mosaic: requiring an ancestor-or-equal floor left every
                    // z12 target near the camera dry (the blend loads z13-15
                    // there), so terrain never published and Android 3D fell
                    // back to the flat map (traska.100).
                    const auto mosaic = raster_drape::selectMosaic(drapeID, candidateIDs, mosaicMaxDetailDepth);
                    std::unordered_set<OverscaledTileID> wanted(mosaic.details.begin(), mosaic.details.end());
                    if (mosaic.floor) wanted.insert(*mosaic.floor);
                    std::unordered_map<OverscaledTileID, const gfx::Texture2D*> routed;
                    if (drapeGroup) {
                        visitLayerGroupDrawables(*drapeGroup, [&](gfx::Drawable& drawable) {
                            if (const auto& tileID = drawable.getTileID()) {
                                routed[*tileID] = drawable.getTexture(idRasterImage0Texture).get();
                            }
                        });
                        stats.drawablesRemoved += drapeGroup->removeDrawablesIf([&](gfx::Drawable& drawable) {
                            const auto& tileID = drawable.getTileID();
                            if (!tileID || wanted.contains(*tileID)) return false;
                            return raster_drape::mosaicSupersedes(mosaic, drapeID, *tileID);
                        });
                    }
                    bool anyRouted = false;
                    const auto route = [&](const OverscaledTileID& tileID) {
                        const auto bucket = bucketByID.find(tileID);
                        if (bucket == bucketByID.end()) return;
                        const auto existing = routed.find(tileID);
                        if (existing != routed.end() && bucket->second->texture2d &&
                            raster_drape::textureIdentityMatches(bucket->second->texture2d.get(), existing->second)) {
                            return;
                        }
                        auto staged = stageDrawables(tileID, *bucket->second);
                        if (staged.empty() || !ensureGroup()) return;
                        stats.drawablesRemoved += drapeGroup->removeDrawables(renderPass, tileID).size();
                        for (auto& drawable : staged) {
                            drapeGroup->addDrawable(renderPass, tileID, std::move(drawable));
                            ++stats.drawablesAdded;
                            ++probeGapFilledNow;
                            ++probeAdded;
                        }
                        activeTerrain->diagNoteRasterRouted(drapeID, tileID.canonical.z);
                        anyRouted = true;
                    };
                    if (mosaic.floor) {
                        route(*mosaic.floor);
                    } else {
                        ++probeFloorNeededNow;
                    }
                    for (const auto& detail : mosaic.details) {
                        route(detail);
                    }
                    if (wasEmpty && !anyRouted) {
                        ++probeGapDryNow;
                    }
                    return;
                }

                bool hasFloor = drapeGroup && klattraDrapeHasRasterFloor(*drapeGroup, drapeID);
                bool hasCurrentFloor = false;
                if (!hasFloor) ++probeFloorNeededNow;
                if (selection.floor) {
                    auto* floorBucket = bucketForID(*selection.floor);
                    hasCurrentFloor = floorBucket && groupUsesCurrentTexture(*selection.floor, *floorBucket);
                    if (!hasCurrentFloor && floorBucket) {
                        auto stagedFloor = stageDrawables(*selection.floor, *floorBucket);
                        if (!stagedFloor.empty() && ensureGroup()) {
                            stats.drawablesRemoved += drapeGroup->removeDrawablesIf([&](gfx::Drawable& drawable) {
                                const auto& tileID = drawable.getTileID();
                                return tileID && raster_drape::tileCoversTarget(*tileID, drapeID);
                            });
                            for (auto& drawable : stagedFloor) {
                                drapeGroup->addDrawable(renderPass, *selection.floor, std::move(drawable));
                                ++stats.drawablesAdded;
                                ++probeGapFilledNow;
                                ++probeAdded;
                            }
                            activeTerrain->diagNoteRasterRouted(drapeID, selection.floor->canonical.z);
                            ++probeFloorRoutedNow;
                            const int dz = static_cast<int>(drapeID.canonical.z) -
                                           static_cast<int>(selection.floor->canonical.z);
                            drapeGapfillDzHist[static_cast<std::size_t>(std::clamp(dz + 1, 0, 5))]++;
                            hasFloor = true;
                            hasCurrentFloor = true;
                        }
                    } else if (hasCurrentFloor) {
                        hasFloor = true;
                    }
                }
                if (!hasFloor) {
                    ++probeFloorDryNow;
                    ++probeGapDryNow;
                    return;
                }

                std::vector<OverscaledTileID> existingDetailIDs;
                if (drapeGroup) {
                    visitLayerGroupDrawables(*drapeGroup, [&](gfx::Drawable& drawable) {
                        const auto& tileID = drawable.getTileID();
                        if (tileID && raster_drape::tileIsStrictChildOf(*tileID, drapeID)) {
                            existingDetailIDs.push_back(*tileID);
                    }
                    });
                }
                const auto existingDetail = raster_drape::selectCohort(drapeID, existingDetailIDs);

                if (!selection.hasCompleteChildren()) {
                    // Keep a previously committed quartet. Only remove legacy
                    // partial children; the full-canvas floor remains visible.
                    if (!existingDetail.hasCompleteChildren() && drapeGroup) {
                        stats.drawablesRemoved += drapeGroup->removeDrawablesIf([&](gfx::Drawable& drawable) {
                            const auto& tileID = drawable.getTileID();
                            return tileID && raster_drape::tileIsStrictChildOf(*tileID, drapeID);
                        });
                    }
                    if (authoritativeTerrainRaster && hasCurrentFloor) {
                        publicationReadyTargetIDs.push_back(drapeID);
                    }
                    return;
                }

                bool sameDetailCohort = existingDetail.hasCompleteChildren();
                for (std::size_t i = 0; sameDetailCohort && i < selection.children.size(); ++i) {
                    sameDetailCohort = existingDetail.children[i] == selection.children[i];
                    if (sameDetailCohort) {
                        auto* childBucket = bucketForID(*selection.children[i]);
                        sameDetailCohort = childBucket &&
                                           groupUsesCurrentTexture(*selection.children[i], *childBucket);
                    }
                }
                if (sameDetailCohort) {
                    if (authoritativeTerrainRaster && hasCurrentFloor) {
                        publicationReadyTargetIDs.push_back(drapeID);
                    }
                    return;
                }

                std::array<std::vector<gfx::UniqueDrawable>, 4> stagedChildren;
                bool allChildrenStaged = true;
                for (std::size_t i = 0; i < selection.children.size(); ++i) {
                    auto* childBucket = bucketForID(*selection.children[i]);
                    if (!childBucket) {
                        allChildrenStaged = false;
                        break;
                }
                    stagedChildren[i] = stageDrawables(*selection.children[i], *childBucket);
                    if (stagedChildren[i].empty()) {
                        allChildrenStaged = false;
                        break;
                }
            }
                if (!allChildrenStaged || !ensureGroup()) return;

                stats.drawablesRemoved += drapeGroup->removeDrawablesIf([&](gfx::Drawable& drawable) {
                    const auto& tileID = drawable.getTileID();
                    return tileID && raster_drape::tileIsStrictChildOf(*tileID, drapeID);
                });
                for (std::size_t i = 0; i < stagedChildren.size(); ++i) {
                    for (auto& drawable : stagedChildren[i]) {
                        drapeGroup->addDrawable(renderPass, *selection.children[i], std::move(drawable));
                        ++stats.drawablesAdded;
                        ++probeGapFilledNow;
                        ++probeAdded;
        }
                    activeTerrain->diagNoteRasterRouted(drapeID, selection.children[i]->canonical.z);
                }
                drapeGapfillDzHist[0] += 4;
                if (authoritativeTerrainRaster && hasCurrentFloor) {
                    publicationReadyTargetIDs.push_back(drapeID);
                }
            });

            if (authoritativeTerrainRaster) {
                activeTerrain->noteDrapeSourceTargetReadiness(baseImpl->source, publicationReadyTargetIDs);
            }

            // CPU-only DIAG census of the surface actually stored by each
            // target after the transaction. Per-target completeness cannot
            // reveal a z/z+1 discontinuity between neighboring targets, which
            // is the remaining giant diagonal plate seen on Android.
            if (authoritativeTerrainRaster && RenderTerrain::stageDiagEnabled()) {
                std::vector<std::pair<OverscaledTileID, uint8_t>> resolvedSurfaceZooms;
                std::size_t unresolvedTargets = 0;
                activeTerrain->visitDrapeTargets(
                    [&](const OverscaledTileID& targetID, TerrainDrapeTargetPtr& target) {
                        if (!target) return;
                        const auto surfaceZoom = target->diagnosticRasterSurfaceZoomForTile(targetID, layerIndex);
                        if (surfaceZoom) {
                            resolvedSurfaceZooms.emplace_back(targetID, *surfaceZoom);
                        } else {
                            ++unresolvedTargets;
                        }
                    });

                std::array<uint32_t, 26> surfaceZoomHistogram{};
                for (const auto& [targetID, surfaceZoom] : resolvedSurfaceZooms) {
                    (void)targetID;
                    if (surfaceZoom < surfaceZoomHistogram.size()) surfaceZoomHistogram[surfaceZoom]++;
                }

                std::size_t neighborPairs = 0;
                std::size_t mixedNeighborPairs = 0;
                std::string mixedSample;
                for (auto a = resolvedSurfaceZooms.begin(); a != resolvedSurfaceZooms.end(); ++a) {
                    for (auto b = std::next(a); b != resolvedSurfaceZooms.end(); ++b) {
                        const auto& aID = a->first;
                        const auto& bID = b->first;
                        if (aID.wrap != bID.wrap || aID.canonical.z != bID.canonical.z ||
                            aID.overscaledZ != bID.overscaledZ) {
                            continue;
                        }
                        const auto dx = aID.canonical.x > bID.canonical.x ? aID.canonical.x - bID.canonical.x
                                                                          : bID.canonical.x - aID.canonical.x;
                        const auto dy = aID.canonical.y > bID.canonical.y ? aID.canonical.y - bID.canonical.y
                                                                          : bID.canonical.y - aID.canonical.y;
                        if (dx + dy != 1u) continue;
                        ++neighborPairs;
                        if (a->second == b->second) continue;
                        ++mixedNeighborPairs;
                        if (mixedSample.empty()) {
                            mixedSample = klattraDrapeIDString(aID) + "@z" + std::to_string(a->second) + "," +
                                          klattraDrapeIDString(bID) + "@z" + std::to_string(b->second);
                        }
                    }
                }

                static std::chrono::steady_clock::time_point lastCohortEmit{};
                const auto now = std::chrono::steady_clock::now();
                if (now - lastCohortEmit >= std::chrono::seconds(1)) {
                    lastCohortEmit = now;
                    klattraGapfillEmit(
                        "[KLATTRA RASTER_COHORT] layer=" + getID() +
                        " targets=" + std::to_string(resolvedSurfaceZooms.size() + unresolvedTargets) +
                        " resolved=" + std::to_string(resolvedSurfaceZooms.size()) +
                        " unresolved=" + std::to_string(unresolvedTargets) +
                        " surfaceZ=" + klattraRasterZoomHistogramString(surfaceZoomHistogram) +
                        " neighborPairs=" + std::to_string(neighborPairs) +
                        " mixedNeighbors=" + std::to_string(mixedNeighborPairs) +
                        " sample=" + (mixedSample.empty() ? std::string("-") : mixedSample));
                }
            }

        drapeGapfillFilled += probeGapFilledNow;
        // 1 Hz device-visible gauge (STAGE family). filled is
        // cumulative; gaps/dry are this-update gauges — dry stays hot
        // for canvases with NO loaded candidate anywhere (e.g. a layer
        // whose tileset never covered the region), so a standing dry
        // count names the residual population the fix cannot reach.
        if (RenderTerrain::stageDiagEnabled() &&
            (probeGapsNow || drapeGapfillFilled || probeFloorNeededNow || probeFloorDryNow)) {
            static std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastEmitByLayer;
            auto& lastEmit = lastEmitByLayer[getID()];
            const auto nowG = std::chrono::steady_clock::now();
            if (nowG - lastEmit >= std::chrono::seconds(1)) {
                lastEmit = nowG;
                klattraGapfillEmit(
                    "[KLATTRA STAGE] gapfill layer=" + getID() + " gaps=" + std::to_string(probeGapsNow) +
                    " filled=" + std::to_string(drapeGapfillFilled) + " dry=" + std::to_string(probeGapDryNow) +
                    " floorNeed=" + std::to_string(probeFloorNeededNow) + " floorRouted=" +
                    std::to_string(probeFloorRoutedNow) + " floorDry=" + std::to_string(probeFloorDryNow) +
                        " dz=c1:" + std::to_string(drapeGapfillDzHist[0]) + ",eq:" +
                        std::to_string(drapeGapfillDzHist[1]) + ",a1:" + std::to_string(drapeGapfillDzHist[2]) +
                        ",a2:" + std::to_string(drapeGapfillDzHist[3]) + ",a3:" +
                        std::to_string(drapeGapfillDzHist[4]) + ",a4p:" + std::to_string(drapeGapfillDzHist[5]));
            }
        }
    }

        // 1 Hz probe summary (stderr — the simulator swallows Log::Warning).
        if (std::getenv("KLATTRA_TRACE_STDERR") != nullptr && (probeTiles || probeBlockTiles || probeAdded)) {
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
