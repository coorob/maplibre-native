#include <mbgl/renderer/sources/render_raster_source.hpp>
#include <mbgl/renderer/render_tile.hpp>
#include <mbgl/tile/raster_tile.hpp>
#include <mbgl/algorithm/update_tile_masks.hpp>
#include <mbgl/renderer/tile_parameters.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>

namespace mbgl {

using namespace style;

RenderRasterSource::RenderRasterSource(Immutable<style::TileSource::Impl> impl_, const TaggedScheduler& threadPool_)
    : RenderTileSetSource(std::move(impl_), threadPool_) {}

inline const style::TileSource::Impl& RenderRasterSource::impl() const {
    return static_cast<const style::TileSource::Impl&>(*baseImpl);
}

const std::optional<Tileset>& RenderRasterSource::getTileset() const {
    return impl().tileset;
}

void RenderRasterSource::updateInternal(const Tileset& tileset,
                                        const std::vector<Immutable<LayerProperties>>& layers,
                                        const bool needsRendering,
                                        const bool needsRelayout,
                                        const TileParameters& parameters) {
    // Mirror the DEM source's variable-zoom pitch gate (40°, see
    // render_raster_dem_source.cpp): at trail-preview pitches (46–63°) the
    // stock 60° gate emits NO coarse far-field tiles, so terrain drape
    // targets beyond the near field have no raster tile to route imagery
    // from — they baked base-colour only ("beige wedges" residue,
    // 2026-07-04 sim histogram: ~102/112 targets content-less). With the
    // gate lowered, the far field is served by few coarse parents instead
    // of an unbounded full-frustum full-zoom cover — also a net tile-count
    // and disk-write win. Same env knob as the DEM source.
    TileParameters rasterParameters = parameters;
    {
        const char* v = std::getenv("KLATTRA_TERRAIN_LOD_PITCH_DEG");
        double deg = 40.0;
        if (v && *v) {
            char* end = nullptr;
            const double parsed = std::strtod(v, &end);
            if (end != v) deg = std::clamp(parsed, 20.0, 60.0);
        }
        rasterParameters.tileLodPitchThreshold = deg * M_PI / 180.0;
    }
    tilePyramid.update(layers,
                       needsRendering,
                       needsRelayout,
                       rasterParameters,
                       *baseImpl,
                       impl().getTileSize(),
                       tileset.zoomRange,
                       tileset.bounds,
                       [&](const OverscaledTileID& tileID, TileObserver* observer_) {
                           return std::make_unique<RasterTile>(tileID, baseImpl->id, parameters, tileset, observer_);
                       });
    algorithm::updateTileMasks(tilePyramid.getRenderedTiles());

    // Probe: 1 Hz rendered-pyramid histogram for the beige-wedge hunt — is
    // there ANY coarse far-field imagery tile for drape routing to pick up?
    // stderr because the simulator swallows mbgl Log::Warning.
    if (std::getenv("KLATTRA_TRACE_STDERR") != nullptr) {
        static std::chrono::steady_clock::time_point lastLog{};
        const auto now = std::chrono::steady_clock::now();
        if (now - lastLog >= std::chrono::seconds(1)) {
            lastLog = now;
            std::array<uint32_t, 26> byZ{};
            std::size_t rendered = 0;
            for (const auto& [renderedID, tileRef] : tilePyramid.getRenderedTiles()) {
                (void)tileRef;
                ++byZ[std::min<std::size_t>(renderedID.canonical.z, byZ.size() - 1)];
                ++rendered;
            }
            std::string hist;
            for (std::size_t z = 0; z < byZ.size(); ++z) {
                if (!byZ[z]) continue;
                if (!hist.empty()) hist += ' ';
                hist += 'z' + std::to_string(z) + ':' + std::to_string(byZ[z]);
            }
            fprintf(stderr,
                    "[KLATTRA_TRACE] [KLATTRA RASTER] source=%s rendered=%zu byZ=%s zoom=%.2f pitchDeg=%.1f\n",
                    baseImpl->id.c_str(),
                    rendered,
                    hist.empty() ? "-" : hist.c_str(),
                    parameters.transformState.getZoom(),
                    parameters.transformState.getPitch() * 180.0 / M_PI);
        }
    }
}

void RenderRasterSource::prepare(const SourcePrepareParameters& parameters) {
    RenderTileSource::prepare(parameters);
}

std::unordered_map<std::string, std::vector<Feature>> RenderRasterSource::queryRenderedFeatures(
    const ScreenLineString&,
    const TransformState&,
    const std::unordered_map<std::string, const RenderLayer*>&,
    const RenderedQueryOptions&,
    const mat4&) const {
    return std::unordered_map<std::string, std::vector<Feature>>{};
}

std::vector<Feature> RenderRasterSource::querySourceFeatures(const SourceQueryOptions&) const {
    return {};
}

} // namespace mbgl
