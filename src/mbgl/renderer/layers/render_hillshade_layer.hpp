#pragma once

#include <mbgl/renderer/render_layer.hpp>
#include <mbgl/renderer/buckets/hillshade_bucket.hpp>
#include <mbgl/style/layers/hillshade_layer_impl.hpp>
#include <mbgl/style/layers/hillshade_layer_properties.hpp>
#include <mbgl/tile/tile_id.hpp>
#include <mbgl/gfx/texture2d.hpp>

#include <unordered_map>

namespace mbgl {

class HillshadeLayerTweaker;
using HillshadeLayerTweakerPtr = std::shared_ptr<HillshadeLayerTweaker>;

class RenderHillshadeLayer : public RenderLayer {
public:
    explicit RenderHillshadeLayer(Immutable<style::HillshadeLayer::Impl>);
    ~RenderHillshadeLayer() override;

    void markLayerRenderable(bool willRender, UniqueChangeRequestVec& changes) override;

    void layerRemoved(UniqueChangeRequestVec&) override;

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

    void updateLayerTweaker();

    void layerChanged(const TransitionParameters& parameters,
                      const Immutable<style::Layer::Impl>& impl,
                      UniqueChangeRequestVec& changes) override;

    void prepare(const LayerPrepareParameters&) override;

    void addRenderTarget(const RenderTargetPtr&, UniqueChangeRequestVec&);
    void removeRenderTargets(UniqueChangeRequestVec&);

    // Paint properties
    style::HillshadePaintProperties::Unevaluated unevaluated;
    uint8_t maxzoom = util::TERRAIN_RGB_MAXZOOM;

    std::array<float, 2> getLatRange(const UnwrappedTileID& id);
    std::array<float, 2> getLight(const PaintParameters& parameters);

    gfx::ShaderProgramBasePtr hillshadePrepareShader;
    gfx::ShaderProgramBasePtr hillshadeShader;
    std::vector<RenderTargetPtr> activatedRenderTargets;

    using HillshadeVertexVector = gfx::VertexVector<HillshadeLayoutVertex>;
    std::shared_ptr<HillshadeVertexVector> staticDataSharedVertices;

    LayerTweakerPtr prepareLayerTweaker;
    // Flat-neutral prepared texture (RG=128 -> deriv 0): sampled by main
    // drawables whose prepare target has not baked yet, so fresh tiles shade
    // like flat terrain instead of undefined (black) memory.
    gfx::Texture2DPtr neutralPrepareTexture;

    // Last completed bake per tile, carried across bucket re-parses: a
    // re-parse recreates the prepare target, which holds nothing until its
    // bake runs — binding the previous bake for that window keeps the
    // tile's relief instead of blinking flat for a frame (the churn
    // shading-pop). Refreshed whenever a completed bake is seen, pruned
    // against the current render-tile cover, so it stays cover-sized and
    // extends a texture's lifetime only for the re-parse window itself.
    std::unordered_map<OverscaledTileID, gfx::Texture2DPtr> carriedBakeTextures;

    // Phase 2 drape routing: per-drape-target HillshadeLayerTweakers. One
    // entry per overlapping DEM drape RenderTarget, each rebinding the
    // hillshade quad's matrix into that target's space.
    std::unordered_map<OverscaledTileID, HillshadeLayerTweakerPtr> drapeLayerTweakers;

    // Last TileMask routed for each hillshade source tile. RasterDEM cover
    // changes can reuse the same bucket while replacing its parent/child
    // mask geometry; drape drawables must be rebuilt when that happens or
    // stale parent and child shade footprints overlap and double-darken the
    // terrain during small camera/zoom movements.
    std::unordered_map<OverscaledTileID, TileMask> drapeTileMasks;
};

} // namespace mbgl
