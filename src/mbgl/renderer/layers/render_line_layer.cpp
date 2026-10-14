#include <mbgl/renderer/layers/render_line_layer.hpp>

#include <mbgl/geometry/feature_index.hpp>
#include <mbgl/geometry/line_atlas.hpp>
#include <mbgl/gfx/cull_face_mode.hpp>
#include <mbgl/gfx/shader_registry.hpp>
#include <mbgl/renderer/buckets/line_bucket.hpp>
#include <mbgl/renderer/image_manager.hpp>
#include <mbgl/renderer/paint_parameters.hpp>
#include <mbgl/renderer/render_source.hpp>
#include <mbgl/renderer/render_tile.hpp>
#include <mbgl/renderer/tile_render_data.hpp>
#include <mbgl/renderer/upload_parameters.hpp>
#include <mbgl/style/expression/image.hpp>
#include <mbgl/style/layers/line_layer_impl.hpp>
#include <mbgl/tile/geometry_tile.hpp>
#include <mbgl/tile/tile.hpp>
#include <mbgl/util/convert.hpp>
#include <mbgl/util/intersection_tests.hpp>
#include <mbgl/util/logging.hpp>
#include <mbgl/util/math.hpp>

#include <mbgl/gfx/drawable_atlases_tweaker.hpp>
#include <mbgl/gfx/drawable_builder.hpp>
#include <mbgl/gfx/line_drawable_data.hpp>
#include <mbgl/renderer/layer_group.hpp>
#include <mbgl/renderer/layers/line_layer_tweaker.hpp>
#include <mbgl/renderer/render_terrain.hpp>
#include <mbgl/renderer/terrain_line_lift.hpp>
#include <mbgl/renderer/update_parameters.hpp>
#include <mbgl/shaders/line_layer_ubo.hpp>
#include <mbgl/shaders/shader_program_base.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <unordered_set>

namespace mbgl {

using namespace style;
using namespace shaders;

namespace {

bool klattraLogDrapeStale() {
    return std::getenv("KLATTRA_LOG_DRAPE_STALE") != nullptr;
}

bool klattraDisableLineDrape() {
    static const bool disabled = std::getenv("KLATTRA_DISABLE_LINE_DRAPE") != nullptr;
    return disabled;
}

std::string klattraDrapeIDString(const OverscaledTileID& id) {
    return "z" + std::to_string(static_cast<int>(id.canonical.z)) + "/" +
           std::to_string(id.canonical.x) + "/" + std::to_string(id.canonical.y);
}

inline const LineLayer::Impl& impl_cast(const Immutable<style::Layer::Impl>& impl) {
    assert(impl->getTypeInfo() == LineLayer::Impl::staticTypeInfo());
    return static_cast<const LineLayer::Impl&>(*impl);
}

const auto posNormalAttribName = "a_pos_normal";

// Marks the lifted line shader variant in the program cache key; matches no
// vertex attribute, so it changes nothing else.
constexpr std::size_t liftedVariantSentinelID = static_cast<std::size_t>(lineVertexAttributeCount) + 1;

// KLATTRA_LIFTED_LINES_STATS=1: one Warning line per second with the cost of
// re-sampling lifted line heights on the render thread.
struct LiftedLineStats {
    bool enabled = std::getenv("KLATTRA_LIFTED_LINES_STATS") != nullptr;
    std::chrono::steady_clock::time_point windowStart = std::chrono::steady_clock::now();
    uint64_t checks = 0, keyHits = 0, resamples = 0, vertices = 0, uploads = 0, unknown = 0;
    // Per lifted layer and tile, while terrain is on: lifted under one
    // coarser-or-equal terrain tile, lifted over finer terrain tiles only,
    // drawn flat (ready terrain covers it partly or not at all).
    uint64_t liftedTiles = 0, partialTiles = 0, flatTiles = 0;
    double sampleMs = 0.0;

    void maybeEmit() {
        const auto now = std::chrono::steady_clock::now();
        if (now - windowStart < std::chrono::seconds(1)) return;
        const std::string line = "[KLATTRA LIFTED] checks=" + std::to_string(checks) +
                                 " keyHits=" + std::to_string(keyHits) + " resamples=" + std::to_string(resamples) +
                                 " vertices=" + std::to_string(vertices) + " uploads=" + std::to_string(uploads) +
                                 " unknown=" + std::to_string(unknown) + " sampleMs=" + std::to_string(sampleMs) +
                                 " tiles=" + std::to_string(liftedTiles) + "/" + std::to_string(partialTiles) + "/" +
                                 std::to_string(flatTiles);
        Log::Warning(Event::Render, line);
        // The simulator drops Log::Warning; KLATTRA_TRACE_STDERR mirrors it.
        static const bool traceStderr = std::getenv("KLATTRA_TRACE_STDERR") != nullptr;
        if (traceStderr) std::fprintf(stderr, "[KLATTRA_TRACE] %s\n", line.c_str());
        windowStart = now;
        checks = keyHits = resamples = vertices = uploads = unknown = 0;
        liftedTiles = partialTiles = flatTiles = 0;
        sampleMs = 0.0;
    }
};

LiftedLineStats& liftedLineStats() {
    static LiftedLineStats stats;
    return stats;
}

// Refreshes the bucket's per-vertex raw DEM heights for a terrain-lifted
// draw. Shared by every lifted layer drawing the bucket; re-samples only when
// the elevation inputs under this tile change (the global revision is only a
// fast path), and re-uploads only when a height actually moved.
void refreshLiftedElevations(LineBucket& bucket,
                             const RenderTile& tile,
                             const RenderTerrain& terrain,
                             float emptyPlaneElevation) {
    auto& stats = liftedLineStats();
    if (stats.enabled) {
        ++stats.checks;
        stats.maybeEmit();
    }
    const auto revision = terrain.getElevationRevision();
    const auto vertexCount = bucket.vertices.elements();
    const bool sameBinding = bucket.sharedTerrainVertices && bucket.terrainElevationOwner == &terrain &&
                             bucket.terrainElevationTile == tile.id &&
                             bucket.sharedTerrainVertices->elements() == vertexCount;
    if (sameBinding && bucket.terrainElevationRevision == revision) {
        return;
    }
    // The revision is global: most changes come from other parts of the cover.
    const auto key = terrain.renderedElevationKey(tile.id, emptyPlaneElevation);
    if (sameBinding && bucket.terrainElevationKey == key) {
        bucket.terrainElevationRevision = revision;
        if (stats.enabled) ++stats.keyHits;
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    std::vector<Point<int16_t>> points;
    points.reserve(vertexCount);
    for (std::size_t i = 0; i < vertexCount; ++i) {
        // a_pos_normal packs (pos * 2) | flag; an arithmetic shift recovers pos.
        const auto& packed = std::as_const(bucket.vertices).at(i).a1;
        points.emplace_back(static_cast<int16_t>(packed[0] >> 1), static_cast<int16_t>(packed[1] >> 1));
    }
    auto heights = terrain.sampleRenderedElevations(tile.id, points, emptyPlaneElevation);
    // Points with no known surface (no DEM cached yet) take the nearest known
    // height along the vertex stream instead of jumping to the origin, which
    // can be kilometres off in the mountains.
    if (stats.enabled) {
        stats.unknown += static_cast<uint64_t>(std::count_if(heights.begin(), heights.end(), [](float h) {
            return std::isnan(h);
        }));
    }
    float carry = std::numeric_limits<float>::quiet_NaN();
    for (auto& h : heights) {
        if (std::isnan(h)) h = carry;
        else carry = h;
    }
    carry = std::numeric_limits<float>::quiet_NaN();
    for (auto it = heights.rbegin(); it != heights.rend(); ++it) {
        if (std::isnan(*it)) *it = std::isnan(carry) ? emptyPlaneElevation : carry;
        else carry = *it;
    }

    if (!bucket.sharedTerrainVertices) {
        bucket.sharedTerrainVertices = std::make_shared<LineBucket::TerrainVertexVector>();
    }
    auto& vertices = *bucket.sharedTerrainVertices;
    bool changed = vertices.elements() != heights.size();
    for (std::size_t i = 0; !changed && i < heights.size(); ++i) {
        changed = std::as_const(vertices).at(i).a1[0] != heights[i];
    }
    if (changed) {
        vertices.clear();
        vertices.reserve(heights.size());
        for (const float height : heights) {
            vertices.emplace_back(LineBucket::TerrainVertex{{{height}}});
        }
        vertices.updateModified();
    }
    bucket.terrainElevationOwner = &terrain;
    bucket.terrainElevationRevision = revision;
    bucket.terrainElevationKey = key;
    bucket.terrainElevationTile = tile.id;
    if (stats.enabled) {
        ++stats.resamples;
        stats.vertices += vertexCount;
        stats.uploads += changed ? 1 : 0;
        stats.sampleMs +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    }
}

} // namespace

RenderLineLayer::RenderLineLayer(Immutable<style::LineLayer::Impl> _impl)
    : RenderLayer(makeMutable<LineLayerProperties>(std::move(_impl))),
      unevaluated(impl_cast(baseImpl).paint.untransitioned()),
      colorRamp(std::make_shared<PremultipliedImage>(Size(256, 1))),
      terrainLifted(terrain_line_lift::isLiftedLayerID(baseImpl->id)) {
    styleDependencies = unevaluated.getDependencies();
}

RenderLineLayer::~RenderLineLayer() = default;

void RenderLineLayer::transition(const TransitionParameters& parameters) {
    unevaluated = impl_cast(baseImpl).paint.transitioned(parameters, std::move(unevaluated));
    styleDependencies = unevaluated.getDependencies();
    updateColorRamp();

#if MLN_RENDER_BACKEND_METAL
    if (auto* tweaker = static_cast<LineLayerTweaker*>(layerTweaker.get())) {
        tweaker->updateGPUExpressions(unevaluated, parameters.now);
    }
    if (liftedLayerTweaker) {
        liftedLayerTweaker->updateGPUExpressions(unevaluated, parameters.now);
    }
#endif // MLN_RENDER_BACKEND_METAL
}

void RenderLineLayer::evaluate(const PropertyEvaluationParameters& parameters) {
    const auto previousProperties = staticImmutableCast<LineLayerProperties>(evaluatedProperties);
    auto properties = makeMutable<LineLayerProperties>(staticImmutableCast<LineLayer::Impl>(baseImpl),
                                                       parameters.getCrossfadeParameters(),
                                                       unevaluated.evaluate(parameters, previousProperties->evaluated));
    auto& evaluated = properties->evaluated;

    passes = (evaluated.get<style::LineOpacity>().constantOr(1.0) > 0 &&
              evaluated.get<style::LineColor>().constantOr(Color::black()).a > 0 &&
              evaluated.get<style::LineWidth>().constantOr(1.0) > 0)
                 ? RenderPass::Translucent
                 : RenderPass::None;
    properties->renderPasses = mbgl::underlying_type(passes);
    evaluatedProperties = std::move(properties);

    if (auto* tweaker = static_cast<LineLayerTweaker*>(layerTweaker.get())) {
        tweaker->updateProperties(evaluatedProperties);
#if MLN_RENDER_BACKEND_METAL
        tweaker->updateGPUExpressions(unevaluated, parameters.now);
#endif // MLN_RENDER_BACKEND_METAL
    }
    // Mirror property updates to the per-drape-target tweakers — otherwise
    // style changes only land in the main pass and the draped surface
    // stays stale.
    for (auto& [_, tw] : drapeLayerTweakers) {
        if (tw) {
            tw->updateProperties(evaluatedProperties);
#if MLN_RENDER_BACKEND_METAL
            tw->updateGPUExpressions(unevaluated, parameters.now);
#endif
        }
    }
    if (liftedLayerTweaker) {
        liftedLayerTweaker->updateProperties(evaluatedProperties);
#if MLN_RENDER_BACKEND_METAL
        liftedLayerTweaker->updateGPUExpressions(unevaluated, parameters.now);
#endif
    }
}

void RenderLineLayer::markLayerRenderable(bool willRender, UniqueChangeRequestVec& changes) {
    RenderLayer::markLayerRenderable(willRender, changes);
    if (liftedLayerGroup) {
        activateLayerGroup(liftedLayerGroup, willRender, changes);
    }
}

void RenderLineLayer::layerRemoved(UniqueChangeRequestVec& changes) {
    RenderLayer::layerRemoved(changes);
    if (liftedLayerGroup) {
        activateLayerGroup(liftedLayerGroup, false, changes);
    }
}

void RenderLineLayer::layerIndexChanged(int32_t newLayerIndex, UniqueChangeRequestVec& changes) {
    RenderLayer::layerIndexChanged(newLayerIndex, changes);
    changeLayerIndex(liftedLayerGroup, newLayerIndex, changes);
}

std::size_t RenderLineLayer::removeTile(RenderPass renderPass, const OverscaledTileID& tileID) {
    auto removed = RenderLayer::removeTile(renderPass, tileID);
    if (liftedLayerGroup) {
        const auto n = liftedLayerGroup->removeDrawables(renderPass, tileID).size();
        stats.drawablesRemoved += n;
        removed += n;
    }
    return removed;
}

std::size_t RenderLineLayer::removeAllDrawables() {
    auto removed = RenderLayer::removeAllDrawables();
    if (liftedLayerGroup) {
        const auto n = liftedLayerGroup->getDrawableCount();
        stats.drawablesRemoved += n;
        liftedLayerGroup->clearDrawables();
        removed += n;
    }
    return removed;
}

bool RenderLineLayer::hasTransition() const {
    return unevaluated.hasTransition();
}

bool RenderLineLayer::hasCrossfade() const {
    return getCrossfade<LineLayerProperties>(evaluatedProperties).t != 1;
}

void RenderLineLayer::prepare(const LayerPrepareParameters& params) {
    RenderLayer::prepare(params);
    for (const RenderTile& tile : *renderTiles) {
        const LayerRenderData* renderData = tile.getLayerRenderData(*baseImpl);
        if (!renderData) continue;

        const auto& evaluated = getEvaluated<LineLayerProperties>(renderData->layerProperties);
        if (evaluated.get<LineDasharray>().from.empty()) continue;

        const auto& bucket = static_cast<const LineBucket&>(*renderData->bucket);
        const LinePatternCap cap = bucket.layout.get<LineCap>() == LineCapType::Round ? LinePatternCap::Round
                                                                                      : LinePatternCap::Square;
        // Ensures that the dash data gets added to the atlas.
        params.lineAtlas.getDashPatternTexture(
            evaluated.get<LineDasharray>().from, evaluated.get<LineDasharray>().to, cap);
    }
}

namespace {

GeometryCollection offsetLine(const GeometryCollection& rings, double offset) {
    assert(offset != 0.0f);
    assert(!rings.empty());

    GeometryCollection newRings;
    newRings.reserve(rings.size());

    const Point<double> zero(0, 0);
    for (const auto& ring : rings) {
        newRings.emplace_back();
        auto& newRing = newRings.back();
        newRing.reserve(ring.size());

        for (auto i = ring.begin(); i != ring.end(); ++i) {
            auto& p = *i;

            Point<double> aToB = i == ring.begin() ? zero : util::perp(util::unit(convertPoint<double>(p - *(i - 1))));
            Point<double> bToC = i + 1 == ring.end() ? zero
                                                     : util::perp(util::unit(convertPoint<double>(*(i + 1) - p)));
            Point<double> extrude = util::unit(aToB + bToC);

            const double cosHalfAngle = extrude.x * bToC.x + extrude.y * bToC.y;
            extrude *= (cosHalfAngle != 0) ? (1.0 / cosHalfAngle) : 0;

            newRing.emplace_back(convertPoint<int16_t>(extrude * offset) + p);
        }
    }

    return newRings;
}

} // namespace

bool RenderLineLayer::queryIntersectsFeature(const GeometryCoordinates& queryGeometry,
                                             const GeometryTileFeature& feature,
                                             const float zoom,
                                             const TransformState& transformState,
                                             const float pixelsToTileUnits,
                                             const mat4&,
                                             const FeatureState& featureState) const {
    const auto& evaluated = static_cast<const LineLayerProperties&>(*evaluatedProperties).evaluated;
    // Translate query geometry
    auto translatedQueryGeometry = FeatureIndex::translateQueryGeometry(queryGeometry,
                                                                        evaluated.get<style::LineTranslate>(),
                                                                        evaluated.get<style::LineTranslateAnchor>(),
                                                                        static_cast<float>(transformState.getBearing()),
                                                                        pixelsToTileUnits);

    // Evaluate function
    auto offset = evaluated.get<style::LineOffset>().evaluate(
                      feature, zoom, featureState, style::LineOffset::defaultValue()) *
                  pixelsToTileUnits;
    // Test intersection
    const auto halfWidth = static_cast<float>(getLineWidth(feature, zoom, featureState) / 2.0 * pixelsToTileUnits);

    // Apply offset to geometry
    if (offset != 0.0f && !feature.getGeometries().empty()) {
        return util::polygonIntersectsBufferedMultiLine(
            translatedQueryGeometry.value_or(queryGeometry), offsetLine(feature.getGeometries(), offset), halfWidth);
    }

    return util::polygonIntersectsBufferedMultiLine(
        translatedQueryGeometry.value_or(queryGeometry), feature.getGeometries(), halfWidth);
}

void RenderLineLayer::updateColorRamp() {
    const style::ColorRampPropertyValue colorValue = unevaluated.get<LineGradient>().getValue();
    if (!colorRamp || !applyColorRamp(colorValue, *colorRamp)) {
        return;
    }

    if (colorRampTexture2D) {
        colorRampTexture2D.reset();

        // delete all gradient drawables
        if (layerGroup) {
            stats.drawablesRemoved += layerGroup->getDrawableCount();
            layerGroup->clearDrawables();
        }
    }
}

float RenderLineLayer::getLineWidth(const GeometryTileFeature& feature,
                                    const float zoom,
                                    const FeatureState& featureState) const {
    const auto& evaluated = static_cast<const LineLayerProperties&>(*evaluatedProperties).evaluated;
    float lineWidth = evaluated.get<style::LineWidth>().evaluate(
        feature, zoom, featureState, style::LineWidth::defaultValue());
    float gapWidth = evaluated.get<style::LineGapWidth>().evaluate(
        feature, zoom, featureState, style::LineGapWidth::defaultValue());
    if (gapWidth) {
        return gapWidth + 2 * lineWidth;
    } else {
        return lineWidth;
    }
}

namespace {

inline void setSegments(std::unique_ptr<gfx::DrawableBuilder>& builder, const LineBucket& bucket) {
    builder->setSegments(gfx::Triangles(), bucket.sharedTriangles, bucket.segments.data(), bucket.segments.size());
}

} // namespace

void RenderLineLayer::update(gfx::ShaderRegistry& shaders,
                             gfx::Context& context,
                             const TransformState& state,
                             [[maybe_unused]] const std::shared_ptr<UpdateParameters>& parameters,
                             const RenderTree&,
                             UniqueChangeRequestVec& changes) {
    if (!renderTiles || renderTiles->empty()) {
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
        auto tweaker = std::make_shared<LineLayerTweaker>(getID(), evaluatedProperties);
#if MLN_RENDER_BACKEND_METAL
        tweaker->updateGPUExpressions(unevaluated, parameters->timePoint);
#endif // MLN_RENDER_BACKEND_METAL

        layerTweaker = std::move(tweaker);
        layerGroup->addLayerTweaker(layerTweaker);
    }

    const RenderPass renderPass = static_cast<RenderPass>(evaluatedProperties->renderPasses &
                                                          ~mbgl::underlying_type(RenderPass::Opaque));

    // Terrain-lifted layers: 3D drawables live in their own group so only they
    // are deferred past the terrain mesh; tiles without ready terrain stay in
    // the flat main pass like every other layer.
    TileLayerGroup* liftedGroup = nullptr;
    if (terrainLifted && activeTerrain) {
        if (!liftedLayerGroup) {
            liftedLayerGroup = context.createTileLayerGroup(layerIndex, /*initialCapacity=*/64, getID() + "-lifted");
            if (liftedLayerGroup) {
                activateLayerGroup(liftedLayerGroup, true, changes);
            }
        }
        if (liftedLayerGroup && !liftedLayerTweaker) {
            liftedLayerTweaker = std::make_shared<LineLayerTweaker>(getID() + "-lifted", evaluatedProperties);
            liftedLayerTweaker->setTerrainLifted(true);
#if MLN_RENDER_BACKEND_METAL
            liftedLayerTweaker->updateGPUExpressions(unevaluated, parameters->timePoint);
#endif // MLN_RENDER_BACKEND_METAL
            liftedLayerGroup->addLayerTweaker(liftedLayerTweaker);
        }
        liftedGroup = liftedLayerGroup.get();
    } else if (liftedLayerGroup && liftedLayerGroup->getDrawableCount() > 0) {
        // Terrain off: the flat main pass carries the layer again.
        stats.drawablesRemoved += liftedLayerGroup->getDrawableCount();
        liftedLayerGroup->clearDrawables();
    }
    // Raw height that renders at the terrain's zero plane, where empty-DEM
    // tiles lie: the elevation origin, with the terrain tweaker's fallback.
    float liftedEmptyPlaneElevation = 0.0f;
    static const bool absoluteHeights = std::getenv("KLATTRA_TERRAIN_ABSOLUTE_HEIGHTS") != nullptr;
    if (liftedGroup && !absoluteHeights) {
        const float fallbackOrigin = std::abs(state.getLatLng().latitude()) >= 60.0 ? 1000.0f : 0.0f;
        liftedEmptyPlaneElevation = activeTerrain->getElevationOriginMeters().value_or(fallbackOrigin);
    }
    // Only plain and dashed lines have the lifted shader path.
    const bool liftableVariant = unevaluated.get<LinePattern>().isUndefined() &&
                                 unevaluated.get<LineGradient>().getValue().isUndefined();
    // Lift a tile wherever ready terrain covers it, also when only several
    // finer terrain tiles do (hasLiftedLineCoverage): the terrain cover keeps
    // finer tiles further out than vector sources do, and a tile drawn flat
    // there would lie under the mesh. Heights come per vertex from whichever
    // terrain tile is under the point.
    const auto liftableTile = [&](const OverscaledTileID& id) {
        return liftedGroup && liftableVariant && activeTerrain->hasLiftedLineCoverage(id);
    };
    if (liftedGroup) {
        stats.drawablesRemoved += liftedGroup->removeDrawablesIf([&](gfx::Drawable& drawable) {
            const auto& tileID = drawable.getTileID();
            return drawable.getRenderPass() != passes || !tileID || !hasRenderTile(*tileID) ||
                   !liftableTile(*tileID);
        });
    }

    stats.drawablesRemoved += tileLayerGroup->removeDrawablesIf([&](gfx::Drawable& drawable) {
        // If the render pass has changed or the tile has  dropped out of the cover set, remove it.
        const auto& tileID = drawable.getTileID();
        if (drawable.getRenderPass() != passes || (tileID && !hasRenderTile(*tileID))) {
            return true;
        }
        return false;
    });

    // When terrain is active, main-pass line drawables for tiles with DEM
    // coverage would render flat at z=0 and bleed through past the terrain
    // mesh edges. Drop those — the drape variant emitted inside the per-tile
    // loop still paints them onto the terrain mesh. Keep main-pass drawables
    // for tiles without DEM coverage so the layer doesn't vanish at zooms
    // below the DEM source's range. See `render_fill_layer.cpp` for the
    // matching rationale and the gl-js `LAYERS_TO_TEXTURES` precedent in
    // `src/webgl/render_to_texture.ts`.
    if (activeTerrain) {
        stats.drawablesRemoved += tileLayerGroup->removeDrawablesIf([&](gfx::Drawable& drawable) {
            const auto& tileID = drawable.getTileID();
            return tileID && (activeTerrain->hasElevationCoverage(*tileID) || liftableTile(*tileID));
        });
    }

    // Mirror the same cover-set cleanup for every drape group this layer
    // owns inside the terrain drape cache, and drop drapeLayerTweakers
    // entries whose drape target has been evicted.
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
                                      " kind=line drape=" + klattraDrapeIDString(drapeID) +
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

    auto createLineBuilder = [&](const std::string& name,
                                 gfx::ShaderPtr shader) -> std::unique_ptr<gfx::DrawableBuilder> {
        std::unique_ptr<gfx::DrawableBuilder> builder = context.createDrawableBuilder(name);
        builder->setShader(std::static_pointer_cast<gfx::ShaderProgramBase>(shader));
        builder->setRenderPass(renderPass);
        builder->setSubLayerIndex(0);
        builder->setDepthType(gfx::DepthMaskType::ReadOnly);
        builder->setColorMode(renderPass == RenderPass::Translucent ? gfx::ColorMode::alphaBlended()
                                                                    : gfx::ColorMode::unblended());
        builder->setCullFaceMode(gfx::CullFaceMode::disabled());
        builder->setEnableStencil(true);

        return builder;
    };

    auto addAttributes =
        [&](gfx::DrawableBuilder& builder, const LineBucket& bucket, gfx::VertexAttributeArrayPtr&& vertexAttrs) {
            const auto vertexCount = bucket.vertices.elements();
            builder.setRawVertices({}, vertexCount, gfx::AttributeDataType::Short4);

            if (const auto& attr = vertexAttrs->set(idLinePosNormalVertexAttribute)) {
                attr->setSharedRawData(bucket.sharedVertices,
                                       offsetof(LineLayoutVertex, a1),
                                       /*vertexOffset=*/0,
                                       sizeof(LineLayoutVertex),
                                       gfx::AttributeDataType::Short2);
            }

            if (const auto& attr = vertexAttrs->set(idLineDataVertexAttribute)) {
                attr->setSharedRawData(bucket.sharedVertices,
                                       offsetof(LineLayoutVertex, a2),
                                       /*vertexOffset=*/0,
                                       sizeof(LineLayoutVertex),
                                       gfx::AttributeDataType::UByte4);
            }

            builder.setVertexAttributes(std::move(vertexAttrs));
        };

    tileLayerGroup->setStencilTiles(renderTiles);

    StringIDSetsPair propertiesAsUniforms;
    for (const RenderTile& tile : *renderTiles) {
        const auto& tileID = tile.getOverscaledTileID();
        if (liftedGroup && liftedLineStats().enabled) {
            auto& liftStats = liftedLineStats();
            if (!liftableTile(tileID)) {
                ++liftStats.flatTiles;
            } else if (activeTerrain->hasElevationCoverage(tileID)) {
                ++liftStats.liftedTiles;
            } else {
                ++liftStats.partialTiles;
            }
            liftStats.maybeEmit();
        }

        const LayerRenderData* renderData = getRenderDataForPass(tile, renderPass);
        if (!renderData) {
            removeTile(renderPass, tileID);
            continue;
        }

        auto& bucket = static_cast<LineBucket&>(*renderData->bucket);
        if (!bucket.sharedTriangles->elements()) {
            removeTile(renderPass, tileID);
            continue;
        }

        auto& paintPropertyBinders = bucket.paintPropertyBinders.at(getID());
        const auto& evaluated = getEvaluated<LineLayerProperties>(renderData->layerProperties);

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

        auto updateExisting = [&](gfx::Drawable& drawable) {
            if (drawable.getLayerTweaker() != layerTweaker) {
                // This drawable was produced on a previous style/bucket, and should not be updated.
                return false;
            }
            return true;
        };
        if (updateTile(renderPass, tileID, std::move(updateExisting))) {
            continue;
        }

        const bool liftTile = liftableTile(tileID);
        if (liftTile) {
            refreshLiftedElevations(bucket, tile, *activeTerrain, liftedEmptyPlaneElevation);
            bool liftedExists = false;
            liftedGroup->visitDrawables(renderPass, tileID, [&](gfx::Drawable& drawable) {
                drawable.setRenderTile(renderTilesOwner, &tile);
                drawable.setBinders(renderData->bucket, &paintPropertyBinders);
                liftedExists = true;
            });
            if (liftedExists) {
                continue;
            }
        }

        const auto addDrawable = [&](std::unique_ptr<gfx::Drawable>&& drawable, LineLayerTweaker::LineType type) {
            if (liftTile) {
                drawable->setTileID(tileID);
                drawable->setType(mbgl::underlying_type(type));
                drawable->setLayerTweaker(liftedLayerTweaker);
                drawable->setRenderTile(renderTilesOwner, &tile);
                drawable->setBinders(renderData->bucket, &paintPropertyBinders);
                drawable->setIs3D(true);
                drawable->setEnableStencil(false); // flat clip masks cannot clip raised geometry
                drawable->setDepthType(gfx::DepthMaskType::ReadOnly);
                const bool roundCap = bucket.layout.get<LineCap>() == LineCapType::Round;
                drawable->setData(std::make_unique<gfx::LineDrawableData>(roundCap ? LinePatternCap::Round
                                                                                   : LinePatternCap::Square));
                liftedGroup->addDrawable(renderPass, tileID, std::move(drawable));
                ++stats.drawablesAdded;
                return;
            }
            if (activeTerrain && activeTerrain->hasElevationCoverage(tileID)) {
                // Skip main-pass — drape variant carries this layer onto
                // the terrain mesh. See cleanup-on-terrain block above.
                // When this tile has no DEM coverage, fall through to the
                // main-pass emit so the layer doesn't vanish.
                return;
            }
            drawable->setTileID(tileID);
            drawable->setType(mbgl::underlying_type(type));
            drawable->setLayerTweaker(layerTweaker);
            drawable->setRenderTile(renderTilesOwner, &tile);
            drawable->setBinders(renderData->bucket, &paintPropertyBinders);

            const bool roundCap = bucket.layout.get<LineCap>() == LineCapType::Round;
            const auto capType = roundCap ? LinePatternCap::Round : LinePatternCap::Square;
            drawable->setData(std::make_unique<gfx::LineDrawableData>(capType));

            tileLayerGroup->addDrawable(renderPass, tileID, std::move(drawable));
            ++stats.drawablesAdded;
        };

        propertiesAsUniforms.first.clear();
        propertiesAsUniforms.second.clear();

        auto vertexAttrs = context.createVertexAttributeArray();
        vertexAttrs->readDataDrivenPaintProperties<LineColor,
                                                   LineBlur,
                                                   LineOpacity,
                                                   LineGapWidth,
                                                   LineOffset,
                                                   LineWidth,
                                                   LineFloorWidth,
                                                   LinePattern>(
            paintPropertyBinders, evaluated, propertiesAsUniforms, idLineColorVertexAttribute);
        if (liftTile && bucket.sharedTerrainVertices) {
            if (const auto& attr = vertexAttrs->set(idLineTerrainElevationVertexAttribute)) {
                attr->setSharedRawData(bucket.sharedTerrainVertices,
                                       offsetof(LineBucket::TerrainVertex, a1),
                                       /*vertexOffset=*/0,
                                       sizeof(LineBucket::TerrainVertex),
                                       gfx::AttributeDataType::Float);
            }
        }
        // Only the lifted shader variant declares the terrain height attribute
        // (HAS_UNIFORM_u_terrain_lift); every other line program leaves it out,
        // because Metal binds no fallback buffer for a declared attribute that
        // a drawable does not provide. Program caches key on the excluded
        // attribute ids only, so the lifted variant adds a sentinel id too.
        StringIDSetsPair liftedUniforms;
        if (liftTile) {
            liftedUniforms = propertiesAsUniforms;
            liftedUniforms.first.insert(terrain_line_lift::shaderDefineName);
            liftedUniforms.second.insert(liftedVariantSentinelID);
        }
        propertiesAsUniforms.second.insert(idLineTerrainElevationVertexAttribute);
        const StringIDSetsPair& lineUniforms = liftTile ? liftedUniforms : propertiesAsUniforms;

        // Phase 2 drape routing helper, shared across line variants. Emits a
        // copy of this source-tile's line geometry into each overlapping DEM
        // tile's drape RenderTarget. Variant-specific bits (shader group +
        // type tag + optional texture binding) are parameters; the optional
        // configureExtras callback binds variant-specific textures
        // (Gradient → color ramp, Pattern → atlas, SDF → line atlas) on the
        // drape builder before flushing.
        const auto emitDrapeLineVariant = [&](LineLayerTweaker::LineType variant,
                                                const gfx::ShaderGroupPtr& shaderGroup,
                                                const std::string& nameSuffix,
                                                const std::function<void(gfx::DrawableBuilder&)>& configureExtras = {}) {
            if (!activeTerrain || !shaderGroup || klattraDisableLineDrape() || terrainLifted) return;
            activeTerrain->visitDrapeTargets(
                [&](const OverscaledTileID& drapeID, TerrainDrapeTargetPtr& drapeTarget) {
                    if (!drapeTarget || !LayerTweaker::tilesOverlap(tileID, drapeID)) return;

                    auto& tw = drapeLayerTweakers[drapeID];
                    if (!tw) {
                        tw = std::make_shared<LineLayerTweaker>(
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

                    bool alreadyHasVariant = false;
                    drapeGroup->visitDrawables(passes, tileID, [&](const gfx::Drawable& d) {
                        if (d.getType() == static_cast<size_t>(mbgl::underlying_type(variant))) {
                            alreadyHasVariant = true;
                        }
                    });
                    if (alreadyHasVariant) return;

                    auto drapeShader = shaderGroup->getOrCreateShader(
                        context, propertiesAsUniforms, posNormalAttribName);
                    if (!drapeShader) return;

                    auto drapeBuilder = createLineBuilder(nameSuffix + "-drape", std::move(drapeShader));
                    if (!drapeBuilder) return;
                    // Stencil clipping is a main-pass mechanism; drape groups
                    // never set stencil tiles. Disable to skip the assert in
                    // PaintParameters::stencilModeForClipping.
                    drapeBuilder->setEnableStencil(false);

                    auto drapeVertexAttrs = context.createVertexAttributeArray();
                    drapeVertexAttrs->readDataDrivenPaintProperties<LineColor,
                                                                    LineBlur,
                                                                    LineOpacity,
                                                                    LineGapWidth,
                                                                    LineOffset,
                                                                    LineWidth,
                                                                    LineFloorWidth,
                                                                    LinePattern>(
                        paintPropertyBinders,
                        evaluated,
                        propertiesAsUniforms,
                        idLineColorVertexAttribute);

                    addAttributes(*drapeBuilder, bucket, std::move(drapeVertexAttrs));
                    if (configureExtras) {
                        configureExtras(*drapeBuilder);
                    }
                    setSegments(drapeBuilder, bucket);
                    drapeBuilder->flush(context);

                    const bool roundCap = bucket.layout.get<LineCap>() == LineCapType::Round;
                    const auto capType = roundCap ? LinePatternCap::Round : LinePatternCap::Square;
                    for (auto& drapeDrawable : drapeBuilder->clearDrawables()) {
                        drapeDrawable->setTileID(tileID);
                        drapeDrawable->setType(mbgl::underlying_type(variant));
                        drapeDrawable->setLayerTweaker(tw);
                        drapeDrawable->setRenderTile(renderTilesOwner, &tile);
                        drapeDrawable->setBinders(renderData->bucket, &paintPropertyBinders);
                        drapeDrawable->setData(std::make_unique<gfx::LineDrawableData>(capType));
                        drapeGroup->addDrawable(passes, tileID, std::move(drapeDrawable));
                        ++stats.drawablesAdded;
                    }
                });
        };

        if (!evaluated.get<LineDasharray>().from.empty()) {
            // dash array line (SDF)
            if (!lineSDFShaderGroup) {
                lineSDFShaderGroup = shaders.getShaderGroup("LineSDFShader");
                if (!lineSDFShaderGroup) {
                    continue;
                }
            }

            auto shader = lineSDFShaderGroup->getOrCreateShader(context, lineUniforms, posNormalAttribName);
            if (!shader) {
                continue;
            }

            emitDrapeLineVariant(LineLayerTweaker::LineType::SDF, lineSDFShaderGroup, "lineSDF");

            auto builder = createLineBuilder("lineSDF", std::move(shader));

            // vertices, attributes and segments
            addAttributes(*builder, bucket, std::move(vertexAttrs));
            setSegments(builder, bucket);

            builder->flush(context);
            for (auto& drawable : builder->clearDrawables()) {
                addDrawable(std::move(drawable), LineLayerTweaker::LineType::SDF);
            }
        } else if (!unevaluated.get<LinePattern>().isUndefined()) {
            // pattern line
            if (!linePatternShaderGroup) {
                linePatternShaderGroup = shaders.getShaderGroup("LinePatternShader");
                if (!linePatternShaderGroup) {
                    continue;
                }
            }

            auto shader = linePatternShaderGroup->getOrCreateShader(context, propertiesAsUniforms, posNormalAttribName);
            if (!shader) {
                continue;
            }

            auto builder = createLineBuilder("linePattern", std::move(shader));

            // vertices and attributes
            addAttributes(*builder, bucket, std::move(vertexAttrs));

            // texture
            if (const auto& atlases = tile.getAtlasTextures(); atlases && atlases->icon) {
                auto iconTweaker = std::make_shared<gfx::DrawableAtlasesTweaker>(
                    atlases,
                    std::nullopt,
                    idLineImageTexture,
                    /*isText*/ false,
                    /*sdfIcons*/ true, // to force linear filter
                    /*rotationAlignment_*/ AlignmentType::Auto,
                    /*iconScaled*/ false,
                    /*textSizeIsZoomConstant_*/ false);

                builder->addTweaker(iconTweaker);

                // Drape routing: same atlas tweaker on the drape builder
                // so the terrain mesh sees the pattern too.
                emitDrapeLineVariant(
                    LineLayerTweaker::LineType::Pattern,
                    linePatternShaderGroup,
                    "linePattern",
                    [&](gfx::DrawableBuilder& db) {
                        db.addTweaker(iconTweaker);
                    });

                setSegments(builder, bucket);

                builder->flush(context);
                for (auto& drawable : builder->clearDrawables()) {
                    addDrawable(std::move(drawable), LineLayerTweaker::LineType::Pattern);
                }
            }
        } else if (!unevaluated.get<LineGradient>().getValue().isUndefined()) {
            // gradient line
            if (!lineGradientShaderGroup) {
                lineGradientShaderGroup = shaders.getShaderGroup("LineGradientShader");
                if (!lineGradientShaderGroup) {
                    continue;
                }
            }

            auto shader = lineGradientShaderGroup->getOrCreateShader(
                context, propertiesAsUniforms, posNormalAttribName);
            if (!shader) {
                continue;
            }

            auto builder = createLineBuilder("lineGradient", std::move(shader));

            // vertices and attributes
            addAttributes(*builder, bucket, std::move(vertexAttrs));

            // texture
            if (!colorRampTexture2D && colorRamp->valid()) {
                // create texture. to be reused for all the tiles of the layer
                colorRampTexture2D = context.createTexture2D();
                colorRampTexture2D->setImage(colorRamp);
                colorRampTexture2D->setSamplerConfiguration({.filter = gfx::TextureFilterType::Linear,
                                                             .wrapU = gfx::TextureWrapType::Clamp,
                                                             .wrapV = gfx::TextureWrapType::Clamp});
            }

            if (colorRampTexture2D) {
                builder->setTexture(colorRampTexture2D, idLineImageTexture);

                // Drape routing: bind the same color-ramp texture on the
                // drape builder so the terrain mesh sees the gradient too.
                emitDrapeLineVariant(
                    LineLayerTweaker::LineType::Gradient,
                    lineGradientShaderGroup,
                    "lineGradient",
                    [&](gfx::DrawableBuilder& db) {
                        db.setTexture(colorRampTexture2D, idLineImageTexture);
                    });

                setSegments(builder, bucket);

                builder->flush(context);
                for (auto& drawable : builder->clearDrawables()) {
                    addDrawable(std::move(drawable), LineLayerTweaker::LineType::Gradient);
                }
            }
        } else {
            // simple line
            if (!lineShaderGroup) {
                lineShaderGroup = shaders.getShaderGroup("LineShader");
                if (!lineShaderGroup) {
                    continue;
                }
            }

            auto shader = lineShaderGroup->getOrCreateShader(context, lineUniforms, posNormalAttribName);
            if (!shader) {
                continue;
            }

            emitDrapeLineVariant(LineLayerTweaker::LineType::Simple, lineShaderGroup, "line");

            auto builder = createLineBuilder("line", std::move(shader));

            // vertices, attributes and segments
            addAttributes(*builder, bucket, std::move(vertexAttrs));
            setSegments(builder, bucket);

            builder->flush(context);
            for (auto& drawable : builder->clearDrawables()) {
                addDrawable(std::move(drawable), LineLayerTweaker::LineType::Simple);
            }
        }
    }
}

} // namespace mbgl
