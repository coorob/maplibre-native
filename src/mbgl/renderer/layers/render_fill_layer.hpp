#pragma once

#include <mbgl/renderer/render_layer.hpp>
#include <mbgl/style/layers/fill_layer_impl.hpp>
#include <mbgl/style/layers/fill_layer_properties.hpp>
#include <mbgl/layout/pattern_layout.hpp>
#include <mbgl/renderer/buckets/fill_bucket.hpp>
#include <mbgl/tile/tile_id.hpp>

#include <memory>
#include <unordered_map>

namespace mbgl {

class FillLayerTweaker;
using FillLayerTweakerPtr = std::shared_ptr<FillLayerTweaker>;

class RenderFillLayer final : public RenderLayer {
public:
    enum class FillVariant : uint8_t {
        Fill,
        FillPattern,
        FillOutline,
        FillOutlinePattern,
        FillOutlineTriangulated,
        Undefined = 255
    };

public:
    explicit RenderFillLayer(Immutable<style::FillLayer::Impl>);
    ~RenderFillLayer() override;

    /// Generate any changes needed by the layer
    void update(gfx::ShaderRegistry&,
                gfx::Context&,
                const TransformState&,
                const std::shared_ptr<UpdateParameters>&,
                const RenderTree&,
                UniqueChangeRequestVec&) override;

private:
    void transition(const TransitionParameters&) override;
    void evaluate(const PropertyEvaluationParameters&) override;
    bool hasTransition() const override;
    bool hasCrossfade() const override;

    bool queryIntersectsFeature(const GeometryCoordinates&,
                                const GeometryTileFeature&,
                                float,
                                const TransformState&,
                                float,
                                const mat4&,
                                const FeatureState&) const override;

    // Paint properties
    style::FillPaintProperties::Unevaluated unevaluated;

    gfx::ShaderGroupPtr fillShaderGroup;
    gfx::ShaderGroupPtr outlineShaderGroup;
    gfx::ShaderGroupPtr patternShaderGroup;
    gfx::ShaderGroupPtr outlinePatternShaderGroup;

#if MLN_TRIANGULATE_FILL_OUTLINES
    gfx::ShaderGroupPtr outlineTriangulatedShaderGroup;
#endif // MLN_TRIANGULATE_FILL_OUTLINES

    // Phase 2 drape routing: per-drape-target FillLayerTweakers, keyed by
    // the drape target's tile ID. Each tweaker carries that target's tileID
    // so getDrapeMatrix() can project source-tile drawables into the right
    // drape RenderTarget. Lazily populated on first update() under terrain.
    std::unordered_map<OverscaledTileID, FillLayerTweakerPtr> drapeLayerTweakers;
};

} // namespace mbgl
