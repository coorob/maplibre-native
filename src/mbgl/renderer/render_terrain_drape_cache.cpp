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
                                               Size size,
                                               gfx::TextureChannelDataType channelType) {
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
    // The terrain shader samples this as a colour texture so we don't need
    // HDR or float precision; near tiles use 8-bit RGBA, mid/far tiers pass
    // packed 565 (see RenderTerrain's tier selection).
    auto target = context.createRenderTarget(size, channelType);
    if (!target) {
        // .56: do NOT cache the failure. Under memory pressure the backend
        // can return null; caching it left the tile canvas-less — a solid
        // style-background plate on the mesh — for as long as it stayed in
        // cover, with no retry and no probe visibility. Returning without
        // inserting lets the next frame's getOrCreate try again.
        Log::Warning(Event::Render,
                     "[KLATTRA DRAPE] render-target allocation FAILED tile=" + klattraTileString(tileID) +
                         " size=" + std::to_string(size.width));
        return nullptr;
    }
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
    targetsByTileID.emplace(tileID, target);
    return target;
}

TerrainDrapeTargetPtr TerrainDrapeCache::get(const OverscaledTileID& tileID) const {
    auto it = targetsByTileID.find(tileID);
    return it == targetsByTileID.end() ? nullptr : it->second;
}

} // namespace mbgl
