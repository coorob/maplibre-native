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
#include <mbgl/renderer/update_parameters.hpp>
#include <mbgl/shaders/line_layer_ubo.hpp>
#include <mbgl/shaders/shader_program_base.hpp>

#include <unordered_set>

namespace mbgl {

using namespace style;
using namespace shaders;

namespace {

inline const LineLayer::Impl& impl_cast(const Immutable<style::Layer::Impl>& impl) {
    assert(impl->getTypeInfo() == LineLayer::Impl::staticTypeInfo());
    return static_cast<const LineLayer::Impl&>(*impl);
}

const auto posNormalAttribName = "a_pos_normal";

} // namespace

RenderLineLayer::RenderLineLayer(Immutable<style::LineLayer::Impl> _impl)
    : RenderLayer(makeMutable<LineLayerProperties>(std::move(_impl))),
      unevaluated(impl_cast(baseImpl).paint.untransitioned()),
      colorRamp(std::make_shared<PremultipliedImage>(Size(256, 1))) {
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
                             const TransformState&,
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

    stats.drawablesRemoved += tileLayerGroup->removeDrawablesIf([&](gfx::Drawable& drawable) {
        // If the render pass has changed or the tile has  dropped out of the cover set, remove it.
        const auto& tileID = drawable.getTileID();
        if (drawable.getRenderPass() != passes || (tileID && !hasRenderTile(*tileID))) {
            return true;
        }
        return false;
    });

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
                    stats.drawablesRemoved += drapeGroup->removeDrawablesIf(
                        [&](gfx::Drawable& drawable) {
                            const auto& dID = drawable.getTileID();
                            return dID && !hasRenderTile(*dID);
                        });
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

        const auto addDrawable = [&](std::unique_ptr<gfx::Drawable>&& drawable, LineLayerTweaker::LineType type) {
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
            if (!activeTerrain || !shaderGroup) return;
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

            auto shader = lineSDFShaderGroup->getOrCreateShader(context, propertiesAsUniforms, posNormalAttribName);
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

                builder->addTweaker(std::move(iconTweaker));

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

            auto shader = lineShaderGroup->getOrCreateShader(context, propertiesAsUniforms, posNormalAttribName);
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
