#include <mbgl/renderer/render_terrain_drape_cache.hpp>

#include <mbgl/gfx/context.hpp>
#include <mbgl/gfx/types.hpp>

namespace mbgl {

TerrainDrapeTargetPtr TerrainDrapeCache::getOrCreate(gfx::Context& context,
                                               const OverscaledTileID& tileID,
                                               Size size) {
    auto it = targetsByTileID.find(tileID);
    if (it != targetsByTileID.end()) {
        return it->second;
    }
    // 8-bit RGBA — matches the trove of color buffers MapLibre Native uses
    // elsewhere for offscreen passes. The terrain shader samples this as a
    // colour texture so we don't need HDR or float-precision storage here.
    auto target = context.createRenderTarget(size, gfx::TextureChannelDataType::UnsignedByte);
    targetsByTileID.emplace(tileID, target);
    return target;
}

TerrainDrapeTargetPtr TerrainDrapeCache::get(const OverscaledTileID& tileID) const {
    auto it = targetsByTileID.find(tileID);
    return it == targetsByTileID.end() ? nullptr : it->second;
}

} // namespace mbgl
