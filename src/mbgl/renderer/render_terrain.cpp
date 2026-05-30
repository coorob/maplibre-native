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
#include <mbgl/shaders/shader_source.hpp>
#include <mbgl/shaders/terrain_layer_ubo.hpp>
#include <mbgl/shaders/shader_defines.hpp>
#include <mbgl/shaders/segment.hpp>
#include <mbgl/util/constants.hpp>
#include <mbgl/util/logging.hpp>
#include <mbgl/util/image.hpp>
#include <mbgl/util/mat4.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <limits>
#include <unordered_set>

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

std::string klattraTexturePtrString(const std::shared_ptr<gfx::Texture2D>& texture) {
    return texture ? std::to_string(reinterpret_cast<uintptr_t>(texture.get())) : "0";
}

std::size_t klattraDrapeDrawableCount(const TerrainDrapeTargetPtr& target) {
    if (!target) return 0;
    std::size_t count = 0;
    target->visitLayerGroups([&](LayerGroupBase& layerGroup) { count += layerGroup.getDrawableCount(); });
    return count;
}

bool klattraDrapeTargetReady(const TerrainDrapeTargetPtr& target) {
    return target && target->hasCompletedRender() && target->numDrawables() > 0;
}

int32_t klattraEnvTargetSize(const char* name, int32_t fallback) {
    const char* value = std::getenv(name);
    if (!value) return fallback;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value) return fallback;
    return static_cast<int32_t>(std::clamp<long>(parsed, 256, 4096));
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
                           const TransformState& /*state*/,
                           const std::shared_ptr<UpdateParameters>& /*updateParameters*/,
                           const RenderTree& renderTree,
                           UniqueChangeRequestVec& changes) {
    // Find the DEM source if we haven't already
    if (!demSource && !impl->sourceID.empty()) {
        demSource = orchestrator.getRenderSource(impl->sourceID);
        if (!demSource) {
            Log::Warning(Event::Render, "Terrain could not find DEM source: " + impl->sourceID);
        }
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
    //  - `currentSourceIDs`: the actual DEM tile's OverscaledTileID per
    //    cover slot, deduped. Used as the DEM texture/image cache key.
    //
    // Drape targets are keyed by ideal tile, not by DEM source tile. DEM
    // parent fallback is fine for elevation, but visible topo colour must
    // stay at the tile LOD the camera is actually drawing; otherwise a coarse
    // source parent can stretch low-zoom drape pixels across a close view.
    std::unordered_set<OverscaledTileID> currentIdealIDs;
    std::unordered_set<OverscaledTileID> currentSourceIDs;
    currentIdealIDs.reserve(renderTiles->size());
    currentSourceIDs.reserve(renderTiles->size());
    for (const auto& renderTile : *renderTiles) {
        currentIdealIDs.emplace(renderTile.id.canonical.z, renderTile.id.wrap, renderTile.id.canonical);
        currentSourceIDs.insert(renderTile.getOverscaledTileID());
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
        const auto drapeTargetSizeForTile = [](const OverscaledTileID& tileID) -> int32_t {
            // Low-zoom DEM padding can legitimately cover many more terrain
            // tiles at pitched camera angles. Their on-screen texel density
            // does not justify the full close-zoom 2048px drape budget.
            static const int32_t z8Size = klattraEnvTargetSize("KLATTRA_DRAPE_TARGET_SIZE_Z8", 512);
            static const int32_t z10Size = klattraEnvTargetSize("KLATTRA_DRAPE_TARGET_SIZE_Z10", 1024);
            if (tileID.canonical.z <= 8) return z8Size;
            if (tileID.canonical.z <= 10) return z10Size;
            return DRAPE_TARGET_SIZE;
        };

        for (const auto& tileID : currentIdealIDs) {
            const bool wasAllocated = drapeCache.get(tileID) != nullptr;
            const int32_t targetSize = drapeTargetSizeForTile(tileID);
            auto target = drapeCache.getOrCreate(context, tileID, {targetSize, targetSize});
            if (!wasAllocated && target) {
                changes.emplace_back(std::make_unique<AddRenderTargetRequest>(target));
            }
        }
        auto evicted = drapeCache.pruneIf(
            [&](const OverscaledTileID& id) { return currentIdealIDs.find(id) == currentIdealIDs.end(); });
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
    // exact-zoom data is still in flight — to whichever ancestor's
    // texture happens to be available, with a sub-rect UV remap. This
    // is the GL-JS-style parent-fallback DEM sampling: the terrain mesh
    // never has to render flat or with a sea-level empty texture while
    // tiles stream in; it keeps a coarse-but-correct shape and silently
    // sharpens once the exact-zoom DEM arrives.
    std::unordered_map<OverscaledTileID, DEMBinding> nextBindings;
    nextBindings.reserve(renderTiles->size());
    for (const auto& renderTile : *renderTiles) {
        const auto& tile = renderTile.getTile();
        if (tile.kind != Tile::Kind::RasterDEM) continue;

        const auto& ideal = renderTile.id; // UnwrappedTileID
        const OverscaledTileID idealOS(ideal.canonical.z, ideal.wrap, ideal.canonical);
        const auto& sourceID = tile.id;

        DEMBinding binding{/*texture=*/nullptr, /*sourceID=*/sourceID};

        if (auto texIt = demTexturesByTile.find(sourceID); texIt != demTexturesByTile.end()) {
            binding.texture = texIt->second;
            // If the source tile is a strict ancestor of the ideal,
            // compute the ideal's sub-rect inside the source's UV space.
            if (ideal.canonical.z > sourceID.canonical.z) {
                const uint8_t dz = static_cast<uint8_t>(ideal.canonical.z - sourceID.canonical.z);
                const uint32_t mask = (1u << dz) - 1u;
                const uint32_t sub_x = ideal.canonical.x & mask;
                const uint32_t sub_y = ideal.canonical.y & mask;
                const float scale = 1.0f / static_cast<float>(1u << dz);
                binding.demTL = {{static_cast<float>(sub_x) * scale, static_cast<float>(sub_y) * scale}};
                binding.demScale = scale;
            }
            // Else: ideal == source (or, defensively, ideal is an ancestor
            // of source — shouldn't happen since updateRenderables walks
            // UP from ideal looking for parents). Identity remap.
        } else {
            // No DEM data anywhere in the parent chain for this ideal —
            // bind the 1×1 empty-elevation texture so the mesh renders
            // flat for one frame rather than vanishing or showing
            // basemap bleed-through. Once an ancestor's DEM lands in
            // `demTexturesByTile`, the next frame upgrades to a real
            // texture and the binding state-transition re-creates the
            // drawable.
            binding.texture = getOrCreateEmptyDEMTexture(context);
            binding.sourceID = idealOS;
            binding.usedEmptyDEM = true;
        }

        auto drape = drapeCache.get(idealOS);
        if (drape) {
            binding.drapeReady = klattraDrapeTargetReady(drape);
        }

        if (traceDrape) {
            Log::Info(Event::Render,
                      "[KLATTRA DRAPE_TRACE] binding frame=" + std::to_string(drapeTraceFrame) +
                          " ideal=" + klattraTileString(idealOS) +
                          " source=" + klattraTileString(binding.sourceID) +
                          " drape=" + klattraTileString(idealOS) +
                          " demTexture=" + klattraTexturePtrString(binding.texture) +
                          " emptyDEM=" + std::to_string(binding.usedEmptyDEM) +
                          " demTL=" + std::to_string(binding.demTL[0]) + "," + std::to_string(binding.demTL[1]) +
                          " demScale=" + std::to_string(binding.demScale) +
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
    for (const auto& [idealID, binding] : nextBindings) {
        if (auto existing = currentBindings.find(idealID); existing != currentBindings.end()) {
            if (existing->second.sourceID == binding.sourceID &&
                existing->second.texture == binding.texture &&
                existing->second.drapeReady == binding.drapeReady) {
                if (traceDrape) {
                    Log::Info(Event::Render,
                              "[KLATTRA DRAPE_TRACE] terrain-drawable-keep frame=" +
                                  std::to_string(drapeTraceFrame) +
                                  " ideal=" + klattraTileString(idealID) +
                                  " source=" + klattraTileString(binding.sourceID) +
                                  " demTexture=" + klattraTexturePtrString(binding.texture) +
                                  " drapeReady=" + std::to_string(binding.drapeReady));
                }
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
                              " oldDrapeReady=" + std::to_string(existing->second.drapeReady) +
                              " newDrapeReady=" + std::to_string(binding.drapeReady));
            }
            lg->removeDrawablesIf([&idealID](gfx::Drawable& d) {
                return d.getTileID().has_value() && *d.getTileID() == idealID;
            });
        }
        if (!binding.drapeReady) {
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
            if (traceDrape) {
                Log::Info(Event::Render,
                          "[KLATTRA DRAPE_TRACE] terrain-drawable-add frame=" +
                              std::to_string(drapeTraceFrame) +
                              " ideal=" + klattraTileString(idealID) +
                              " source=" + klattraTileString(binding.sourceID) +
                              " terrainDrawablesBefore=" + std::to_string(lg->getDrawableCount()));
            }
            lg->addDrawable(std::move(drawable));
        }
    }

    // Prune drawables for ideals that left the cover. The drape cache's
    // RenderTargets are pruned separately above; this only handles the
    // terrain mesh drawables themselves.
    if (!currentBindings.empty()) {
        const auto removed = lg->removeDrawablesIf([&currentIdealIDs](gfx::Drawable& d) {
            const auto& maybeID = d.getTileID();
            return maybeID.has_value() && currentIdealIDs.find(*maybeID) == currentIdealIDs.end();
        });
        if (traceDrape && removed > 0) {
            Log::Info(Event::Render,
                      "[KLATTRA DRAPE_TRACE] terrain-drawable-prune frame=" +
                          std::to_string(drapeTraceFrame) +
                          " removed=" + std::to_string(removed) +
                          " remaining=" + std::to_string(lg->getDrawableCount()));
        }
    }

    currentBindings = std::move(nextBindings);
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
    // Generate a regular grid mesh for terrain, plus a perimeter skirt to
    // hide basemap bleed-through at tile edges and same-level seams.
    //
    // The base mesh is the standard (MESH_SIZE+1)² grid of vertices in
    // tile-extent coordinates (0..8192). Each vertex stores [pos.x,
    // pos.y, tex.u, tex.v] — the vertex shader samples the DEM at
    // `pos / EXTENT` (texture_pos is currently informational only).
    //
    // The skirt adds four strips of vertices along the tile perimeter
    // — one per edge, each (MESH_SIZE+1) vertices long. Each skirt
    // vertex has the SAME pos as its mesh-edge counterpart but
    // `texture_pos = (-1, -1)` as a sentinel: the shader detects the
    // negative tex coord and drops the vertex's world elevation by
    // `SKIRT_DROP_METERS`. The result is a vertical wall hanging
    // straight down from each tile edge, sampling the same drape
    // texture as the edge so the wall visually continues the basemap
    // content underneath the mesh. Mirrors `_buildSkirts` in
    // maplibre-gl-js's `src/render/terrain.ts` (which uses a 3rd
    // position component as the flag — we repurpose the texture_pos
    // sentinel to avoid widening the vertex format).
    //
    // Why hanging-down skirts work: when the camera looks past a tile
    // edge, the basemap (rendered at z=0 in the main pass) would
    // otherwise show through. The skirt's bottom sits at
    // `(mesh_edge_elevation − SKIRT_DROP_METERS)` — chosen to be well
    // below z=0 in worst-case high-mountain coverage — so depth-test
    // resolves the skirt in front of the basemap at the screen pixels
    // adjacent to the edge. Same trick used by Cesium quantized-mesh
    // (per-tile `westSkirtHeight` etc.) and discussed at length in the
    // game-dev literature on LOD-crack mitigation.

    const size_t gridSize = MESH_SIZE;
    const size_t verticesPerSide = gridSize + 1;
    const size_t totalMainVertices = verticesPerSide * verticesPerSide;
    const size_t totalSkirtVertices = 4 * verticesPerSide;

    std::vector<int16_t> vertices;
    vertices.reserve((totalMainVertices + totalSkirtVertices) * 4);

    const float posStep = static_cast<float>(util::EXTENT) / static_cast<float>(gridSize);
    const float texStep = static_cast<float>(util::EXTENT) / static_cast<float>(gridSize);
    const int16_t extentI16 = static_cast<int16_t>(util::EXTENT);

    // Main grid vertices.
    for (size_t y = 0; y < verticesPerSide; ++y) {
        for (size_t x = 0; x < verticesPerSide; ++x) {
            vertices.push_back(static_cast<int16_t>(x * posStep));
            vertices.push_back(static_cast<int16_t>(y * posStep));
            vertices.push_back(static_cast<int16_t>(x * texStep));
            vertices.push_back(static_cast<int16_t>(y * texStep));
        }
    }

    // Skirt vertices: 4 perimeter strips. Each strip has `verticesPerSide`
    // entries with `pos` = the corresponding mesh edge vertex's position
    // and `texture_pos = (-1, -1)` as the shader sentinel. The shader
    // applies SKIRT_DROP_METERS to skirt vertices' elevation. The skirt
    // hangs straight down from each tile edge — outward-leaning skirts
    // (Cesium-style) were tried but caused visible sloped "fins" at the
    // outer cover boundary; the camera could see the slope from angles
    // where the rest of the mesh occluded the tile interior. Vertical
    // skirts hide behind the mesh from any typical viewing angle.
    auto pushSkirtVertex = [&](int16_t px, int16_t py) {
        vertices.push_back(px);
        vertices.push_back(py);
        vertices.push_back(-1);
        vertices.push_back(-1);
    };
    // Top edge (y=0)
    for (size_t x = 0; x < verticesPerSide; ++x) {
        pushSkirtVertex(static_cast<int16_t>(x * posStep), 0);
    }
    // Bottom edge (y=EXTENT)
    for (size_t x = 0; x < verticesPerSide; ++x) {
        pushSkirtVertex(static_cast<int16_t>(x * posStep), extentI16);
    }
    // Left edge (x=0)
    for (size_t y = 0; y < verticesPerSide; ++y) {
        pushSkirtVertex(0, static_cast<int16_t>(y * posStep));
    }
    // Right edge (x=EXTENT)
    for (size_t y = 0; y < verticesPerSide; ++y) {
        pushSkirtVertex(extentI16, static_cast<int16_t>(y * posStep));
    }

    const size_t skirtTopBase = totalMainVertices;
    const size_t skirtBottomBase = skirtTopBase + verticesPerSide;
    const size_t skirtLeftBase = skirtBottomBase + verticesPerSide;
    const size_t skirtRightBase = skirtLeftBase + verticesPerSide;

    // Index data.
    std::vector<uint16_t> indices;
    indices.reserve((gridSize * gridSize + 4 * gridSize) * 6);

    // Main grid triangles (2 per cell).
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

    // Skirt triangles: each strip connects mesh-edge vertices to the
    // matching skirt vertices, two triangles per cell.
    // Winding matches the main grid (CW in tile-space y-down). Back-face
    // culling is disabled for the terrain pass, so winding only affects
    // depth-test consistency at near-coincident edges.
    const auto addSkirtStrip = [&](size_t meshA, size_t meshB, size_t skirtA, size_t skirtB) {
        indices.push_back(static_cast<uint16_t>(meshA));
        indices.push_back(static_cast<uint16_t>(skirtA));
        indices.push_back(static_cast<uint16_t>(meshB));
        indices.push_back(static_cast<uint16_t>(meshB));
        indices.push_back(static_cast<uint16_t>(skirtA));
        indices.push_back(static_cast<uint16_t>(skirtB));
    };
    // Top edge: mesh row y=0 ↔ skirt-top strip.
    for (size_t x = 0; x < gridSize; ++x) {
        addSkirtStrip(/*meshA=*/x,
                      /*meshB=*/x + 1,
                      /*skirtA=*/skirtTopBase + x,
                      /*skirtB=*/skirtTopBase + x + 1);
    }
    // Bottom edge: mesh row y=gridSize ↔ skirt-bottom strip.
    for (size_t x = 0; x < gridSize; ++x) {
        addSkirtStrip(/*meshA=*/gridSize * verticesPerSide + x + 1,
                      /*meshB=*/gridSize * verticesPerSide + x,
                      /*skirtA=*/skirtBottomBase + x + 1,
                      /*skirtB=*/skirtBottomBase + x);
    }
    // Left edge: mesh column x=0 ↔ skirt-left strip.
    for (size_t y = 0; y < gridSize; ++y) {
        addSkirtStrip(/*meshA=*/(y + 1) * verticesPerSide,
                      /*meshB=*/y * verticesPerSide,
                      /*skirtA=*/skirtLeftBase + y + 1,
                      /*skirtB=*/skirtLeftBase + y);
    }
    // Right edge: mesh column x=gridSize ↔ skirt-right strip.
    for (size_t y = 0; y < gridSize; ++y) {
        addSkirtStrip(/*meshA=*/y * verticesPerSide + gridSize,
                      /*meshB=*/(y + 1) * verticesPerSide + gridSize,
                      /*skirtA=*/skirtRightBase + y,
                      /*skirtB=*/skirtRightBase + y + 1);
    }

    mesh = TerrainMesh{
        nullptr, // vertexBuffer
        nullptr, // indexBuffer
        vertices.size() / 4,
        indices.size(),
        std::move(vertices),
        std::move(indices)
    };
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
    builder->setRenderPass(RenderPass::Opaque);
    builder->setDepthType(gfx::DepthMaskType::ReadWrite);
    builder->setColorMode(gfx::ColorMode::unblended());
    builder->setEnableDepth(true);
    builder->setIs3D(true);

    // Set vertex data - copy vertices to raw buffer
    std::vector<uint8_t> vertexData(terrainMesh.vertices.size() * sizeof(int16_t));
    std::memcpy(vertexData.data(), terrainMesh.vertices.data(), vertexData.size());
    builder->setRawVertices(std::move(vertexData), terrainMesh.vertexCount, gfx::AttributeDataType::Short4);

    // Set index data and segments
    // Create a single segment covering the entire terrain mesh
    SegmentVector segments;
    segments.emplace_back(0, // vertex offset
                          0, // index offset
                          terrainMesh.vertexCount, // vertex count
                          terrainMesh.indexCount); // index count

    std::vector<uint16_t> indexData = terrainMesh.indices;
    builder->setSegments(gfx::Triangles(), std::move(indexData), segments.data(), segments.size());

    if (demTexture) {
        builder->setTexture(demTexture, 0); // slot 0 = demTexture
    }

    // Bind the ideal tile's drape RenderTarget as the surface colour input.
    // DEM sampling may still borrow a source parent through binding.demTL /
    // binding.demScale, but visible map colour must not borrow that parent
    // texture.
    if (auto drape = drapeCache.get(idealID);
        klattraDrapeTargetReady(drape) && drape->getTexture()) {
        if (klattraLogDrapeTrace()) {
            Log::Info(Event::Render,
                      "[KLATTRA DRAPE_TRACE] create-terrain-drawable-bind ideal=" +
                          klattraTileString(idealID) +
                          " source=" + klattraTileString(binding.sourceID) +
                          " drape=" + klattraTileString(idealID) +
                          " demTexture=" + klattraTexturePtrString(demTexture) +
                          " emptyDEM=" + std::to_string(binding.usedEmptyDEM) +
                          " drapeTarget=" + drape->getDebugName() +
                          " drapePtr=" + std::to_string(reinterpret_cast<uintptr_t>(drape.get())) +
                          " drapeCompleted=" + std::to_string(drape->getCompletedRenderCount()) +
                          " drapeGroups=" + std::to_string(drape->numLayerGroups()) +
                          " drapeContentGroups=" + std::to_string(drape->numContentLayerGroups()) +
                          " drapeDrawables=" + std::to_string(klattraDrapeDrawableCount(drape)));
        }
        builder->setTexture(drape->getTexture(), 1); // slot 1 = mapTexture
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

void RenderTerrain::clearRenderState(UniqueChangeRequestVec& changes) {
    if (auto* lg = static_cast<LayerGroup*>(layerGroup.get())) {
        lg->removeDrawablesIf([](gfx::Drawable&) { return true; });
    }

    currentBindings.clear();
    demImagesByTile.clear();
    demTexturesByTile.clear();

    auto evicted = drapeCache.pruneIf([](const OverscaledTileID&) { return true; });
    for (auto& [tileID, target] : evicted) {
        (void)tileID;
        changes.emplace_back(std::make_unique<RemoveRenderTargetRequest>(std::move(target)));
    }
}

} // namespace mbgl
