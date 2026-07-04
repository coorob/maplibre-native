#include <mbgl/renderer/render_terrain.hpp>
#include <mbgl/renderer/update_parameters.hpp>
#include <mbgl/renderer/render_source.hpp>
#include <mbgl/renderer/render_tile.hpp>
#include <mbgl/renderer/render_pass.hpp>
#include <mbgl/renderer/render_tree.hpp>
#include <mbgl/renderer/render_layer.hpp>
#include <mbgl/renderer/render_static_data.hpp>
#include <mbgl/renderer/render_orchestrator.hpp>
#include <mbgl/renderer/change_request.hpp>
#include <mbgl/renderer/layer_group.hpp>
#include <mbgl/renderer/layers/terrain_layer_tweaker.hpp>
#include <mbgl/renderer/buckets/hillshade_bucket.hpp>
#include <mbgl/map/transform_state.hpp>
#include <mbgl/geometry/dem_data.hpp>
#include <mbgl/tile/raster_dem_tile.hpp>
#include <mbgl/tile/tile.hpp>
#include <mbgl/gfx/context.hpp>
#include <mbgl/gfx/drawable.hpp>
#include <mbgl/gfx/drawable_impl.hpp>
#include <mbgl/gfx/drawable_builder.hpp>
#include <mbgl/gfx/shader_registry.hpp>
#include <mbgl/gfx/color_mode.hpp>
#include <mbgl/gfx/texture2d.hpp>
#include <mbgl/gfx/vertex_attribute.hpp>
#include <mbgl/gfx/vertex_vector.hpp>
#include <mbgl/shaders/shader_source.hpp>
#include <mbgl/shaders/terrain_layer_ubo.hpp>
#include <mbgl/shaders/shader_defines.hpp>
#include <mbgl/shaders/segment.hpp>
#include <mbgl/util/constants.hpp>
#include <mbgl/util/projection.hpp>
#include <mbgl/util/logging.hpp>
#include <mbgl/util/image.hpp>
#include <mbgl/util/mat4.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <limits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mbgl {

namespace {

bool klattraLogDrapeTrace() {
    static const bool enabled = std::getenv("KLATTRA_LOG_DRAPE_TRACE") != nullptr;
    return enabled;
}

bool klattraTraceStderr() {
    static const bool enabled = std::getenv("KLATTRA_TRACE_STDERR") != nullptr;
    return enabled;
}

void klattraTrace(const std::string& message) {
    if (!klattraTraceStderr()) return;
    std::fprintf(stderr, "[KLATTRA_TRACE] %s\n", message.c_str());
}

std::string klattraTileString(const OverscaledTileID& id) {
    return "z" + std::to_string(id.canonical.z) +
           "/" + std::to_string(id.canonical.x) +
           "/" + std::to_string(id.canonical.y) +
           "=>z" + std::to_string(id.overscaledZ);
}

std::string klattraTexturePtrString(const std::shared_ptr<gfx::Texture2D>& texture) {
    return texture ? std::to_string(reinterpret_cast<uintptr_t>(texture.get())) : "0";
}

struct TerrainLayoutVertex {
    std::array<int16_t, 2> pos;
    std::array<int16_t, 2> texturePos;
};

std::size_t klattraDrapeDrawableCount(const TerrainDrapeTargetPtr& target) {
    if (!target) return 0;
    std::size_t count = 0;
    target->visitLayerGroups([&](LayerGroupBase& layerGroup) { count += layerGroup.getDrawableCount(); });
    return count;
}

bool klattraDrapeTargetReady(const TerrainDrapeTargetPtr& target) {
    return RenderTerrain::isDrapeTargetReady(target);
}

bool klattraDrapeTargetReadyForTile(const TerrainDrapeTargetPtr& target, const OverscaledTileID& tileID) {
    return RenderTerrain::isDrapeTargetReadyForTile(target, tileID);
}

bool klattraIsAncestorOf(const OverscaledTileID& ancestor, const OverscaledTileID& child) {
    return ancestor.canonical.z < child.canonical.z && LayerTweaker::tilesOverlap(ancestor, child);
}

std::pair<std::array<float, 2>, float> klattraSubrectForChildInAncestor(const OverscaledTileID& child,
                                                                        const OverscaledTileID& ancestor) {
    const uint8_t dz = static_cast<uint8_t>(child.canonical.z - ancestor.canonical.z);
    const uint32_t mask = (1u << dz) - 1u;
    const uint32_t subX = child.canonical.x & mask;
    const uint32_t subY = child.canonical.y & mask;
    const float scale = 1.0f / static_cast<float>(1u << dz);
    return {{{static_cast<float>(subX) * scale, static_cast<float>(subY) * scale}}, scale};
}

int32_t klattraEnvTargetSize(const char* name, int32_t fallback) {
    const char* value = std::getenv(name);
    if (!value) return fallback;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value) return fallback;
    return static_cast<int32_t>(std::clamp<long>(parsed, 256, 4096));
}

uint32_t klattraEnvTileCount(const char* name, uint32_t fallback) {
    const char* value = std::getenv(name);
    if (!value) return fallback;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value) return fallback;
    return static_cast<uint32_t>(std::clamp<unsigned long>(parsed, 0, 512));
}

uint32_t klattraEnvFrameCount(const char* name, uint32_t fallback) {
    const char* value = std::getenv(name);
    if (!value) return fallback;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value) return fallback;
    return static_cast<uint32_t>(std::clamp<unsigned long>(parsed, 0, 120));
}

uint32_t klattraEnvLevelCount(const char* name, uint32_t fallback) {
    const char* value = std::getenv(name);
    if (!value) return fallback;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value) return fallback;
    return static_cast<uint32_t>(std::clamp<unsigned long>(parsed, 0, 4));
}

uint32_t klattraEnvTilePadding(const char* name, uint32_t fallback) {
    const char* value = std::getenv(name);
    if (!value) return fallback;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value) return fallback;
    return static_cast<uint32_t>(std::clamp<unsigned long>(parsed, 0, 2));
}

void klattraAddDrapeOverscan(std::unordered_set<OverscaledTileID>& drapeIDs,
                             const std::unordered_set<OverscaledTileID>& idealIDs,
                             uint32_t padding) {
    if (padding == 0 || idealIDs.empty()) {
        return;
    }

    const int32_t pad = static_cast<int32_t>(padding);
    const std::size_t ringArea = static_cast<std::size_t>((pad * 2 + 1) * (pad * 2 + 1) - 1);
    drapeIDs.reserve(drapeIDs.size() + idealIDs.size() * ringArea);

    for (const auto& tileID : idealIDs) {
        const auto z = tileID.canonical.z;
        const auto worldSize = int64_t{1} << z;
        const auto baseX = static_cast<int64_t>(tileID.wrap) * worldSize +
                           static_cast<int64_t>(tileID.canonical.x);
        const auto baseY = static_cast<int64_t>(tileID.canonical.y);

        for (int32_t dy = -pad; dy <= pad; ++dy) {
            const auto y = baseY + dy;
            if (y < 0 || y >= worldSize) {
                continue;
            }
            for (int32_t dx = -pad; dx <= pad; ++dx) {
                if (dx == 0 && dy == 0) {
                    continue;
                }
                const UnwrappedTileID neighbor(z, baseX + dx, y);
                drapeIDs.emplace(tileID.overscaledZ, neighbor.wrap, neighbor.canonical);
            }
        }
    }
}

} // namespace

RenderTerrain::RenderTerrain(Immutable<style::Terrain::Impl> impl_)
    : impl(std::move(impl_)) {
}

RenderTerrain::~RenderTerrain() = default;

void RenderTerrain::update(const UpdateParameters& parameters) {
    // Find the DEM source if we haven't already
    if (!demSource && !impl->sourceID.empty()) {
        // In a full implementation, we would look up the source from parameters.sources
        // and cache the RenderSource pointer
        // For now, this is a placeholder
    }
}

void RenderTerrain::update(RenderOrchestrator& orchestrator,
                           gfx::ShaderRegistry& shaders,
                           gfx::Context& context,
                           const TransformState& state,
                           const std::shared_ptr<UpdateParameters>& /*updateParameters*/,
                           const RenderTree& renderTree,
                           UniqueChangeRequestVec& changes) {
    // Re-resolve the DEM source every update. The orchestrator owns the
    // RenderSources and can destroy them behind us (source removal,
    // RenderOrchestrator::clearData on style swap) while this RenderTerrain
    // survives — Terrain::Impl compares equal across styles sharing
    // sourceID + exaggeration — so a pointer cached across frames can
    // dangle (same UAF class as the PMTilesFileSource teardown bug). One
    // map lookup per frame is cheap; a vanished source then routes through
    // the clearRenderState path below, which also releases GPU targets.
    demSource = impl->sourceID.empty() ? nullptr : orchestrator.getRenderSource(impl->sourceID);
    if (!demSource && !impl->sourceID.empty()) {
        Log::Warning(Event::Render, "Terrain could not find DEM source: " + impl->sourceID);
        klattraTrace("terrain no-dem-source source=" + impl->sourceID);
    }

    // Create layer group if we don't have one
    if (!layerGroup) {
        if (auto layerGroup_ = context.createLayerGroup(TERRAIN_LAYER_INDEX, /*initialCapacity=*/1, "terrain")) {
            layerGroup = std::move(layerGroup_);
            activateLayerGroup(true, changes);
        } else {
            Log::Error(Event::Render, "Failed to create terrain layer group");
            return;
        }
    }

    if (!tweaker) {
        tweaker = std::make_unique<TerrainLayerTweaker>(this);
    }

    const bool traceDrape = klattraLogDrapeTrace();
    static uint64_t drapeTraceFrameCounter = 0;
    const uint64_t drapeTraceFrame = traceDrape ? ++drapeTraceFrameCounter : 0;

    // If we don't have a DEM source, we can't create terrain drawables
    if (!demSource) {
        klattraTrace("terrain update-no-dem source=" + impl->sourceID);
        if (traceDrape) {
            Log::Info(Event::Render,
                      "[KLATTRA DRAPE_TRACE] update-no-dem frame=" + std::to_string(drapeTraceFrame) +
                          " source=" + impl->sourceID);
        }
        clearRenderState(changes);
        return;
    }

    auto renderTiles = demSource->getRawRenderTiles();
    if (renderTiles->empty()) {
        klattraTrace("terrain update-empty-cover source=" + impl->sourceID +
                     " previousBindings=" + std::to_string(currentBindings.size()) +
                     " previousDrapeTargets=" + std::to_string(drapeCache.size()));
        if (traceDrape) {
            Log::Info(Event::Render,
                      "[KLATTRA DRAPE_TRACE] update-empty-cover frame=" + std::to_string(drapeTraceFrame) +
                          " source=" + impl->sourceID +
                          " previousBindings=" + std::to_string(currentBindings.size()) +
                          " previousDrapeTargets=" + std::to_string(drapeCache.size()));
        }
        clearRenderState(changes);
        return;
    }

    auto* lg = static_cast<LayerGroup*>(layerGroup.get());
    if (!lg) {
        return;
    }

    if (traceDrape) {
        Log::Info(Event::Render,
                  "[KLATTRA DRAPE_TRACE] update-begin frame=" + std::to_string(drapeTraceFrame) +
                      " source=" + impl->sourceID +
                      " renderTiles=" + std::to_string(renderTiles->size()) +
                      " previousBindings=" + std::to_string(currentBindings.size()) +
                      " previousDemTextures=" + std::to_string(demTexturesByTile.size()) +
                      " previousDrapeTargets=" + std::to_string(drapeCache.size()) +
                      " terrainDrawables=" + std::to_string(lg->getDrawableCount()));
    }
    klattraTrace("terrain update-begin source=" + impl->sourceID +
                 " renderTiles=" + std::to_string(renderTiles->size()) +
                 " previousBindings=" + std::to_string(currentBindings.size()) +
                 " previousDemTextures=" + std::to_string(demTexturesByTile.size()) +
                 " previousDrapeTargets=" + std::to_string(drapeCache.size()) +
                 " terrainDrawables=" + std::to_string(lg->getDrawableCount()));

    // Build two sets up-front:
    //
    //  - `currentIdealIDs`: the IDEAL OverscaledTileID for each cover slot,
    //    constructed from `renderTile.id` (the UnwrappedTileID handed to
    //    `renderTileFn` in `algorithm::updateRenderables`). Used as the key
    //    for terrain mesh drawables. Distinct from the actual tile's id,
    //    which can be a parent in fallback. There's one drawable per ideal
    //    so a single z=10 parent feeding 16 z=12 ideals produces 16
    //    drawables — each at z=12 mesh density — sharing the parent's
    //    DEM texture via a UV sub-rect.
    //
    // Drape targets are keyed by ideal tile, not by DEM source tile. DEM
    // parent fallback is fine for elevation, but visible topo colour must
    // stay at the tile LOD the camera is actually drawing; otherwise a coarse
    // source parent can stretch low-zoom drape pixels across a close view.
    std::unordered_set<OverscaledTileID> currentIdealIDs;
    currentIdealIDs.reserve(renderTiles->size());
    for (const auto& renderTile : *renderTiles) {
        currentIdealIDs.emplace(renderTile.id.canonical.z, renderTile.id.wrap, renderTile.id.canonical);
    }

    // Exact drape targets give the final sharp topo texture. Coarser parent
    // targets give fast-moving cameras something stable to sample while new
    // exact targets bake. This mirrors the render-to-texture tile pyramid in
    // mature terrain renderers: show a cached parent immediately, sharpen to
    // the child only after it has completed.
    std::unordered_set<OverscaledTileID> currentDrapeIDs = currentIdealIDs;
    // Default 1: bake a ring of targets just outside the visible cover so a
    // moving camera reaches tiles whose drape is already ready. With the
    // per-target GPU waits gone (perf batch), frames present fast enough to
    // expose the bake window at the leading edge as black/blurry tiles —
    // the ring is what hides it. Ring targets rank as far in the
    // distance-ranked budgets, so they stay at the small end of the size
    // buckets.
    static const uint32_t drapeOverscanTiles =
        klattraEnvTilePadding("KLATTRA_DRAPE_OVERSCAN_TILES", 2);
    klattraAddDrapeOverscan(currentDrapeIDs, currentIdealIDs, drapeOverscanTiles);
    const std::vector<OverscaledTileID> exactAndOverscanDrapeIDs(currentDrapeIDs.begin(),
                                                                 currentDrapeIDs.end());
    // Terrain meshes stay tied to the DEM source cover so every raised tile
    // can resolve real elevation data (or a source-provided parent fallback).
    // The DEM source itself expands its pitched cover; drape targets can still
    // overscan further so satellite colour is ready before the mesh arrives.
    const std::unordered_set<OverscaledTileID>& terrainMeshIDs = currentIdealIDs;
    // One parent fallback level is back on by default: with distance-ranked
    // ring budgets bounding target sizes, the extra ~25% mostly-far targets
    // are cheap, and the parent texture is what stops fresh tiles flashing
    // in empty at the leading edge of a pan (validated on-sim 2026-06-10).
    static const uint32_t drapeFallbackLevels =
        klattraEnvLevelCount("KLATTRA_DRAPE_FALLBACK_LEVELS", 1);
    if (drapeFallbackLevels > 0) {
        for (const auto& tileID : exactAndOverscanDrapeIDs) {
            for (uint32_t level = 1; level <= drapeFallbackLevels; ++level) {
                if (tileID.canonical.z < level) {
                    break;
                }
                currentDrapeIDs.insert(tileID.scaledTo(static_cast<uint8_t>(tileID.canonical.z - level)));
            }
        }
    }

    const bool drapeCoverStable = currentIdealIDs == previousIdealIDs;
    const bool cameraChanging = state.isChanging() || state.isGestureInProgress();
    if (!cameraChanging && drapeCoverStable) {
        stableDrapeCoverFrames++;
    } else {
        stableDrapeCoverFrames = 0;
    }
    previousIdealIDs = currentIdealIDs;

    static const uint32_t qualityUpgradeFrames =
        klattraEnvFrameCount("KLATTRA_DRAPE_QUALITY_UPGRADE_FRAMES", 8);
    const bool useHighQualityDrape = !cameraChanging && stableDrapeCoverFrames >= qualityUpgradeFrames;
    if (traceDrape) {
        Log::Info(Event::Render,
                  "[KLATTRA DRAPE_TRACE] target-quality frame=" + std::to_string(drapeTraceFrame) +
                      " cameraChanging=" + std::to_string(cameraChanging) +
                      " coverStable=" + std::to_string(drapeCoverStable) +
                      " stableFrames=" + std::to_string(stableDrapeCoverFrames) +
                      " fallbackLevels=" + std::to_string(drapeFallbackLevels) +
                      " overscanTiles=" + std::to_string(drapeOverscanTiles) +
                      " exactDrapeTargets=" + std::to_string(exactAndOverscanDrapeIDs.size()) +
                      " activeDrapeTargets=" + std::to_string(currentDrapeIDs.size()) +
                      " highQuality=" + std::to_string(useHighQualityDrape));
    }

    // Phase 1 of the drape pass (see FINISH_TERRAIN.md / TERRAIN_PROGRESS.md):
    // ensure a per-tile RenderTarget exists before we create drawables for
    // those tiles, so the drawable-creation step can bind the matching
    // target's texture as its `mapTexture` (Phase 3). Phase 2 — routing
    // 2D layer drawables into these targets so they have real surface
    // content — is the next step.
    // Allocate a per-ideal-tile drape RenderTarget for each visible cover
    // slot if one doesn't already exist, and prune any targets whose ideal
    // tiles have dropped out of the cover set. The terrain mesh fragment
    // shader samples this target as the surface colour; basemap layers route
    // drawables into it (RenderBackgroundLayer / RenderFillLayer / ...).
    {
        // Distance-ranked drape budgets: spend texture memory where the user
        // is looking instead of by tile zoom. Every drape tile is ranked by
        // its centre's mercator distance to the camera centre; the nearest
        // KLATTRA_DRAPE_NEAR_TILES bake at nearSize even while the camera
        // moves (the old moving/still split made gestures soft, then popped
        // sharp on settle), the next KLATTRA_DRAPE_MID_TILES at midSize, and
        // the rest at farSize. Worst-case GPU memory is bounded by the ring
        // counts regardless of zoom or pitch — the per-zoom-bucket budgets
        // this replaces let a pitched 72-tile cover jetsam the app when the
        // z10 bucket was raised to 2048 (measured 3.3 GB on an iPhone 16 Pro).
        const LatLng cameraCenter = state.getLatLng();
        const double centerX = (cameraCenter.longitude() + 180.0) / 360.0;
        const double centerLatRad = cameraCenter.latitude() * M_PI / 180.0;
        const double centerY = 0.5 - std::log(std::tan(M_PI / 4.0 + centerLatRad / 2.0)) / (2.0 * M_PI);
        std::vector<std::pair<double, OverscaledTileID>> rankedDrapeIDs;
        rankedDrapeIDs.reserve(currentDrapeIDs.size());
        for (const auto& tileID : currentDrapeIDs) {
            const double scale = static_cast<double>(1u << tileID.canonical.z);
            const double dx = (tileID.canonical.x + 0.5) / scale + tileID.wrap - centerX;
            const double dy = (tileID.canonical.y + 0.5) / scale - centerY;
            rankedDrapeIDs.emplace_back(dx * dx + dy * dy, tileID);
        }
        std::sort(rankedDrapeIDs.begin(), rankedDrapeIDs.end(), [](const auto& a, const auto& b) {
            return a.first < b.first;
        });
        std::unordered_map<OverscaledTileID, uint32_t> drapeRankByTile;
        drapeRankByTile.reserve(rankedDrapeIDs.size());
        for (uint32_t i = 0; i < rankedDrapeIDs.size(); ++i) {
            drapeRankByTile.emplace(rankedDrapeIDs[i].second, i);
        }

        static const int32_t nearSize = klattraEnvTargetSize("KLATTRA_DRAPE_TARGET_SIZE_NEAR", DRAPE_TARGET_SIZE);
        static const int32_t midSize = klattraEnvTargetSize("KLATTRA_DRAPE_TARGET_SIZE_MID", 1024);
        static const int32_t farSize = klattraEnvTargetSize("KLATTRA_DRAPE_TARGET_SIZE_FAR", 512);
        static const uint32_t nearTiles = klattraEnvTileCount("KLATTRA_DRAPE_NEAR_TILES", 12);
        static const uint32_t midTiles = klattraEnvTileCount("KLATTRA_DRAPE_MID_TILES", 24);
        static const uint32_t ringSlack = klattraEnvTileCount("KLATTRA_DRAPE_RING_SLACK", 6);

        const auto drapeTargetSizeForTile = [&](const OverscaledTileID& tileID) -> int32_t {
            const auto rankIt = drapeRankByTile.find(tileID);
            const uint32_t rank = rankIt != drapeRankByTile.end() ? rankIt->second
                                                                  : std::numeric_limits<uint32_t>::max();
            const uint8_t desiredRing = rank < nearTiles ? 0 : (rank < nearTiles + midTiles ? 1 : 2);
            uint8_t ring = desiredRing;
            const auto ringIt = drapeRingByTile.find(tileID);
            if (ringIt != drapeRingByTile.end() && desiredRing > ringIt->second) {
                // Promote immediately, demote with slack: a tile keeps its
                // ring until its rank falls RING_SLACK past the boundary, so
                // panning does not flap targets at ring edges (every flip
                // reallocates and rebakes a render target).
                const uint32_t boundary = ringIt->second == 0 ? nearTiles : nearTiles + midTiles;
                ring = rank >= boundary + ringSlack ? desiredRing : ringIt->second;
            }
            drapeRingByTile[tileID] = ring;
            if (ring == 0) return nearSize;
            if (ring == 1) return midSize;
            return farSize;
        };

        // A fast pan/zoom can reshuffle every ring assignment in a single
        // update; resizing them all at once keeps old+new generations alive
        // simultaneously (retired copy + fresh allocation per tile) and can
        // transiently spike hundreds of MB above steady state. Cap resizes
        // per frame — skipped tiles keep rendering at their old size and are
        // picked up on following frames (0 = uncapped).
        static const uint32_t maxResizesPerFrame = klattraEnvTileCount("KLATTRA_DRAPE_MAX_RESIZES_PER_FRAME", 3);
        uint32_t resizesThisFrame = 0;
        for (const auto& tileID : currentDrapeIDs) {
            const int32_t targetSize = drapeTargetSizeForTile(tileID);
            const Size desiredSize{static_cast<uint32_t>(targetSize), static_cast<uint32_t>(targetSize)};
            if (auto existing = drapeCache.get(tileID)) {
                const Size existingSize = existing->getSize();
                // Resize both ways: ring promotions sharpen the tile, ring
                // demotions release the big target again — without them, long
                // pans accumulate near-ring targets until jetsam. The retired
                // target keeps rendering until its successor bakes.
                if (existingSize != desiredSize &&
                    (maxResizesPerFrame == 0 || resizesThisFrame < maxResizesPerFrame)) {
                    resizesThisFrame++;
                    auto oldTarget = drapeCache.take(tileID);
                    if (klattraDrapeTargetReady(oldTarget)) {
                        retiredDrapeTargetsByTile[tileID] = oldTarget;
                    } else {
                        retiredDrapeTargetsByTile.erase(tileID);
                    }
                    if (traceDrape) {
                        Log::Info(Event::Render,
                                  "[KLATTRA DRAPE_TRACE] cache-resize frame=" + std::to_string(drapeTraceFrame) +
                                      " tile=" + klattraTileString(tileID) +
                                      " oldSize=" + std::to_string(existingSize.width) + "x" +
                                          std::to_string(existingSize.height) +
                                      " newSize=" + std::to_string(desiredSize.width) + "x" +
                                          std::to_string(desiredSize.height) +
                                      " retiredReady=" + std::to_string(klattraDrapeTargetReady(oldTarget)) +
                                      " stableFrames=" + std::to_string(stableDrapeCoverFrames));
                    }
                    changes.emplace_back(std::make_unique<RemoveRenderTargetRequest>(oldTarget));
                }
            }

            const bool wasAllocated = drapeCache.get(tileID) != nullptr;
            auto target = drapeCache.getOrCreate(context, tileID, desiredSize);
            if (!wasAllocated && target) {
                changes.emplace_back(std::make_unique<AddRenderTargetRequest>(target));
            }
        }
        // Retain ready out-of-cover ancestors as fallback content, but CAP
        // them: uncapped retention accumulates one generation per zoom level
        // on a continuous zoom-in (~100–400 MB that no ring budget covers).
        // Deepest ancestors first — a z-1 parent is a far better fallback
        // than a z-5 one.
        static const uint32_t maxRetainedAncestors = klattraEnvTileCount("KLATTRA_DRAPE_MAX_RETAINED_ANCESTORS", 8);
        std::vector<OverscaledTileID> retainedAncestors;
        drapeCache.visitAll([&](const OverscaledTileID& id, const TerrainDrapeTargetPtr& target) {
            if (currentDrapeIDs.find(id) != currentDrapeIDs.end()) return;
            if (!klattraDrapeTargetReady(target)) return;
            for (const auto& idealID : currentIdealIDs) {
                if (klattraIsAncestorOf(id, idealID)) {
                    retainedAncestors.push_back(id);
                    return;
                }
            }
        });
        if (retainedAncestors.size() > maxRetainedAncestors) {
            std::sort(retainedAncestors.begin(),
                      retainedAncestors.end(),
                      [](const OverscaledTileID& a, const OverscaledTileID& b) {
                          return a.canonical.z > b.canonical.z;
                      });
            retainedAncestors.erase(retainedAncestors.begin() + maxRetainedAncestors, retainedAncestors.end());
        }
        const std::unordered_set<OverscaledTileID> ancestorKeep(retainedAncestors.begin(), retainedAncestors.end());
        auto evicted = drapeCache.pruneIf([&](const OverscaledTileID& id) {
            if (currentDrapeIDs.find(id) != currentDrapeIDs.end()) {
                return false;
            }
            return ancestorKeep.find(id) == ancestorKeep.end();
        });
        for (auto& [evictedID, evictedTarget] : evicted) {
            if (traceDrape) {
                Log::Info(Event::Render,
                          "[KLATTRA DRAPE_TRACE] cache-evict frame=" + std::to_string(drapeTraceFrame) +
                              " tile=" + klattraTileString(evictedID) +
                              " target=" + (evictedTarget ? evictedTarget->getDebugName() : std::string("null")) +
                              " ptr=" + std::to_string(reinterpret_cast<uintptr_t>(evictedTarget.get())) +
                              " completed=" + std::to_string(evictedTarget ? evictedTarget->getCompletedRenderCount() : 0) +
                              " groups=" + std::to_string(evictedTarget ? evictedTarget->numLayerGroups() : 0) +
                              " drawables=" + std::to_string(klattraDrapeDrawableCount(evictedTarget)));
            }
            // The orchestrator still holds the AddRenderTargetRequest's
            // shared_ptr to this target — emit a matching remove request
            // so it lets go and the GPU resources can release.
            changes.emplace_back(std::make_unique<RemoveRenderTargetRequest>(std::move(evictedTarget)));
        }
        for (auto it = retiredDrapeTargetsByTile.begin(); it != retiredDrapeTargetsByTile.end();) {
            if (currentDrapeIDs.find(it->first) == currentDrapeIDs.end()) {
                it = retiredDrapeTargetsByTile.erase(it);
            } else {
                ++it;
            }
        }
        // Release parked resize-predecessors as soon as their successor has
        // baked — for EVERY cover member, not just ideals. The DEM-binding
        // loop's erase only covers ideal tiles; fallback parents are never
        // ideals, so their retired 2048² copies used to survive until the
        // parent left cover entirely (~50–150 MB of dead targets during long
        // pitched pans).
        for (auto it = retiredDrapeTargetsByTile.begin(); it != retiredDrapeTargetsByTile.end();) {
            if (klattraDrapeTargetReady(drapeCache.get(it->first))) {
                it = retiredDrapeTargetsByTile.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = drapeRingByTile.begin(); it != drapeRingByTile.end();) {
            if (currentDrapeIDs.find(it->first) == currentDrapeIDs.end()) {
                it = drapeRingByTile.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Drape render targets are separate offscreen targets, so normal
    // RenderLayer::markLayerRenderable(false) only removes the main
    // framebuffer layer group. Prune stale drape groups here as well, or
    // hidden/zoomed-out style layers can stay baked into the terrain texture.
    {
        std::unordered_set<int32_t> activeDrapeLayerIndices;
        for (const auto& item : renderTree.getLayerRenderItemMap()) {
            activeDrapeLayerIndices.insert(item.layer.get().getLayerIndex());
        }

        std::size_t removedGroups = 0;
        drapeCache.visitAll([&](const OverscaledTileID& drapeID, const TerrainDrapeTargetPtr& target) {
            if (!target) return;
            const auto removed = target->removeLayerGroupsIf(
                [&](const int32_t layerIndex, const LayerGroupBase&) {
                    if (layerIndex == std::numeric_limits<int32_t>::max()) {
                        return false;
                    }
                    return activeDrapeLayerIndices.find(layerIndex) == activeDrapeLayerIndices.end();
                });
            removedGroups += removed;
            if (traceDrape && removed) {
                Log::Info(Event::Render,
                          "[KLATTRA DRAPE_TRACE] prune-inactive-groups frame=" +
                              std::to_string(drapeTraceFrame) +
                              " tile=" + klattraTileString(drapeID) +
                              " removed=" + std::to_string(removed));
            }
        });
        if (traceDrape && removedGroups) {
            Log::Info(Event::Render,
                      "[KLATTRA DRAPE_TRACE] prune-inactive-groups-total frame=" +
                          std::to_string(drapeTraceFrame) +
                          " removed=" + std::to_string(removedGroups) +
                          " activeLayers=" + std::to_string(activeDrapeLayerIndices.size()));
        }
    }

    // Refresh the CPU-side DEM image cache so getElevation() returns valid
    // values for every tile in cover. Holds a shared_ptr to the image so
    // sampling stays valid even if the source bucket gets torn down between
    // frames. Drops entries that left cover.
    {
        std::unordered_map<OverscaledTileID, std::shared_ptr<const PremultipliedImage>> next;
        next.reserve(renderTiles->size());
        std::size_t imageReady = 0;
        std::size_t imageMissing = 0;
        for (const auto& renderTile : *renderTiles) {
            const auto& tileID = renderTile.getOverscaledTileID();
            const auto& tile = renderTile.getTile();
            if (tile.kind != Tile::Kind::RasterDEM) {
                imageMissing++;
                continue;
            }
            const auto* demTile = static_cast<const RasterDEMTile*>(&tile);
            const auto* hillshadeBucket = const_cast<RasterDEMTile*>(demTile)->getBucket();
            if (!hillshadeBucket) {
                imageMissing++;
                if (traceDrape) {
                    Log::Info(Event::Render,
                              "[KLATTRA DRAPE_TRACE] dem-image-missing frame=" + std::to_string(drapeTraceFrame) +
                                  " tile=" + klattraTileString(tileID) +
                                  " reason=no-bucket");
                }
                continue;
            }
            const auto& imagePtr = hillshadeBucket->getDEMData().getImagePtr();
            if (imagePtr && !imagePtr->size.isEmpty()) {
                next.emplace(tileID, imagePtr);
                imageReady++;
            } else {
                imageMissing++;
                if (traceDrape) {
                    Log::Info(Event::Render,
                              "[KLATTRA DRAPE_TRACE] dem-image-missing frame=" + std::to_string(drapeTraceFrame) +
                                  " tile=" + klattraTileString(tileID) +
                                  " reason=no-image");
                }
            }
        }
        demImagesByTile = std::move(next);
        if (traceDrape) {
            Log::Info(Event::Render,
                      "[KLATTRA DRAPE_TRACE] dem-image-summary frame=" + std::to_string(drapeTraceFrame) +
                          " ready=" + std::to_string(imageReady) +
                          " missing=" + std::to_string(imageMissing));
        }
        if (auto sampledElevation = getElevationAtLatLng(state.getLatLng())) {
            elevationOriginMeters = *sampledElevation;
            klattraTrace("terrain origin elevation=" + std::to_string(*sampledElevation));
        }
    }

    // Refresh the GPU DEM texture cache. Keyed by the actual DEM tile's
    // OverscaledTileID (not the cover slot's ideal), so a single texture
    // can back multiple ideal drawables that all parent-fallback into the
    // same source. Persists entries across frames as long as the source
    // tile remains reachable from cover (= present in the source's
    // renderTiles), so panning doesn't force a re-upload of cached DEMs.
    {
        std::unordered_map<OverscaledTileID, std::shared_ptr<gfx::Texture2D>> nextTextures;
        nextTextures.reserve(renderTiles->size());
        std::size_t reusedTextures = 0;
        std::size_t createdTextures = 0;
        std::size_t missingTextures = 0;
        for (const auto& renderTile : *renderTiles) {
            const auto& tile = renderTile.getTile();
            if (tile.kind != Tile::Kind::RasterDEM) {
                missingTextures++;
                continue;
            }
            const auto& sourceID = tile.id;
            if (nextTextures.find(sourceID) != nextTextures.end()) continue;

            auto* demTile = const_cast<RasterDEMTile*>(static_cast<const RasterDEMTile*>(&tile));
            auto* hillshadeBucket = demTile ? demTile->getBucket() : nullptr;
            if (!hillshadeBucket) {
                missingTextures++;
                if (traceDrape) {
                    Log::Info(Event::Render,
                              "[KLATTRA DRAPE_TRACE] dem-texture-missing frame=" + std::to_string(drapeTraceFrame) +
                                  " source=" + klattraTileString(sourceID) +
                                  " reason=no-bucket");
                }
                continue;
            }
            const auto& demData = hillshadeBucket->getDEMData();
            auto imagePtr = demData.getImagePtr();
            if (!imagePtr || imagePtr->size.isEmpty()) {
                missingTextures++;
                if (traceDrape) {
                    Log::Info(Event::Render,
                              "[KLATTRA DRAPE_TRACE] dem-texture-missing frame=" + std::to_string(drapeTraceFrame) +
                                  " source=" + klattraTileString(sourceID) +
                                  " reason=no-image");
                }
                continue;
            }

            if (auto existing = demTexturesByTile.find(sourceID); existing != demTexturesByTile.end()) {
                nextTextures.emplace(sourceID, existing->second);
                reusedTextures++;
                if (traceDrape) {
                    Log::Info(Event::Render,
                              "[KLATTRA DRAPE_TRACE] dem-texture-reuse frame=" + std::to_string(drapeTraceFrame) +
                                  " source=" + klattraTileString(sourceID) +
                                  " texture=" + klattraTexturePtrString(existing->second));
                }
            } else if (auto texture = createDEMTexture(context, demData)) {
                if (traceDrape) {
                    Log::Info(Event::Render,
                              "[KLATTRA DRAPE_TRACE] dem-texture-create frame=" + std::to_string(drapeTraceFrame) +
                                  " source=" + klattraTileString(sourceID) +
                                  " texture=" + klattraTexturePtrString(texture));
                }
                nextTextures.emplace(sourceID, std::move(texture));
                createdTextures++;
            } else {
                missingTextures++;
            }
        }
        demTexturesByTile = std::move(nextTextures);
        if (traceDrape) {
            Log::Info(Event::Render,
                      "[KLATTRA DRAPE_TRACE] dem-texture-summary frame=" + std::to_string(drapeTraceFrame) +
                          " reused=" + std::to_string(reusedTextures) +
                          " created=" + std::to_string(createdTextures) +
                          " missing=" + std::to_string(missingTextures) +
                          " active=" + std::to_string(demTexturesByTile.size()));
        }
    }

    // Compute per-ideal DEM bindings. Each cover slot resolves to either
    // the exact source tile's texture (identity UV remap) or — when the
    // exact-zoom data is still in flight — to whichever ancestor's texture
    // happens to be available, with a sub-rect UV remap.
    std::unordered_map<OverscaledTileID, DEMBinding> nextBindings;
    nextBindings.reserve(terrainMeshIDs.size());
    std::size_t readyBindings = 0;
    std::size_t emptyDemBindings = 0;
    std::size_t fallbackDrapeBindings = 0;
    for (const auto& idealOS : terrainMeshIDs) {
        std::optional<OverscaledTileID> resolvedSourceID;
        std::shared_ptr<gfx::Texture2D> resolvedTexture;
        if (auto exact = demTexturesByTile.find(idealOS); exact != demTexturesByTile.end()) {
            resolvedSourceID = idealOS;
            resolvedTexture = exact->second;
        } else {
            for (const auto& [candidateID, texture] : demTexturesByTile) {
                if (!texture || !klattraIsAncestorOf(candidateID, idealOS)) {
                    continue;
                }
                if (!resolvedSourceID || candidateID.canonical.z > resolvedSourceID->canonical.z) {
                    resolvedSourceID = candidateID;
                    resolvedTexture = texture;
                }
            }
        }

        DEMBinding binding(resolvedTexture, resolvedSourceID.value_or(idealOS));
        if (binding.texture && resolvedSourceID && idealOS.canonical.z > resolvedSourceID->canonical.z) {
            auto [demTL, demScale] = klattraSubrectForChildInAncestor(idealOS, *resolvedSourceID);
            binding.demTL = demTL;
            binding.demScale = demScale;
        } else if (!binding.texture) {
            // No DEM data anywhere in the loaded parent chain for this
            // overscanned tile. Keep the mesh alive with a flat DEM texture
            // for the frame instead of leaving a hole in the pitched view.
            binding.texture = getOrCreateEmptyDEMTexture(context);
            binding.sourceID = idealOS;
            binding.usedEmptyDEM = true;
        }

        TerrainDrapeTargetPtr drape = drapeCache.get(idealOS);
        OverscaledTileID resolvedDrapeID = idealOS;
        if (!klattraDrapeTargetReadyForTile(drape, idealOS)) {
            auto retired = retiredDrapeTargetsByTile.find(idealOS);
            if (retired != retiredDrapeTargetsByTile.end() &&
                klattraDrapeTargetReadyForTile(retired->second, idealOS)) {
                drape = retired->second;
            } else {
                TerrainDrapeTargetPtr bestAncestor;
                std::optional<OverscaledTileID> bestAncestorID;
                drapeCache.visitAll([&](const OverscaledTileID& candidateID, const TerrainDrapeTargetPtr& candidate) {
                    if (!klattraDrapeTargetReadyForTile(candidate, idealOS) ||
                        !klattraIsAncestorOf(candidateID, idealOS)) {
                        return;
                    }
                    if (!bestAncestorID || candidateID.canonical.z > bestAncestorID->canonical.z) {
                        bestAncestor = candidate;
                        bestAncestorID = candidateID;
                    }
                });
                // A freshly-resized ancestor's live target has no content for
                // a frame or two, but its READY predecessor is parked in the
                // retired map — use it, or children flash empty at the pan
                // leading edge (the exact artifact parent fallback exists to
                // prevent). Live candidates win ties via the strict > above.
                for (const auto& [retiredID, retiredTarget] : retiredDrapeTargetsByTile) {
                    if (!klattraDrapeTargetReadyForTile(retiredTarget, idealOS) ||
                        !klattraIsAncestorOf(retiredID, idealOS)) {
                        continue;
                    }
                    if (!bestAncestorID || retiredID.canonical.z > bestAncestorID->canonical.z) {
                        bestAncestor = retiredTarget;
                        bestAncestorID = retiredID;
                    }
                }
                if (bestAncestor && bestAncestorID) {
                    drape = bestAncestor;
                    resolvedDrapeID = *bestAncestorID;
                    binding.usedDrapeFallback = true;
                    auto [drapeTL, drapeScale] = klattraSubrectForChildInAncestor(idealOS, resolvedDrapeID);
                    binding.drapeTL = drapeTL;
                    binding.drapeScale = drapeScale;
                }
            }
        } else {
            retiredDrapeTargetsByTile.erase(idealOS);
        }
        if (klattraDrapeTargetReadyForTile(drape, idealOS) && drape->getTexture()) {
            binding.drapeReady = true;
            binding.drapeID = resolvedDrapeID;
            binding.drapeTexture = drape->getTexture();
        }
        if (binding.drapeReady) {
            readyBindings++;
        }
        if (binding.usedEmptyDEM) {
            emptyDemBindings++;
        }
        if (binding.usedDrapeFallback) {
            fallbackDrapeBindings++;
        }

        if (traceDrape) {
            Log::Info(Event::Render,
                      "[KLATTRA DRAPE_TRACE] binding frame=" + std::to_string(drapeTraceFrame) +
                          " ideal=" + klattraTileString(idealOS) +
                          " source=" + klattraTileString(binding.sourceID) +
                          " drape=" + (binding.drapeID ? klattraTileString(*binding.drapeID) : std::string("none")) +
                          " demTexture=" + klattraTexturePtrString(binding.texture) +
                          " emptyDEM=" + std::to_string(binding.usedEmptyDEM) +
                          " demTL=" + std::to_string(binding.demTL[0]) + "," + std::to_string(binding.demTL[1]) +
                          " demScale=" + std::to_string(binding.demScale) +
                          " drapeTexture=" + klattraTexturePtrString(binding.drapeTexture) +
                          " drapeFallback=" + std::to_string(binding.usedDrapeFallback) +
                          " drapeTL=" + std::to_string(binding.drapeTL[0]) + "," +
                              std::to_string(binding.drapeTL[1]) +
                          " drapeScale=" + std::to_string(binding.drapeScale) +
                          " drapeReady=" + std::to_string(binding.drapeReady) +
                          " drapePtr=" + std::to_string(reinterpret_cast<uintptr_t>(drape.get())) +
                          " drapeCompleted=" + std::to_string(drape ? drape->getCompletedRenderCount() : 0) +
                          " drapeGroups=" + std::to_string(drape ? drape->numLayerGroups() : 0) +
                          " drapeContentGroups=" + std::to_string(drape ? drape->numContentLayerGroups() : 0) +
                          " drapeDrawables=" + std::to_string(klattraDrapeDrawableCount(drape)));
        }

        nextBindings.emplace(idealOS, std::move(binding));
    }

    // Create or refresh terrain drawables, one per IDEAL tile. Recreate
    // when the source DEM tile changes (typically an upgrade from a
    // parent-fallback texture to the exact-zoom texture, or empty
    // fallback → real DEM); otherwise reuse the existing drawable.
    // Tiles that end this update WITH a drawable — the prune below holds a
    // leaving tile's drawable until the ideals covering it appear here.
    std::unordered_set<OverscaledTileID> drawableBackedTiles;
    drawableBackedTiles.reserve(nextBindings.size());
    for (const auto& [idealID, binding] : nextBindings) {
        if (auto existing = currentBindings.find(idealID); existing != currentBindings.end()) {
            if (existing->second.sourceID == binding.sourceID &&
                existing->second.texture == binding.texture &&
                existing->second.drapeTexture == binding.drapeTexture &&
                existing->second.drapeID == binding.drapeID &&
                existing->second.drapeTL == binding.drapeTL &&
                existing->second.drapeScale == binding.drapeScale &&
                existing->second.drapeReady == binding.drapeReady) {
                if (traceDrape) {
                    Log::Info(Event::Render,
                              "[KLATTRA DRAPE_TRACE] terrain-drawable-keep frame=" +
                                  std::to_string(drapeTraceFrame) +
                                  " ideal=" + klattraTileString(idealID) +
                                  " source=" + klattraTileString(binding.sourceID) +
                                  " demTexture=" + klattraTexturePtrString(binding.texture) +
                                  " drape=" +
                                      (binding.drapeID ? klattraTileString(*binding.drapeID) : std::string("none")) +
                                  " drapeTexture=" + klattraTexturePtrString(binding.drapeTexture) +
                                  " drapeFallback=" + std::to_string(binding.usedDrapeFallback) +
                                  " drapeReady=" + std::to_string(binding.drapeReady));
                }
                drawableBackedTiles.insert(idealID);
                continue; // drawable already up to date
            }
            if (traceDrape) {
                Log::Info(Event::Render,
                          "[KLATTRA DRAPE_TRACE] terrain-drawable-refresh frame=" +
                              std::to_string(drapeTraceFrame) +
                              " ideal=" + klattraTileString(idealID) +
                              " oldSource=" + klattraTileString(existing->second.sourceID) +
                              " newSource=" + klattraTileString(binding.sourceID) +
                              " oldTexture=" + klattraTexturePtrString(existing->second.texture) +
                              " newTexture=" + klattraTexturePtrString(binding.texture) +
                              " oldDrape=" +
                                  (existing->second.drapeID ? klattraTileString(*existing->second.drapeID)
                                                            : std::string("none")) +
                              " newDrape=" +
                                  (binding.drapeID ? klattraTileString(*binding.drapeID) : std::string("none")) +
                              " oldDrapeTexture=" + klattraTexturePtrString(existing->second.drapeTexture) +
                              " newDrapeTexture=" + klattraTexturePtrString(binding.drapeTexture) +
                              " oldDrapeReady=" + std::to_string(existing->second.drapeReady) +
                              " newDrapeReady=" + std::to_string(binding.drapeReady));
            }
            if (!binding.drapeReady) {
                // The refreshed binding lost its drape — typically a source or
                // ring upgrade whose new bake hasn't completed. Keep showing
                // the existing drawable (its textures stay alive through the
                // drawable's refs) instead of pruning to a hole; the refresh
                // re-runs on a later frame once the drape is ready.
                if (traceDrape) {
                    Log::Info(Event::Render,
                              "[KLATTRA DRAPE_TRACE] terrain-drawable-hold frame=" +
                                  std::to_string(drapeTraceFrame) +
                                  " ideal=" + klattraTileString(idealID) +
                                  " source=" + klattraTileString(binding.sourceID) +
                                  " reason=refresh-drape-not-ready");
                }
                drawableBackedTiles.insert(idealID);
                continue;
            }
            lg->removeDrawablesIf([&idealID](gfx::Drawable& d) {
                return d.getTileID().has_value() && *d.getTileID() == idealID;
            });
        }
        if (!binding.drapeReady) {
            klattraTrace("terrain drawable-skip ideal=" + klattraTileString(idealID) +
                         " source=" + klattraTileString(binding.sourceID) +
                         " emptyDEM=" + std::to_string(binding.usedEmptyDEM) +
                         " drapeFallback=" + std::to_string(binding.usedDrapeFallback));
            if (traceDrape) {
                Log::Info(Event::Render,
                          "[KLATTRA DRAPE_TRACE] terrain-drawable-skip frame=" +
                              std::to_string(drapeTraceFrame) +
                              " ideal=" + klattraTileString(idealID) +
                              " source=" + klattraTileString(binding.sourceID) +
                              " reason=drape-not-ready");
            }
            continue;
        }
        if (auto drawable = createDrawableForTile(context, shaders, idealID, binding)) {
            klattraTrace("terrain drawable-add ideal=" + klattraTileString(idealID) +
                         " source=" + klattraTileString(binding.sourceID) +
                         " emptyDEM=" + std::to_string(binding.usedEmptyDEM) +
                         " drapeFallback=" + std::to_string(binding.usedDrapeFallback));
            if (traceDrape) {
                Log::Info(Event::Render,
                          "[KLATTRA DRAPE_TRACE] terrain-drawable-add frame=" +
                              std::to_string(drapeTraceFrame) +
                              " ideal=" + klattraTileString(idealID) +
                              " source=" + klattraTileString(binding.sourceID) +
                              " terrainDrawablesBefore=" + std::to_string(lg->getDrawableCount()));
            }
            lg->addDrawable(std::move(drawable));
            drawableBackedTiles.insert(idealID);
        }
    }

    // Prune drawables for ideals that left the cover. The drape cache's
    // RenderTargets are pruned separately above; this only handles the
    // terrain mesh drawables themselves.
    //
    // Zoom-level transitions need care: the replacement ideals often haven't
    // had their first bake when the old level's tiles leave the cover, and
    // pruning immediately flashes a hole (black at high pitch). Hold a
    // leaving tile's drawable until every current ideal overlapping it is
    // drawable-backed — the swap then happens within one update, so old and
    // new never coexist in a rendered frame in the common single-cover case.
    // Tiles with no overlapping ideal (camera panned away) prune at once,
    // and a frame cap bounds pathological covers.
    if (!currentBindings.empty()) {
        static const uint32_t pruneHoldFrames =
            klattraEnvFrameCount("KLATTRA_DRAPE_PRUNE_HOLD_FRAMES", 30);
        std::unordered_map<OverscaledTileID, uint32_t> nextPruneHoldAges;
        std::size_t held = 0;
        const auto removed = lg->removeDrawablesIf([&](gfx::Drawable& d) {
            const auto& maybeID = d.getTileID();
            if (!maybeID.has_value()) return false;
            const auto& leavingID = *maybeID;
            if (terrainMeshIDs.find(leavingID) != terrainMeshIDs.end()) {
                return false; // still an ideal
            }
            if (pruneHoldFrames > 0) {
                bool overlapsAny = false;
                bool covered = true;
                for (const auto& idealID : terrainMeshIDs) {
                    if (!klattraIsAncestorOf(idealID, leavingID) &&
                        !klattraIsAncestorOf(leavingID, idealID)) {
                        continue;
                    }
                    overlapsAny = true;
                    if (drawableBackedTiles.find(idealID) == drawableBackedTiles.end()) {
                        covered = false;
                        break;
                    }
                }
                if (overlapsAny && !covered) {
                    const auto ageIt = pruneHoldAgeByTile.find(leavingID);
                    const uint32_t age = (ageIt == pruneHoldAgeByTile.end()) ? 1 : ageIt->second + 1;
                    if (age <= pruneHoldFrames) {
                        nextPruneHoldAges.emplace(leavingID, age);
                        held++;
                        return false; // hold: replacement not ready yet
                    }
                }
            }
            return true;
        });
        pruneHoldAgeByTile = std::move(nextPruneHoldAges);
        if (traceDrape && (removed > 0 || held > 0)) {
            Log::Info(Event::Render,
                      "[KLATTRA DRAPE_TRACE] terrain-drawable-prune frame=" +
                          std::to_string(drapeTraceFrame) +
                          " removed=" + std::to_string(removed) +
                          " held=" + std::to_string(held) +
                          " remaining=" + std::to_string(lg->getDrawableCount()));
        }
    }

    currentBindings = std::move(nextBindings);
    klattraTrace("terrain update-end source=" + impl->sourceID +
                 " bindings=" + std::to_string(currentBindings.size()) +
                 " ready=" + std::to_string(readyBindings) +
                 " emptyDEM=" + std::to_string(emptyDemBindings) +
                 " drapeFallback=" + std::to_string(fallbackDrapeBindings) +
                 " demTextures=" + std::to_string(demTexturesByTile.size()) +
                 " drapeTargets=" + std::to_string(drapeCache.size()) +
                 " terrainDrawables=" + std::to_string(lg->getDrawableCount()));
    if (traceDrape) {
        Log::Info(Event::Render,
                  "[KLATTRA DRAPE_TRACE] update-end frame=" + std::to_string(drapeTraceFrame) +
                      " bindings=" + std::to_string(currentBindings.size()) +
                      " demTextures=" + std::to_string(demTexturesByTile.size()) +
                      " drapeTargets=" + std::to_string(drapeCache.size()) +
                      " terrainDrawables=" + std::to_string(lg->getDrawableCount()));
    }
}

float RenderTerrain::getElevation(const UnwrappedTileID& tileID, float x, float y) const {
    // x, y are normalised within the tile in [0, 1] — bilinearly samples the
    // cached DEM image's Mapbox-RGB encoded pixels. Returns 0 if the tile
    // isn't in the current cover set or the encoding can't be decoded yet.
    std::shared_ptr<const PremultipliedImage> image;
    for (const auto& [cachedID, cachedImage] : demImagesByTile) {
        if (cachedID.toUnwrapped() == tileID) {
            image = cachedImage;
            break;
        }
    }
    if (!image || image->size.isEmpty()) {
        return 0.0f;
    }

    const auto w = static_cast<int32_t>(image->size.width);
    const auto h = static_cast<int32_t>(image->size.height);
    const auto* px = image->data.get();
    if (!px) return 0.0f;

    const float pX = std::max(0.0f, std::min(1.0f, x)) * static_cast<float>(w - 1);
    const float pY = std::max(0.0f, std::min(1.0f, y)) * static_cast<float>(h - 1);
    const int32_t x0 = static_cast<int32_t>(std::floor(pX));
    const int32_t y0 = static_cast<int32_t>(std::floor(pY));
    const int32_t x1 = std::min(x0 + 1, w - 1);
    const int32_t y1 = std::min(y0 + 1, h - 1);
    const float fx = pX - static_cast<float>(x0);
    const float fy = pY - static_cast<float>(y0);

    const auto sample = [&](int32_t xi, int32_t yi) -> float {
        const size_t offset = (static_cast<size_t>(yi) * w + xi) * 4;
        const float r = px[offset + 0];
        const float g = px[offset + 1];
        const float b = px[offset + 2];
        // Mapbox Terrain RGB: height = -10000 + ((R*256*256 + G*256 + B) * 0.1)
        return -10000.0f + (r * 256.0f * 256.0f + g * 256.0f + b) * 0.1f;
    };

    const float e00 = sample(x0, y0);
    const float e10 = sample(x1, y0);
    const float e01 = sample(x0, y1);
    const float e11 = sample(x1, y1);
    const float e0 = e00 + (e10 - e00) * fx;
    const float e1 = e01 + (e11 - e01) * fx;
    return e0 + (e1 - e0) * fy;
}

float RenderTerrain::getElevationWithExaggeration(const UnwrappedTileID& tileID, float x, float y) const {
    return getElevation(tileID, x, y) * getExaggeration();
}

std::optional<float> RenderTerrain::getElevationAtLatLng(const LatLng& latLng) const {
    if (demImagesByTile.empty()) {
        return std::nullopt;
    }

    std::optional<UnwrappedTileID> bestTile;
    float bestX = 0.5f;
    float bestY = 0.5f;
    uint8_t bestZ = 0;

    for (const auto& [cachedID, cachedImage] : demImagesByTile) {
        if (!cachedImage || cachedImage->size.isEmpty()) {
            continue;
        }

        const auto z = cachedID.canonical.z;
        const auto projected = Projection::project(latLng, static_cast<int32_t>(z));
        const auto unwrapped = cachedID.toUnwrapped();
        const auto scale = int64_t{1} << z;
        const double tileX = static_cast<double>(unwrapped.canonical.x) +
                             static_cast<double>(unwrapped.wrap) * static_cast<double>(scale);
        const double tileY = static_cast<double>(unwrapped.canonical.y);
        const double localX = projected.x - tileX;
        const double localY = projected.y - tileY;
        if (localX < 0.0 || localX > 1.0 || localY < 0.0 || localY > 1.0) {
            continue;
        }
        if (!bestTile || z >= bestZ) {
            bestTile = unwrapped;
            bestX = static_cast<float>(localX);
            bestY = static_cast<float>(localY);
            bestZ = z;
        }
    }

    if (!bestTile) {
        return std::nullopt;
    }

    const float elevation = getElevation(*bestTile, bestX, bestY);
    if (elevation <= -500.0f || elevation >= 9000.0f) {
        return std::nullopt;
    }
    return elevation;
}

float RenderTerrain::getExaggeration() const {
    return impl->exaggeration;
}

const std::string& RenderTerrain::getSourceID() const {
    return impl->sourceID;
}

bool RenderTerrain::isEnabled() const {
    return !impl->sourceID.empty();
}

const RenderTerrain::TerrainMesh& RenderTerrain::getMesh(gfx::Context& context) {
    if (!mesh) {
        generateMesh(context);
    }
    return *mesh;
}

void RenderTerrain::generateMesh(gfx::Context& context) {
    (void)context;
    // Generate a regular grid mesh for terrain in tile-extent coordinates
    // (0..8192). Each vertex stores [pos.x, pos.y, tex.u, tex.v]; the vertex
    // shader samples the DEM at pos / EXTENT and displaces the grid in metres.
    //
    // Earlier builds added 5 km vertical skirts at every tile edge. At the
    // close, high-pitch drone camera used by trail preview, those curtains
    // routinely crossed the camera frustum and hid the real surface. Terrain
    // now relies on a padded cover plus drape fallback instead of skirt walls.

    const size_t gridSize = MESH_SIZE;
    const size_t verticesPerSide = gridSize + 1;
    const size_t totalMainVertices = verticesPerSide * verticesPerSide;

    std::vector<int16_t> vertices;
    vertices.reserve(totalMainVertices * 4);

    const float posStep = static_cast<float>(util::EXTENT) / static_cast<float>(gridSize);
    const float texStep = static_cast<float>(util::EXTENT) / static_cast<float>(gridSize);

    for (size_t y = 0; y < verticesPerSide; ++y) {
        for (size_t x = 0; x < verticesPerSide; ++x) {
            vertices.push_back(static_cast<int16_t>(x * posStep));
            vertices.push_back(static_cast<int16_t>(y * posStep));
            vertices.push_back(static_cast<int16_t>(x * texStep));
            vertices.push_back(static_cast<int16_t>(y * texStep));
        }
    }

    std::vector<uint16_t> indices;
    indices.reserve(gridSize * gridSize * 6);

    for (size_t y = 0; y < gridSize; ++y) {
        for (size_t x = 0; x < gridSize; ++x) {
            const uint16_t topLeft = static_cast<uint16_t>(y * verticesPerSide + x);
            const uint16_t topRight = topLeft + 1;
            const uint16_t bottomLeft = static_cast<uint16_t>((y + 1) * verticesPerSide + x);
            const uint16_t bottomRight = bottomLeft + 1;
            indices.push_back(topLeft);
            indices.push_back(bottomLeft);
            indices.push_back(topRight);
            indices.push_back(topRight);
            indices.push_back(bottomLeft);
            indices.push_back(bottomRight);
        }
    }

    mesh = TerrainMesh{
        nullptr, // vertexBuffer
        nullptr, // indexBuffer
        vertices.size() / 4,
        indices.size(),
        std::move(vertices),
        std::move(indices),
        nullptr, // sharedLayoutVertices, built below
        nullptr  // sharedIndexes, built below
    };

    // Build the shared per-drawable inputs once. Every tile drawable hands
    // these same vectors to its builder; the vectors carry their GPU buffer,
    // so all terrain tiles share one vertex and one index buffer.
    auto layoutVertices = std::make_shared<gfx::VertexVector<TerrainLayoutVertex>>();
    layoutVertices->reserve(mesh->vertexCount);
    const auto& rawVertices = mesh->vertices;
    for (size_t index = 0; index + 3 < rawVertices.size(); index += 4) {
        layoutVertices->emplace_back(TerrainLayoutVertex{
            {rawVertices[index + 0], rawVertices[index + 1]},
            {rawVertices[index + 2], rawVertices[index + 3]},
        });
    }
    mesh->sharedLayoutVertices = std::move(layoutVertices);

    auto sharedIndexes = std::make_shared<gfx::IndexVector<gfx::Triangles>>();
    sharedIndexes->reserve(mesh->indexCount);
    const auto& rawIndices = mesh->indices;
    for (size_t index = 0; index + 2 < rawIndices.size(); index += 3) {
        sharedIndexes->emplace_back(rawIndices[index], rawIndices[index + 1], rawIndices[index + 2]);
    }
    mesh->sharedIndexes = std::move(sharedIndexes);
}

std::shared_ptr<gfx::Texture2D> RenderTerrain::getOrCreateEmptyDEMTexture(gfx::Context& context) {
    if (emptyDEMTexture) {
        return emptyDEMTexture;
    }

    // 1×1 RGBA pixel encoding Mapbox Terrain-RGB elevation 0 m:
    //   height = -10000 + (R*65536 + G*256 + B) * 0.1
    // To produce height = 0: (R*65536 + G*256 + B) * 0.1 = 10000  →
    //   R*65536 + G*256 + B = 100000 = 0x0186A0.
    // So (R, G, B) = (0x01, 0x86, 0xA0) = (1, 134, 160). Alpha is unused
    // by the terrain vertex shader's Mapbox-RGB decode.
    auto texture = context.createTexture2D();
    if (!texture) {
        Log::Error(Event::Render, "Failed to create empty DEM texture");
        return nullptr;
    }

    std::array<uint8_t, 4> pixel{{1, 134, 160, 255}};
    auto image = std::make_shared<PremultipliedImage>(Size(1, 1), pixel.data(), pixel.size());
    texture->setImage(image);
    texture->setSamplerConfiguration({.filter = gfx::TextureFilterType::Linear,
                                      .wrapU = gfx::TextureWrapType::Clamp,
                                      .wrapV = gfx::TextureWrapType::Clamp});

    emptyDEMTexture = std::move(texture);
    return emptyDEMTexture;
}

std::shared_ptr<gfx::Texture2D> RenderTerrain::createDEMTexture(gfx::Context& context, const DEMData& demData) {
    auto imagePtr = demData.getImagePtr();
    if (!imagePtr || imagePtr->size.isEmpty()) {
        return nullptr;
    }

    auto texture = context.createTexture2D();
    if (!texture) {
        Log::Error(Event::Render, "Failed to create DEM texture");
        return nullptr;
    }

    texture->setImage(imagePtr);

    // Linear filtering for smooth elevation interpolation between texels.
    texture->setSamplerConfiguration({
        .filter = gfx::TextureFilterType::Linear,
        .wrapU = gfx::TextureWrapType::Clamp,
        .wrapV = gfx::TextureWrapType::Clamp
    });

    return texture;
}

std::unique_ptr<gfx::Drawable> RenderTerrain::createDrawableForTile(gfx::Context& context,
                                                                      gfx::ShaderRegistry& shaders,
                                                                      const OverscaledTileID& idealID,
                                                                      const DEMBinding& binding) {
    auto demTexture = binding.texture;
    // Ensure mesh is generated
    const auto& terrainMesh = getMesh(context);

    if (terrainMesh.vertices.empty() || terrainMesh.indices.empty()) {
        Log::Error(Event::Render, "Terrain mesh is empty, cannot create drawable");
        return nullptr;
    }

    // Get terrain shader
    auto terrainShader = context.getGenericShader(shaders, "TerrainShader");
    if (!terrainShader) {
        Log::Error(Event::Render, "Terrain shader not found");
        return nullptr;
    }

    // Create drawable builder
    auto builder = context.createDrawableBuilder("terrain-tile");
    if (!builder) {
        Log::Error(Event::Render, "Failed to create drawable builder for terrain tile");
        return nullptr;
    }

    // Configure builder — terrain is an opaque 3D mesh that occludes itself.
    // The Phase-4 matrix Z-scale fix puts elevation into clip-space depth at
    // the same scale as X/Y world-pixels, so the depth buffer can resolve
    // mountains-in-front vs mountains-behind correctly. setIs3D bypasses the
    // 2D sublayer depth-offset hack that LayerTweaker applies for stacked 2D
    // layers — we want the actual perspective depth.
    builder->setShader(terrainShader);
    builder->setRenderPass(RenderPass::Translucent);
    builder->setDepthType(gfx::DepthMaskType::ReadWrite);
    builder->setColorMode(gfx::ColorMode::unblended());
    builder->setEnableDepth(true);
    builder->setIs3D(true);

    // The mesh is identical for every tile: hand the builder the shared
    // vectors built in generateMesh() so all terrain drawables reference one
    // vertex and one index GPU buffer instead of uploading fresh copies.
    const auto& sharedVertices = terrainMesh.sharedLayoutVertices;
    if (!sharedVertices || !terrainMesh.sharedIndexes) {
        Log::Error(Event::Render, "Terrain mesh shared buffers missing, cannot create drawable");
        return nullptr;
    }

    auto vertexAttributes = context.createVertexAttributeArray();
    if (const auto& attr = vertexAttributes->set(shaders::idTerrainPosVertexAttribute)) {
        attr->setSharedRawData(sharedVertices,
                               offsetof(TerrainLayoutVertex, pos),
                               /*vertexOffset=*/0,
                               sizeof(TerrainLayoutVertex),
                               gfx::AttributeDataType::Short2);
    }
    if (const auto& attr = vertexAttributes->set(shaders::idTerrainTexturePosVertexAttribute)) {
        attr->setSharedRawData(sharedVertices,
                               offsetof(TerrainLayoutVertex, texturePos),
                               /*vertexOffset=*/0,
                               sizeof(TerrainLayoutVertex),
                               gfx::AttributeDataType::Short2);
    }
    builder->setVertexAttributes(std::move(vertexAttributes));
    builder->setRawVertices({}, terrainMesh.vertexCount, gfx::AttributeDataType::Short4);

    // Set index data and segments
    // Create a single segment covering the entire terrain mesh
    SegmentVector segments;
    segments.emplace_back(0, // vertex offset
                          0, // index offset
                          terrainMesh.vertexCount, // vertex count
                          terrainMesh.indexCount); // index count

    builder->setSegments(gfx::Triangles(), terrainMesh.sharedIndexes, segments.data(), segments.size());

    if (demTexture) {
        builder->setTexture(demTexture, 0); // slot 0 = demTexture
    }

    // Bind the resolved drape texture as the surface colour input. Usually
    // this is the ideal tile's own target, but during zoom-in it can be a
    // ready ancestor target with binding.drapeTL / binding.drapeScale
    // remapping the child's UVs into the parent sub-rect.
    if (binding.drapeReady && binding.drapeTexture) {
        if (klattraLogDrapeTrace()) {
            auto drape = binding.drapeID ? drapeCache.get(*binding.drapeID) : nullptr;
            Log::Info(Event::Render,
                      "[KLATTRA DRAPE_TRACE] create-terrain-drawable-bind ideal=" +
                          klattraTileString(idealID) +
                          " source=" + klattraTileString(binding.sourceID) +
                          " drape=" +
                              (binding.drapeID ? klattraTileString(*binding.drapeID) : std::string("none")) +
                          " demTexture=" + klattraTexturePtrString(demTexture) +
                          " emptyDEM=" + std::to_string(binding.usedEmptyDEM) +
                          " drapeTexture=" + klattraTexturePtrString(binding.drapeTexture) +
                          " drapeFallback=" + std::to_string(binding.usedDrapeFallback) +
                          " drapeTL=" + std::to_string(binding.drapeTL[0]) + "," +
                              std::to_string(binding.drapeTL[1]) +
                          " drapeScale=" + std::to_string(binding.drapeScale) +
                          " drapeTarget=" + (drape ? drape->getDebugName() : std::string("none")) +
                          " drapePtr=" + std::to_string(reinterpret_cast<uintptr_t>(drape.get())) +
                          " drapeCompleted=" + std::to_string(drape ? drape->getCompletedRenderCount() : 0) +
                          " drapeGroups=" + std::to_string(drape ? drape->numLayerGroups() : 0) +
                          " drapeContentGroups=" + std::to_string(drape ? drape->numContentLayerGroups() : 0) +
                          " drapeDrawables=" + std::to_string(klattraDrapeDrawableCount(drape)));
        }
        builder->setTexture(binding.drapeTexture, 1); // slot 1 = mapTexture
    } else {
        Log::Warning(Event::Render,
                     "Drape target missing or not ready for ideal tile " + util::toString(idealID));
        return nullptr;
    }

    // Flush to create the drawable
    builder->flush(context);

    // Get the drawable
    auto drawables = builder->clearDrawables();
    if (drawables.empty()) {
        Log::Error(Event::Render, "Failed to create terrain drawable for tile");
        return nullptr;
    }

    if (klattraLogDrapeTrace()) {
        Log::Info(Event::Render,
                  "[KLATTRA DRAPE_TRACE] create-terrain-drawable-complete ideal=" +
                      klattraTileString(idealID) +
                      " source=" + klattraTileString(binding.sourceID) +
                      " drawableCount=" + std::to_string(drawables.size()));
    }

    // Set tile ID on the drawable — this is the IDEAL tile, used by the
    // tweaker to compute the world-to-tile matrix and to look up the
    // per-drawable DEM binding (texture + UV remap).
    auto& drawable = drawables[0];
    drawable->setTileID(idealID);

    return std::move(drawable);
}

RenderSource* RenderTerrain::findDEMSource(const UpdateParameters& /*parameters*/) {
    // TODO: Implement source lookup
    // This would iterate through parameters.sources to find the raster-dem source
    // matching impl->sourceID
    return nullptr;
}

void RenderTerrain::activateLayerGroup(bool activate, UniqueChangeRequestVec& changes) {
    if (layerGroup) {
        if (activate) {
            changes.emplace_back(std::make_unique<AddLayerGroupRequest>(layerGroup));
        } else {
            changes.emplace_back(std::make_unique<RemoveLayerGroupRequest>(layerGroup));
        }
    }
}

void RenderTerrain::teardown(UniqueChangeRequestVec& changes) {
    // Drop drawables + every drape render target (clearRenderState emits the
    // RemoveRenderTargetRequests), then deregister the terrain layer group.
    // retiredDrapeTargetsByTile needs no requests here: retired targets had
    // their RemoveRenderTargetRequest emitted when they were retired; the
    // map only keeps the texture alive for sampling and is cleared inside
    // clearRenderState.
    clearRenderState(changes);
    activateLayerGroup(false, changes);
    layerGroup.reset();
    demSource = nullptr;
}

void RenderTerrain::reduceMemoryUse(UniqueChangeRequestVec& changes) {
    // Memory-pressure response (MLNMapView didReceiveMemoryWarning →
    // Renderer::reduceMemoryUse → orchestrator): drop everything not strictly
    // required for the current cover — parked resize predecessors and
    // out-of-cover ancestor fallbacks. Content re-bakes on demand; a brief
    // quality dip beats a jetsam kill. drapeRingByTile's keys are exactly the
    // last update's cover set (pruned to it every frame), which update()
    // keeps only as a local.
    retiredDrapeTargetsByTile.clear();
    auto evicted = drapeCache.pruneIf(
        [&](const OverscaledTileID& id) { return drapeRingByTile.find(id) == drapeRingByTile.end(); });
    for (auto& [tileID, target] : evicted) {
        (void)tileID;
        changes.emplace_back(std::make_unique<RemoveRenderTargetRequest>(std::move(target)));
    }
}

void RenderTerrain::clearRenderState(UniqueChangeRequestVec& changes) {
    if (auto* lg = static_cast<LayerGroup*>(layerGroup.get())) {
        lg->removeDrawablesIf([](gfx::Drawable&) { return true; });
    }

    currentBindings.clear();
    demImagesByTile.clear();
    demTexturesByTile.clear();
    previousIdealIDs.clear();
    stableDrapeCoverFrames = 0;
    retiredDrapeTargetsByTile.clear();
    // Ring hysteresis must not survive a full eviction: stale "was near-ring"
    // entries make re-covered tiles allocate 2048² targets they no longer rank
    // for (~ringSlack × 22 MB of overshoot after a style swap / DEM loss).
    drapeRingByTile.clear();
    pruneHoldAgeByTile.clear();

    auto evicted = drapeCache.pruneIf([](const OverscaledTileID&) { return true; });
    for (auto& [tileID, target] : evicted) {
        (void)tileID;
        changes.emplace_back(std::make_unique<RemoveRenderTargetRequest>(std::move(target)));
    }
}

} // namespace mbgl
