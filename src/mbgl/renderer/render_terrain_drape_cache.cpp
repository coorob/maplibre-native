#include <mbgl/renderer/render_terrain_drape_cache.hpp>

#include <mbgl/gfx/context.hpp>
#include <mbgl/gfx/types.hpp>
#include <mbgl/util/logging.hpp>

#include <cstdlib>
#include <cstdint>

namespace mbgl {

namespace {

bool klattraLogDrapeTrace() {
    static const bool enabled = std::getenv("KLATTRA_LOG_DRAPE_TRACE") != nullptr;
    return enabled;
}

std::string klattraTileString(const OverscaledTileID& id) {
    return "z" + std::to_string(id.canonical.z) +
           "/" + std::to_string(id.canonical.x) +
           "/" + std::to_string(id.canonical.y) +
           "=>z" + std::to_string(id.overscaledZ);
}

} // namespace

TerrainDrapeTargetPtr TerrainDrapeCache::getOrCreate(gfx::Context& context,
                                               const OverscaledTileID& tileID,
                                               Size size) {
    auto it = targetsByTileID.find(tileID);
    if (it != targetsByTileID.end()) {
        if (klattraLogDrapeTrace()) {
            Log::Info(Event::Render,
                      "[KLATTRA DRAPE_TRACE] cache-hit tile=" + klattraTileString(tileID) +
                          " target=" + it->second->getDebugName() +
                          " ptr=" + std::to_string(reinterpret_cast<uintptr_t>(it->second.get())) +
                          " completed=" + std::to_string(it->second->getCompletedRenderCount()) +
                          " groups=" + std::to_string(it->second->numLayerGroups()));
        }
        return it->second;
    }
    // 8-bit RGBA — matches the trove of color buffers MapLibre Native uses
    // elsewhere for offscreen passes. The terrain shader samples this as a
    // colour texture so we don't need HDR or float-precision storage here.
    auto target = context.createRenderTarget(size, gfx::TextureChannelDataType::UnsignedByte);
    if (target) {
        target->setMipmapped(true);
        target->setDebugName("terrain-drape " + klattraTileString(tileID));
        if (klattraLogDrapeTrace()) {
            Log::Info(Event::Render,
                      "[KLATTRA DRAPE_TRACE] cache-create tile=" + klattraTileString(tileID) +
                          " target=" + target->getDebugName() +
                          " ptr=" + std::to_string(reinterpret_cast<uintptr_t>(target.get())) +
                          " size=" + std::to_string(size.width) + "x" + std::to_string(size.height) +
                          " mipmapped=1");
        }
    }
    targetsByTileID.emplace(tileID, target);
    return target;
}

TerrainDrapeTargetPtr TerrainDrapeCache::get(const OverscaledTileID& tileID) const {
    auto it = targetsByTileID.find(tileID);
    return it == targetsByTileID.end() ? nullptr : it->second;
}

} // namespace mbgl
