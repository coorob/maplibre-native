#include <mbgl/renderer/render_terrain.hpp>
#include <mbgl/renderer/update_parameters.hpp>
#include <mbgl/renderer/render_source.hpp>
#include <mbgl/renderer/render_tile.hpp>
#include <mbgl/renderer/render_pass.hpp>
#include <mbgl/renderer/render_tree.hpp>
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

#include <cmath>
#include <cstring>
#include <unordered_set>

namespace mbgl {

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
                           const RenderTree& /*renderTree*/,
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

    // If we don't have a DEM source, we can't create terrain drawables
    if (!demSource) {
        return;
    }

    auto renderTiles = demSource->getRawRenderTiles();
    if (renderTiles->empty()) {
        return;
    }

    auto* lg = static_cast<LayerGroup*>(layerGroup.get());
    if (!lg) {
        return;
    }

    // Phase 1 of the drape pass (see FINISH_TERRAIN.md / TERRAIN_PROGRESS.md):
    // ensure a per-tile RenderTarget exists before we create drawables for
    // those tiles, so the drawable-creation step can bind the matching
    // target's texture as its `mapTexture` (Phase 3). Phase 2 — routing
    // 2D layer drawables into these targets so they have real surface
    // content — is the next step.
    // Allocate a per-tile drape RenderTarget for each visible DEM tile if
    // one doesn't already exist, and prune any targets whose tiles have
    // dropped out of the cover set. The terrain mesh fragment shader
    // samples this target as the surface colour; basemap layers route
    // drawables into it (RenderBackgroundLayer / RenderFillLayer / ...).
    {
        std::unordered_set<OverscaledTileID> currentTileIDs;
        currentTileIDs.reserve(renderTiles->size());
        for (const auto& renderTile : *renderTiles) {
            const auto& tileID = renderTile.getOverscaledTileID();
            currentTileIDs.insert(tileID);
            const bool wasAllocated = drapeCache.get(tileID) != nullptr;
            auto target = drapeCache.getOrCreate(context, tileID, {DRAPE_TARGET_SIZE, DRAPE_TARGET_SIZE});
            if (!wasAllocated && target) {
                changes.emplace_back(std::make_unique<AddRenderTargetRequest>(target));
            }
        }
        drapeCache.pruneIf(
            [&](const OverscaledTileID& id) { return currentTileIDs.find(id) == currentTileIDs.end(); });
    }

    // Create terrain drawables for each DEM tile
    for (const auto& renderTile : *renderTiles) {
        const auto& tileID = renderTile.getOverscaledTileID();

        if (tilesWithDrawables.count(tileID) > 0) {
            continue;
        }

        const auto& tile = renderTile.getTile();
        if (tile.kind != Tile::Kind::RasterDEM) {
            continue;
        }

        auto* demTile = const_cast<RasterDEMTile*>(static_cast<const RasterDEMTile*>(&tile));
        if (!demTile) {
            continue;
        }

        auto* hillshadeBucket = demTile->getBucket();
        if (!hillshadeBucket) {
            continue;
        }

        const auto& demData = hillshadeBucket->getDEMData();
        auto imagePtr = demData.getImagePtr();
        if (!imagePtr || imagePtr->size.isEmpty()) {
            continue;
        }

        auto demTexture = createDEMTexture(context, demData);
        if (!demTexture) {
            continue;
        }

        auto drawable = createDrawableForTile(context, shaders, tileID, demTexture);
        if (drawable) {
            lg->addDrawable(std::move(drawable));
            tilesWithDrawables[tileID] = true;
        }
    }
}

float RenderTerrain::getElevation(const UnwrappedTileID& /*tileID*/, float /*x*/, float /*y*/) const {
    // TODO: Implement DEM tile lookup and bilinear interpolation
    // This would:
    // 1. Find the DEM tile covering this coordinate
    // 2. Get the DEMData from the tile
    // 3. Perform bilinear interpolation to get elevation
    return 0.0f;
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
    // Generate a regular grid mesh for terrain
    // This mesh will be reused for all tiles and displaced by DEM data in shaders

    const size_t gridSize = MESH_SIZE;
    const size_t verticesPerSide = gridSize + 1;
    const size_t totalVertices = verticesPerSide * verticesPerSide;

    // Vertex data: Each vertex has pos (x,y) and texture_pos (u,v)
    // Store as int16_t (short) for Metal short2 attribute format
    // Format: [pos.x, pos.y, tex.u, tex.v, pos.x, pos.y, tex.u, tex.v, ...]
    std::vector<int16_t> vertices;
    vertices.reserve(totalVertices * 4); // 4 shorts per vertex (x, y, u, v)

    const float posStep = static_cast<float>(util::EXTENT) / static_cast<float>(gridSize);
    const float texStep = static_cast<float>(util::EXTENT) / static_cast<float>(gridSize);

    for (size_t y = 0; y < verticesPerSide; ++y) {
        for (size_t x = 0; x < verticesPerSide; ++x) {
            // Position coordinates (in tile space 0-8192)
            vertices.push_back(static_cast<int16_t>(x * posStep));
            vertices.push_back(static_cast<int16_t>(y * posStep));
            // Texture coordinates (same as position for now - will be used to sample DEM)
            vertices.push_back(static_cast<int16_t>(x * texStep));
            vertices.push_back(static_cast<int16_t>(y * texStep));
        }
    }

    // Index data: generate triangles for the grid
    std::vector<uint16_t> indices;
    indices.reserve(gridSize * gridSize * 6); // 2 triangles per grid cell, 3 indices per triangle

    for (size_t y = 0; y < gridSize; ++y) {
        for (size_t x = 0; x < gridSize; ++x) {
            // Calculate vertex indices for this grid cell
            uint16_t topLeft = static_cast<uint16_t>(y * verticesPerSide + x);
            uint16_t topRight = topLeft + 1;
            uint16_t bottomLeft = static_cast<uint16_t>((y + 1) * verticesPerSide + x);
            uint16_t bottomRight = bottomLeft + 1;

            // First triangle (top-left, bottom-left, top-right)
            indices.push_back(topLeft);
            indices.push_back(bottomLeft);
            indices.push_back(topRight);

            // Second triangle (top-right, bottom-left, bottom-right)
            indices.push_back(topRight);
            indices.push_back(bottomLeft);
            indices.push_back(bottomRight);
        }
    }

    // Store mesh data with raw vertices and indices
    mesh = TerrainMesh{
        nullptr, // vertexBuffer - will be created when creating drawable
        nullptr, // indexBuffer - will be created when creating drawable
        vertices.size() / 4, // 4 shorts per vertex (x, y, u, v)
        indices.size(),
        std::move(vertices),
        std::move(indices)
    };
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
                                                                      const OverscaledTileID& tileID,
                                                                      std::shared_ptr<gfx::Texture2D> demTexture) {
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

    // Bind the per-tile drape RenderTarget's offscreen texture as the
    // surface colour input. The drape pass writes basemap layer drawables
    // into this target; the terrain fragment shader samples it. update()
    // always pre-populates the cache for every visible DEM tile, so if we
    // somehow miss here it's a logic error worth flagging.
    if (auto drape = drapeCache.get(tileID); drape && drape->getTexture()) {
        builder->setTexture(drape->getTexture(), 1); // slot 1 = mapTexture
    } else {
        Log::Warning(Event::Render, "Drape target missing for tile " + util::toString(tileID));
    }

    // Flush to create the drawable
    builder->flush(context);

    // Get the drawable
    auto drawables = builder->clearDrawables();
    if (drawables.empty()) {
        Log::Error(Event::Render, "Failed to create terrain drawable for tile");
        return nullptr;
    }

    // Set tile ID on the drawable
    auto& drawable = drawables[0];
    drawable->setTileID(tileID);

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

} // namespace mbgl
