#pragma once

#include <mbgl/renderer/render_layer.hpp>
#include <mbgl/renderer/buckets/raster_bucket.hpp>
#include <mbgl/style/layers/raster_layer_impl.hpp>
#include <mbgl/style/layers/raster_layer_properties.hpp>
#include <mbgl/gfx/context.hpp>
#include <mbgl/tile/tile_id.hpp>

#include <array>
#include <unordered_map>

namespace mbgl {

class ImageSourceRenderData;
class RasterLayerTweaker;
using RasterLayerTweakerPtr = std::shared_ptr<RasterLayerTweaker>;

class RenderRasterLayer final : public RenderLayer {
public:
    explicit RenderRasterLayer(Immutable<style::RasterLayer::Impl>);
    ~RenderRasterLayer() override;

    /// Generate any changes needed by the layer
    void update(gfx::ShaderRegistry&,
                gfx::Context&,
                const TransformState&,
                const std::shared_ptr<UpdateParameters>&,
                const RenderTree&,
                UniqueChangeRequestVec&) override;

protected:
    /// @brief Called by the RenderOrchestrator during RenderTree construction.
    /// This event is run to indicate if the layer should render or not for the current frame.
    /// @param willRender Indicates if this layer should render or not
    /// @param changes The collection of current pending change requests
    void markLayerRenderable(bool willRender, UniqueChangeRequestVec&) override;

    /// @brief Called when the layer index changes
    /// This event is run when a layer is added or removed from the style.
    /// @param newLayerIndex The new layer index for this layer
    /// @param changes The collection of current pending change requests
    void layerIndexChanged(int32_t newLayerIndex, UniqueChangeRequestVec&) override;

    /// Called when the style layer is removed
    void layerRemoved(UniqueChangeRequestVec&) override;

private:
    void transition(const TransitionParameters&) override;
    void evaluate(const PropertyEvaluationParameters&) override;
    bool hasTransition() const override;
    bool hasCrossfade() const override;
    void prepare(const LayerPrepareParameters&) override;

    // Paint properties
    style::RasterPaintProperties::Unevaluated unevaluated;
    const ImageSourceRenderData* imageData = nullptr;

    gfx::ShaderProgramBasePtr rasterShader;
    LayerGroupPtr imageLayerGroup;

    using RasterVertexVector = gfx::VertexVector<RasterLayoutVertex>;
    using RasterVertexVectorPtr = std::shared_ptr<RasterVertexVector>;
    RasterVertexVectorPtr staticDataVertices;

    using TriangleIndexVector = gfx::IndexVector<gfx::Triangles>;
    using TriangleIndexVectorPtr = std::shared_ptr<TriangleIndexVector>;
    TriangleIndexVectorPtr staticDataIndices;

    using RasterSegmentVector = SegmentVector;
    using RasterSegmentVectorPtr = std::shared_ptr<RasterSegmentVector>;
    using SegmentVectorPtr = std::shared_ptr<SegmentVector>;
    SegmentVectorPtr staticDataSegments;

    // Phase 2 drape routing: per-drape-target RasterLayerTweakers.
    std::unordered_map<OverscaledTileID, RasterLayerTweakerPtr> drapeLayerTweakers;

    // .63 phase 3 (drape gap-fill): the source, captured in prepare(), so
    // update() can reach past the render set into the full tile pyramid
    // (active + cache) when a live canvas has no raster content to route.
    RenderSource* drapeGapfillSource = nullptr;
    // Cumulative gap-fill telemetry for the 1 Hz STAGE probe. Histogram
    // buckets by (canvas z − fill z): [0]=child(+1), [1]=equal, [2..4]=
    // ancestor 1..3 levels coarser, [5]=4+ levels (the "green mush" tail).
    std::size_t drapeGapfillFilled = 0;
    std::array<std::size_t, 6> drapeGapfillDzHist{};
};

} // namespace mbgl
