#pragma once

#include <mbgl/renderer/sources/render_tile_source.hpp>
#include <mbgl/style/sources/tile_source_impl.hpp>

namespace mbgl {

class RenderRasterSource final : public RenderTileSetSource {
public:
    explicit RenderRasterSource(Immutable<style::TileSource::Impl>, const TaggedScheduler&);

    // .63: drape gap-fill candidate reservoir — every tile the pyramid
    // still holds (active + cache) with a parsed bucket.
    void visitRasterTileBuckets(const std::function<void(const OverscaledTileID&, RasterBucket&)>&) override;

private:
    void prepare(const SourcePrepareParameters&) final;

    std::unordered_map<std::string, std::vector<Feature>> queryRenderedFeatures(
        const ScreenLineString& geometry,
        const TransformState& transformState,
        const std::unordered_map<std::string, const RenderLayer*>& layers,
        const RenderedQueryOptions& options,
        const mat4& projMatrix) const override;

    std::vector<Feature> querySourceFeatures(const SourceQueryOptions&) const override;

    // RenderTileSetSource overrides
    void updateInternal(const Tileset&,
                        const std::vector<Immutable<style::LayerProperties>>&,
                        bool needsRendering,
                        bool needsRelayout,
                        const TileParameters&) override;
    const std::optional<Tileset>& getTileset() const override;

    const style::TileSource::Impl& impl() const;
};

} // namespace mbgl
