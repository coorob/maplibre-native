#pragma once

#include <mbgl/renderer/render_target.hpp>
#include <mbgl/tile/tile_id.hpp>
#include <mbgl/util/size.hpp>

#include <memory>
#include <unordered_map>
#include <vector>

namespace mbgl {

namespace gfx {
class Context;
} // namespace gfx

// `render_target.hpp` declares `class RenderTarget` but not the
// `TerrainDrapeTargetPtr` alias. The alias is duplicated across
// change_request.hpp / gfx/context.hpp / vulkan/context.cpp etc., which
// means a header that only pulls in render_target.hpp can't actually
// name a target pointer. Use the explicit shared_ptr type here so this
// header is self-contained.
using TerrainDrapeTargetPtr = std::shared_ptr<RenderTarget>;

/// Tile-keyed cache of `RenderTarget`s used to drape the 2D map content over
/// the 3D terrain mesh.
///
/// Conceptually, when terrain is enabled the painter renders each visible
/// DEM tile's 2D layers (basemap raster/vector tiles, lines, fills, etc.)
/// into a tile-sized offscreen `RenderTarget` instead of straight to the
/// final framebuffer. The terrain drawable for that tile then samples this
/// target as its surface texture while the vertex shader displaces the
/// mesh against the DEM. This mirrors how `maplibre-gl-js` runs its drape
/// pass (`renderToTexture` in painter.ts).
///
/// Phase 1 (this class) just establishes the cache. Wiring layer drawables
/// into per-tile targets and binding the resulting texture into terrain
/// drawables is Phase 2 / Phase 3 — see FINISH_TERRAIN.md.
class TerrainDrapeCache {
public:
    TerrainDrapeCache() = default;
    ~TerrainDrapeCache() = default;

    TerrainDrapeCache(const TerrainDrapeCache&) = delete;
    TerrainDrapeCache& operator=(const TerrainDrapeCache&) = delete;

    /// Look up the RenderTarget for the given tile, allocating a new one
    /// (sized to `size` pixels square, in the given channel format) if
    /// missing. Reusing an existing target preserves its layer-group
    /// bindings across frames so we don't constantly re-add the same
    /// drawables. .58: mid/far tiers pass UnsignedShort565 (half the bytes,
    /// no alpha — drape targets clear to the style background so no alpha
    /// is ever needed); the near ring stays 8-bit RGBA.
    TerrainDrapeTargetPtr getOrCreate(gfx::Context& context,
                                      const OverscaledTileID& tileID,
                                      Size size,
                                      gfx::TextureChannelDataType channelType = gfx::TextureChannelDataType::UnsignedByte);

    /// Returns the cached target for tileID if any, otherwise nullptr. Does
    /// not allocate.
    TerrainDrapeTargetPtr get(const OverscaledTileID& tileID) const;

    /// Remove and return a target without destroying it immediately. The
    /// caller can then emit a matching RemoveRenderTargetRequest before
    /// replacing it with a differently sized target for the same tile.
    TerrainDrapeTargetPtr take(const OverscaledTileID& tileID) {
        auto it = targetsByTileID.find(tileID);
        if (it == targetsByTileID.end()) {
            return nullptr;
        }
        auto target = std::move(it->second);
        targetsByTileID.erase(it);
        return target;
    }

    /// Evict targets for tile IDs that the predicate marks for removal.
    /// Returns the (id, target) pairs removed so the caller can emit a
    /// RemoveRenderTargetRequest for each before the targets' lifetimes
    /// end here.
    template <typename Predicate /* bool(const OverscaledTileID&) */>
    std::vector<std::pair<OverscaledTileID, TerrainDrapeTargetPtr>> pruneIf(Predicate shouldEvict) {
        std::vector<std::pair<OverscaledTileID, TerrainDrapeTargetPtr>> removed;
        for (auto it = targetsByTileID.begin(); it != targetsByTileID.end();) {
            if (shouldEvict(it->first)) {
                removed.emplace_back(it->first, it->second);
                it = targetsByTileID.erase(it);
            } else {
                ++it;
            }
        }
        return removed;
    }

    /// Iterate every cached target.
    template <typename Func /* void(const OverscaledTileID&, TerrainDrapeTargetPtr&) */>
    void visitAll(Func f) {
        for (auto& entry : targetsByTileID) {
            f(entry.first, entry.second);
        }
    }

    /// Iterate every cached target (const overload).
    template <typename Func /* void(const OverscaledTileID&, const TerrainDrapeTargetPtr&) */>
    void visitAll(Func f) const {
        for (const auto& entry : targetsByTileID) {
            f(entry.first, entry.second);
        }
    }

    /// Number of cached targets.
    size_t size() const noexcept { return targetsByTileID.size(); }

    /// Drop every cached target (e.g., on terrain teardown).
    void clear() noexcept { targetsByTileID.clear(); }

private:
    std::unordered_map<OverscaledTileID, TerrainDrapeTargetPtr> targetsByTileID;
};

} // namespace mbgl
