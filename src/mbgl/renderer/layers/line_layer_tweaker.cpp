#include <mbgl/renderer/layers/line_layer_tweaker.hpp>

#include <algorithm>

#include <mbgl/gfx/context.hpp>
#include <mbgl/gfx/drawable.hpp>
#include <mbgl/gfx/line_drawable_data.hpp>
#include <mbgl/geometry/line_atlas.hpp>
#include <mbgl/renderer/buckets/line_bucket.hpp>
#include <mbgl/renderer/layer_group.hpp>
#include <mbgl/renderer/paint_parameters.hpp>
#include <mbgl/renderer/render_tile.hpp>
#include <mbgl/renderer/render_terrain.hpp>
#include <mbgl/renderer/terrain_line_lift.hpp>
#include <mbgl/shaders/line_layer_ubo.hpp>
#include <mbgl/shaders/shader_program_base.hpp>
#include <mbgl/style/layers/line_layer_properties.hpp>
#include <mbgl/util/logging.hpp>
#include <mbgl/util/constants.hpp>
#include <mbgl/util/math.hpp>

#if MLN_RENDER_BACKEND_METAL
#include <mbgl/shaders/mtl/line.hpp>
#endif // MLN_RENDER_BACKEND_METAL

namespace mbgl {

using namespace style;
using namespace shaders;

#if MLN_RENDER_BACKEND_METAL && !defined(NDEBUG)
constexpr bool diff(float actual, float expected, float e = 1.0e-6) {
    return actual != expected && (expected == 0 || std::fabs((actual - expected) / expected) > e);
}
constexpr bool diff(Color actual, Color expected, float e = 1.0e-6) {
    return diff(actual.r, expected.r, e) || diff(actual.g, expected.g, e) || diff(actual.b, expected.b, e) ||
           diff(actual.a, expected.a, e);
}
#endif // MLN_RENDER_BACKEND_METAL

#if MLN_RENDER_BACKEND_METAL && !defined(NDEBUG)
template <typename Result>
std::optional<Result> LineLayerTweaker::gpuEvaluate(
    [[maybe_unused]] const LinePaintProperties::PossiblyEvaluated& evaluated,
    const PaintParameters& parameters,
    const std::size_t index) const {
    if (const auto& gpu = gpuExpressions[index]) {
        const float effectiveZoom = (gpu->options & gfx::GPUOptions::IntegerZoom)
                                        ? parameters.state.getIntegerZoom()
                                        : static_cast<float>(parameters.state.getZoom());
        return gpu->template evaluate<Result>(effectiveZoom);
    }
    return {};
}
#endif // MLN_RENDER_BACKEND_METAL

template <typename Property>
auto LineLayerTweaker::evaluate([[maybe_unused]] const PaintParameters& parameters) const {
    const auto& evaluated = static_cast<const LineLayerProperties&>(*evaluatedProperties).evaluated;

#if MLN_RENDER_BACKEND_METAL && !defined(NDEBUG)
    constexpr auto index = propertyIndex<Property>();
    if (auto gpuValue = gpuEvaluate<typename Property::Type>(evaluated, parameters, index)) {
        assert(!diff(*gpuValue, evaluated.get<Property>().constantOr(Property::defaultValue())));
        return *gpuValue;
    }
#endif // MLN_RENDER_BACKEND_METAL

    return evaluated.get<Property>().constantOr(Property::defaultValue());
}

void LineLayerTweaker::execute(LayerGroupBase& layerGroup, const PaintParameters& parameters) {
    if (layerGroup.empty()) {
        return;
    }

    auto& context = parameters.context;
    const auto& evaluated = static_cast<const LineLayerProperties&>(*evaluatedProperties).evaluated;
    const auto& crossfade = static_cast<const LineLayerProperties&>(*evaluatedProperties).crossfade;

    const auto& linePatternValue = evaluated.get<LinePattern>().constantOr(
        Faded<expression::Image>{.from = "", .to = ""});

    const auto zoom = static_cast<float>(parameters.state.getZoom());
    const auto intZoom = parameters.state.getIntegerZoom();

    // getDrapeMatrix is orthographic in the target's canonical geography.
    // Keep line widths in camera pixels; only the antialias footprint uses
    // target texels. Main-viewport units_to_pixels/DPR do not describe this FBO.
    float drapeTexelDensity = 0.0f;
#if defined(__ANDROID__) && MLN_RENDER_BACKEND_OPENGL
    if (drapeTargetID && parameters.activeTerrain) {
        if (const auto target = parameters.activeTerrain->getDrapeTarget(*drapeTargetID)) {
            const auto size = target->getSize();
            if (size.width > 0 && size.width == size.height) {
                drapeTexelDensity = static_cast<float>(
                    size.width / (util::tileSize_D * std::exp2(parameters.state.getZoom() - drapeTargetID->canonical.z)));
            }
        }
    }
#endif

#if MLN_RENDER_BACKEND_METAL
    const auto getExpressionBuffer = [&]() {
        const bool enableEval = gfx::Backend::getEnableGPUExpressionEval();
        if (!expressionUniformBuffer || (gpuExpressionsUpdated && enableEval)) {
            LineExpressionUBO exprUBO = {
                .color = enableEval ? gpuExpressions[propertyIndex<LineColor>()].get() : nullptr,
                .blur = enableEval ? gpuExpressions[propertyIndex<LineBlur>()].get() : nullptr,
                .opacity = enableEval ? gpuExpressions[propertyIndex<LineOpacity>()].get() : nullptr,
                .gapwidth = enableEval ? gpuExpressions[propertyIndex<LineGapWidth>()].get() : nullptr,
                .offset = enableEval ? gpuExpressions[propertyIndex<LineOffset>()].get() : nullptr,
                .width = enableEval ? gpuExpressions[propertyIndex<LineWidth>()].get() : nullptr,
                .floorWidth = enableEval ? gpuExpressions[propertyIndex<LineFloorWidth>()].get() : nullptr,
            };
            context.emplaceOrUpdateUniformBuffer(expressionUniformBuffer, &exprUBO);
            gpuExpressionsUpdated = false;
        }
        return expressionUniformBuffer;
    };
#endif // MLN_RENDER_BACKEND_METAL

    if (!evaluatedPropsUniformBuffer || propertiesUpdated || evaluatedDrapeTexelDensity != drapeTexelDensity) {
#if MLN_RENDER_BACKEND_METAL
        expressionMask =
            !gfx::Backend::getEnableGPUExpressionEval()
                ? LineExpressionMask::None
                : ((gpuExpressions[propertyIndex<LineColor>()] ? LineExpressionMask::Color : LineExpressionMask::None) |
                   (gpuExpressions[propertyIndex<LineBlur>()] ? LineExpressionMask::Blur : LineExpressionMask::None) |
                   (gpuExpressions[propertyIndex<LineOpacity>()] ? LineExpressionMask::Opacity
                                                                 : LineExpressionMask::None) |
                   (gpuExpressions[propertyIndex<LineGapWidth>()] ? LineExpressionMask::GapWidth
                                                                  : LineExpressionMask::None) |
                   (gpuExpressions[propertyIndex<LineOffset>()] ? LineExpressionMask::Offset
                                                                : LineExpressionMask::None) |
                   (gpuExpressions[propertyIndex<LineWidth>()] ? LineExpressionMask::Width : LineExpressionMask::None) |
                   (gpuExpressions[propertyIndex<LineFloorWidth>()] ? LineExpressionMask::FloorWidth
                                                                    : LineExpressionMask::None));
        const LineEvaluatedPropsUBO propsUBO{
            .color = (expressionMask & LineExpressionMask::Color) ? LineColor::defaultValue()
                                                                  : evaluate<LineColor>(parameters),

            .blur = (expressionMask & LineExpressionMask::Blur) ? LineBlur::defaultValue()
                                                                : evaluate<LineBlur>(parameters),

            .opacity = (expressionMask & LineExpressionMask::Opacity) ? LineOpacity::defaultValue()
                                                                      : evaluate<LineOpacity>(parameters),

            .gapwidth = (expressionMask & LineExpressionMask::GapWidth) ? LineGapWidth::defaultValue()
                                                                        : evaluate<LineGapWidth>(parameters),

            .offset = (expressionMask & LineExpressionMask::Offset) ? LineOffset::defaultValue()
                                                                    : evaluate<LineOffset>(parameters),

            .width = (expressionMask & LineExpressionMask::Width) ? LineWidth::defaultValue()
                                                                  : evaluate<LineWidth>(parameters),

            .floorwidth = (expressionMask & LineExpressionMask::FloorWidth) ? LineFloorWidth::defaultValue()
                                                                            : evaluate<LineFloorWidth>(parameters),
            .expressionMask = expressionMask,
            .pad1 = 0};
#elif MLN_RENDER_BACKEND_WEBGPU
        expressionMask = LineExpressionMask::None;
        if (evaluated.get<LineColor>().isConstant()) {
            expressionMask |= LineExpressionMask::Color;
        }
        if (evaluated.get<LineBlur>().isConstant()) {
            expressionMask |= LineExpressionMask::Blur;
        }
        if (evaluated.get<LineOpacity>().isConstant()) {
            expressionMask |= LineExpressionMask::Opacity;
        }
        if (evaluated.get<LineGapWidth>().isConstant()) {
            expressionMask |= LineExpressionMask::GapWidth;
        }
        if (evaluated.get<LineOffset>().isConstant()) {
            expressionMask |= LineExpressionMask::Offset;
        }
        if (evaluated.get<LineWidth>().isConstant()) {
            expressionMask |= LineExpressionMask::Width;
        }
        if (evaluated.get<LineFloorWidth>().isConstant()) {
            expressionMask |= LineExpressionMask::FloorWidth;
        }

        const LineEvaluatedPropsUBO propsUBO{
            .color = evaluated.get<LineColor>().constantOr(LineColor::defaultValue()),
            .blur = evaluated.get<LineBlur>().constantOr(LineBlur::defaultValue()),
            .opacity = evaluated.get<LineOpacity>().constantOr(LineOpacity::defaultValue()),
            .gapwidth = evaluated.get<LineGapWidth>().constantOr(LineGapWidth::defaultValue()),
            .offset = evaluated.get<LineOffset>().constantOr(LineOffset::defaultValue()),
            .width = evaluated.get<LineWidth>().constantOr(LineWidth::defaultValue()),
            .floorwidth = evaluated.get<LineFloorWidth>().constantOr(LineFloorWidth::defaultValue()),
            .expressionMask = expressionMask,
            .pad1 = 0};
#else
        const LineEvaluatedPropsUBO propsUBO{/*color =*/evaluate<LineColor>(parameters),
                                             /*blur =*/evaluate<LineBlur>(parameters),
                                             /*opacity =*/evaluate<LineOpacity>(parameters),
                                             /*gapwidth =*/evaluate<LineGapWidth>(parameters),
                                             /*offset =*/evaluate<LineOffset>(parameters),
                                             /*width =*/evaluate<LineWidth>(parameters),
                                             /*floorwidth =*/evaluate<LineFloorWidth>(parameters),
                                             LineExpressionMask::None,
                                             drapeTexelDensity};
#endif // MLN_RENDER_BACKEND_METAL

        context.emplaceOrUpdateUniformBuffer(evaluatedPropsUniformBuffer, &propsUBO);
        evaluatedDrapeTexelDensity = drapeTexelDensity;
        propertiesUpdated = false;
    }
    auto& layerUniforms = layerGroup.mutableUniformBuffers();
    layerUniforms.set(idLineEvaluatedPropsUBO, evaluatedPropsUniformBuffer);

#if MLN_RENDER_BACKEND_METAL
    // GPU Expressions
    layerUniforms.set(idLineExpressionUBO, getExpressionBuffer());
#endif // MLN_RENDER_BACKEND_METAL

    // Terrain-lifted drawables carry raw DEM metres in z. Fold the terrain
    // mesh's render-height transform into their matrix (same exaggeration and
    // origin as the terrain tweaker), use the terrain's near-clipped projection
    // so both share one depth comparison, and pull them a little toward the
    // camera: a constant clip-space z offset c moves a point at view depth w
    // by c * w / (2 * near), i.e. a fixed fraction of its distance.
    std::optional<std::array<double, 3>> liftTransform; // {scale, offset, clip pull}
    if (terrainLifted && parameters.activeTerrain) {
        const auto& terrain = *parameters.activeTerrain;
        const float exaggeration = terrain.getExaggeration();
        const float fallbackOrigin = std::abs(parameters.state.getLatLng().latitude()) >= 60.0 ? 1000.0f : 0.0f;
        const float origin = terrain.getElevationOriginMeters().value_or(fallbackOrigin);
        static const bool absoluteHeights = std::getenv("KLATTRA_TERRAIN_ABSOLUTE_HEIGHTS") != nullptr;
        const double nearZ = 0.1 * parameters.state.getCameraToCenterDistance();
        liftTransform = std::array<double, 3>{
            exaggeration,
            (absoluteHeights ? 0.0 : -static_cast<double>(origin) * exaggeration) + terrain_line_lift::liftMeters(),
            2.0 * nearZ * terrain_line_lift::depthPullFraction()};
    }

    // Lifted drawables cannot be stencil-clipped (the clip masks are flat).
    // Each one clips to its own tile in the fragment shader; this records which
    // 4x4 cells of a tile are covered by a deeper lifted tile drawn alongside
    // it (a parent kept while its children load), so the parent skips them.
    std::vector<OverscaledTileID> liftedTileIDs;
    if (liftTransform) {
        visitLayerGroupDrawables(layerGroup, [&](gfx::Drawable& drawable) {
            if (drawable.getEnabled() && drawable.getIs3D() && drawable.getTileID()) {
                liftedTileIDs.push_back(*drawable.getTileID());
            }
        });
        std::sort(liftedTileIDs.begin(), liftedTileIDs.end());
        liftedTileIDs.erase(std::unique(liftedTileIDs.begin(), liftedTileIDs.end()), liftedTileIDs.end());
    }
    const auto coveredCellMask = [&](const OverscaledTileID& tile) {
        uint32_t mask = 0;
        for (const auto& other : liftedTileIDs) {
            const int dz = static_cast<int>(other.canonical.z) - static_cast<int>(tile.canonical.z);
            if (dz <= 0 || dz > 2 || other.wrap != tile.wrap || !other.canonical.isChildOf(tile.canonical)) {
                continue;
            }
            const uint32_t span = 1u << (2 - dz); // cells per side covered by the descendant
            const uint32_t x0 = (other.canonical.x - (tile.canonical.x << dz)) * span;
            const uint32_t y0 = (other.canonical.y - (tile.canonical.y << dz)) * span;
            for (uint32_t y = y0; y < y0 + span && y < 4; ++y) {
                for (uint32_t x = x0; x < x0 + span && x < 4; ++x) {
                    mask |= 1u << (y * 4 + x);
                }
            }
        }
        return mask;
    };

#if MLN_UBO_CONSOLIDATION
    int i = 0;
    std::vector<LineDrawableUnionUBO> drawableUBOVector(layerGroup.getDrawableCount());
    std::vector<LineTilePropsUnionUBO> tilePropsUBOVector(layerGroup.getDrawableCount());
#endif

    visitLayerGroupDrawables(layerGroup, [&](gfx::Drawable& drawable) {
        const auto shader = drawable.getShader();
        if (!drawable.getTileID() || !shader || !checkTweakDrawable(drawable)) {
            return;
        }

        const UnwrappedTileID tileID = drawable.getTileID()->toUnwrapped();

        auto* binders = static_cast<LineBinders*>(drawable.getBinders());
        const auto* tile = drawable.getRenderTile();
        if (!binders || !tile) {
            assert(false);
            return;
        }

        const auto patternPosA = tile->getPattern(linePatternValue.from.id());
        const auto patternPosB = tile->getPattern(linePatternValue.to.id());
        binders->setPatternParameters(patternPosA, patternPosB, crossfade);

        const auto& translation = evaluated.get<LineTranslate>();
        const auto anchor = evaluated.get<LineTranslateAnchor>();
        constexpr bool nearClipped = false;
        constexpr bool inViewportPixelUnits = false; // from RenderTile::translatedMatrix
        // Drape mode: route the drawable into the per-DEM-tile drape texture
        // instead of the camera. The camera-zoom ratio preserves geographic
        // stroke widths even when the source and target canonical zooms differ.
        // drapeTexelDensity changes coverage filtering, never that ratio.
        const bool lifted = liftTransform && drawable.getIs3D();
        const auto matrix = [&]() -> mat4 {
            if (drapeTargetID) return getDrapeMatrix(*drawable.getTileID(), *drapeTargetID);
            if (lifted) {
                const auto& [zScale, zOffset, clipPull] = *liftTransform;
                mat4 m;
                parameters.state.matrixFor(m, tileID);
                matrix::translate(m, m, 0.0, 0.0, zOffset);
                matrix::scale(m, m, 1.0, 1.0, zScale);
                matrix::multiply(m, parameters.transformParams.nearClippedProjMatrix, m);
                m[14] -= clipPull; // column 3, row 2: clip-space z translation
                return m;
            }
            return getTileMatrix(
                tileID, parameters, translation, anchor, nearClipped, inViewportPixelUnits, drawable);
        }();
        // GL line shaders read the Simple/SDF drawable UBO's pad1 as
        // u_terrain_lift: 0 = flat/draped, 1 + covered-cell mask = lifted.
        const float terrainLift = lifted ? 1.0f + static_cast<float>(coveredCellMask(*drawable.getTileID())) : 0.0f;

#if !MLN_UBO_CONSOLIDATION
        auto& drawableUniforms = drawable.mutableUniformBuffers();
#endif
        switch (static_cast<LineType>(drawable.getType())) {
            case LineType::Simple: {
#if MLN_UBO_CONSOLIDATION
                drawableUBOVector[i].lineDrawableUBO = {
#else
                const LineDrawableUBO drawableUBO = {
#endif
                    .matrix = util::cast<float>(matrix),
                    .ratio = 1.0f / tileID.pixelsToTileUnits(1.0f, static_cast<float>(zoom)),

                    .color_t = std::get<0>(binders->get<LineColor>()->interpolationFactor(zoom)),
                    .blur_t = std::get<0>(binders->get<LineBlur>()->interpolationFactor(zoom)),
                    .opacity_t = std::get<0>(binders->get<LineOpacity>()->interpolationFactor(zoom)),
                    .gapwidth_t = std::get<0>(binders->get<LineGapWidth>()->interpolationFactor(zoom)),
                    .offset_t = std::get<0>(binders->get<LineOffset>()->interpolationFactor(zoom)),
                    .width_t = std::get<0>(binders->get<LineWidth>()->interpolationFactor(zoom)),
                    .pad1 = terrainLift
                };

#if !MLN_UBO_CONSOLIDATION
                drawableUniforms.createOrUpdate(idLineDrawableUBO, &drawableUBO, context);
#endif

            } break;

            case LineType::Gradient: {
#if MLN_UBO_CONSOLIDATION
                drawableUBOVector[i].lineGradientDrawableUBO = {
#else
                const LineGradientDrawableUBO drawableUBO = {
#endif
                    .matrix = util::cast<float>(matrix),
                    .ratio = 1.0f / tileID.pixelsToTileUnits(1.0f, static_cast<float>(zoom)),

                    .blur_t = std::get<0>(binders->get<LineBlur>()->interpolationFactor(zoom)),
                    .opacity_t = std::get<0>(binders->get<LineOpacity>()->interpolationFactor(zoom)),
                    .gapwidth_t = std::get<0>(binders->get<LineGapWidth>()->interpolationFactor(zoom)),
                    .offset_t = std::get<0>(binders->get<LineOffset>()->interpolationFactor(zoom)),
                    .width_t = std::get<0>(binders->get<LineWidth>()->interpolationFactor(zoom)),
                    .pad1 = 0,
                    .pad2 = 0
                };

#if !MLN_UBO_CONSOLIDATION
                drawableUniforms.createOrUpdate(idLineDrawableUBO, &drawableUBO, context);
#endif
            } break;

            case LineType::Pattern: {
                Size textureSize{0, 0};
                if (const auto& texture = drawable.getTexture(idLineImageTexture)) {
                    textureSize = texture->getSize();
                }
#if MLN_UBO_CONSOLIDATION
                drawableUBOVector[i].linePatternDrawableUBO = {
#else
                const LinePatternDrawableUBO drawableUBO = {
#endif
                    .matrix = util::cast<float>(matrix),
                    .ratio = 1.0f / tileID.pixelsToTileUnits(1.0f, static_cast<float>(zoom)),

                    .blur_t = std::get<0>(binders->get<LineBlur>()->interpolationFactor(zoom)),
                    .opacity_t = std::get<0>(binders->get<LineOpacity>()->interpolationFactor(zoom)),
                    .gapwidth_t = std::get<0>(binders->get<LineGapWidth>()->interpolationFactor(zoom)),
                    .offset_t = std::get<0>(binders->get<LineOffset>()->interpolationFactor(zoom)),
                    .width_t = std::get<0>(binders->get<LineWidth>()->interpolationFactor(zoom)),
                    .pattern_from_t = std::get<0>(binders->get<LinePattern>()->interpolationFactor(zoom)),
                    .pattern_to_t = std::get<1>(binders->get<LinePattern>()->interpolationFactor(zoom))
                };

#if MLN_UBO_CONSOLIDATION
                tilePropsUBOVector[i].linePatternTilePropsUBO = LinePatternTilePropsUBO {
#else
                const LinePatternTilePropsUBO tilePropsUBO = {
#endif
                    .pattern_from = patternPosA ? util::cast<float>(patternPosA->tlbr()) : std::array<float, 4>{0},

                    .pattern_to = patternPosB ? util::cast<float>(patternPosB->tlbr()) : std::array<float, 4>{0},

                    .scale = {parameters.pixelRatio,
                              1 / tileID.pixelsToTileUnits(1, intZoom),
                              crossfade.fromScale,
                              crossfade.toScale},

                    .texsize = {static_cast<float>(textureSize.width), static_cast<float>(textureSize.height)},
                    .fade = crossfade.t, .pad1 = 0
                };

#if !MLN_UBO_CONSOLIDATION
                drawableUniforms.createOrUpdate(idLineDrawableUBO, &drawableUBO, context);
                drawableUniforms.createOrUpdate(idLineTilePropsUBO, &tilePropsUBO, context);
#endif
            } break;

            case LineType::SDF: {
                if (const auto& data = drawable.getData()) {
                    const gfx::LineDrawableData& lineData = static_cast<const gfx::LineDrawableData&>(*data);
                    const auto& dashPatternTexture = parameters.lineAtlas.getDashPatternTexture(
                        evaluated.get<LineDasharray>().from,
                        evaluated.get<LineDasharray>().to,
                        lineData.linePatternCap);

                    // texture
                    if (!drawable.getTexture(idLineImageTexture)) {
                        const auto& texture = dashPatternTexture.getTexture();
                        drawable.setEnabled(!!texture);
                        if (texture) {
                            drawable.setTexture(texture, idLineImageTexture);
                        }
                    }

                    const LinePatternPos& posA = dashPatternTexture.getFrom();
                    const LinePatternPos& posB = dashPatternTexture.getTo();
                    const float widthA = posA.width * crossfade.fromScale;
                    const float widthB = posB.width * crossfade.toScale;

#if MLN_UBO_CONSOLIDATION
                    drawableUBOVector[i].lineSDFDrawableUBO = {
#else
                    const LineSDFDrawableUBO drawableUBO = {
#endif
                        .matrix = util::cast<float>(matrix),
                        .patternscale_a = {1.0f / tileID.pixelsToTileUnits(widthA, intZoom), -posA.height / 2.0f},
                        .patternscale_b = {1.0f / tileID.pixelsToTileUnits(widthB, intZoom), -posB.height / 2.0f},
                        .tex_y_a = posA.y,
                        .tex_y_b = posB.y,
                        .ratio = 1.0f / tileID.pixelsToTileUnits(1.0f, zoom),

                        .color_t = std::get<0>(binders->get<LineColor>()->interpolationFactor(zoom)),
                        .blur_t = std::get<0>(binders->get<LineBlur>()->interpolationFactor(zoom)),
                        .opacity_t = std::get<0>(binders->get<LineOpacity>()->interpolationFactor(zoom)),
                        .gapwidth_t = std::get<0>(binders->get<LineGapWidth>()->interpolationFactor(zoom)),
                        .offset_t = std::get<0>(binders->get<LineOffset>()->interpolationFactor(zoom)),
                        .width_t = std::get<0>(binders->get<LineWidth>()->interpolationFactor(zoom)),
                        .floorwidth_t = std::get<0>(binders->get<LineFloorWidth>()->interpolationFactor(zoom)),
                        .pad1 = terrainLift,
                        .pad2 = 0
                    };

#if MLN_UBO_CONSOLIDATION
                    tilePropsUBOVector[i].lineSDFTilePropsUBO = LineSDFTilePropsUBO {
#else
                    const LineSDFTilePropsUBO tilePropsUBO = {
#endif
                        .sdfgamma = static_cast<float>(dashPatternTexture.getSize().width) /
                                    (std::min(widthA, widthB) * 256.0f *
                                     (drapeTexelDensity > 0.0f
                                          ? drapeTexelDensity * std::exp2(zoom - intZoom)
                                          : parameters.pixelRatio)) / 2.0f,
                        .mix = crossfade.t, .pad1 = 0, .pad2 = 0
                    };

#if !MLN_UBO_CONSOLIDATION
                    drawableUniforms.createOrUpdate(idLineDrawableUBO, &drawableUBO, context);
                    drawableUniforms.createOrUpdate(idLineTilePropsUBO, &tilePropsUBO, context);
#endif
                }
            } break;

            default: {
                Log::Error(Event::General,
                           "LineLayerTweaker: unknown line type: " + std::to_string(drawable.getType()));
            } break;
        }

#if MLN_UBO_CONSOLIDATION
        drawable.setUBOIndex(i++);
#endif
    });

#if MLN_UBO_CONSOLIDATION
    const size_t drawableUBOVectorSize = sizeof(LineDrawableUnionUBO) * drawableUBOVector.size();
    if (!drawableUniformBuffer || drawableUniformBuffer->getSize() < drawableUBOVectorSize) {
        drawableUniformBuffer = context.createUniformBuffer(
            drawableUBOVector.data(), drawableUBOVectorSize, false, true);
    } else {
        drawableUniformBuffer->update(drawableUBOVector.data(), drawableUBOVectorSize);
    }

    const size_t tilePropsUBOVectorSize = sizeof(LineTilePropsUnionUBO) * tilePropsUBOVector.size();
    if (!tilePropsUniformBuffer || tilePropsUniformBuffer->getSize() < tilePropsUBOVectorSize) {
        tilePropsUniformBuffer = context.createUniformBuffer(
            tilePropsUBOVector.data(), tilePropsUBOVectorSize, false, true);
    } else {
        tilePropsUniformBuffer->update(tilePropsUBOVector.data(), tilePropsUBOVectorSize);
    }

    layerUniforms.set(idLineDrawableUBO, drawableUniformBuffer);
    layerUniforms.set(idLineTilePropsUBO, tilePropsUniformBuffer);
#endif
}

#if MLN_RENDER_BACKEND_METAL
void LineLayerTweaker::updateGPUExpressions(const Unevaluated& unevaluated, TimePoint now) {
    if (gfx::Backend::getEnableGPUExpressionEval()) {
        if (unevaluated.updateGPUExpressions(gpuExpressions, now)) {
            gpuExpressionsUpdated = true;

            // Masks also need to be updated
            propertiesUpdated = true;
        }
    }
}

#endif // MLN_RENDER_BACKEND_METAL

} // namespace mbgl
