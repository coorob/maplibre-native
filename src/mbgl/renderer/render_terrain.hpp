#pragma once

#include <mbgl/style/terrain_impl.hpp>
#include <mbgl/util/immutable.hpp>
#include <mbgl/tile/tile_id.hpp>
#include <mbgl/gfx/vertex_buffer.hpp>
#include <mbgl/gfx/index_buffer.hpp>
#include <mbgl/renderer/render_terrain_drape_cache.hpp>

#include <memory>
#include <map>
#include <string>
#include <optional>
#include <vector>
#include <cstdint>
#include <unordered_map>

namespace mbgl {

class TransformState;
class UpdateParameters;
class RenderSource;
class PaintParameters;
class RenderTree;
class LayerGroupBase;
class TerrainLayerTweaker;
class DEMData;
using LayerGroupBasePtr = std::shared_ptr<LayerGroupBase>;
using UniqueChangeRequestVec = std::vector<std::unique_ptr<class ChangeRequest>>;

namespace gfx {
class Context;
class Drawable;
class ShaderRegistry;
class Texture2D;
}

/**
 * @brief Manages 3D terrain rendering using DEM (Digital Elevation Model) data
 *
 * RenderTerrain is responsible for:
 * - Loading and caching DEM tiles from raster-dem sources
 * - Generating and caching terrain mesh geometry
 * - Providing elevation lookups for any coordinate
 * - Managing GPU resources for terrain rendering
 */
class RenderTerrain {
public:
    RenderTerrain(Immutable<style::Terrain::Impl>);
    ~RenderTerrain();

    /**
     * @brief Update terrain state for the current frame
     * @param parameters Update parameters including transform state and sources
     */
    void update(const UpdateParameters& parameters);

    /**
     * @brief Update terrain rendering (create/update drawables)
     * @param orchestrator Render orchestrator for accessing render sources
     * @param shaders Shader registry for getting terrain shader
     * @param context Graphics context for creating drawables and layer groups
     * @param state Transform state
     * @param updateParameters Update parameters
     * @param renderTree Render tree
     * @param changes Vector to collect change requests
     */
    void update(class RenderOrchestrator& orchestrator,
                gfx::ShaderRegistry& shaders,
                gfx::Context& context,
                const TransformState& state,
                const std::shared_ptr<UpdateParameters>& updateParameters,
                const RenderTree& renderTree,
                UniqueChangeRequestVec& changes);

    /**
     * @brief Get elevation at a specific tile coordinate
     * @param tileID The tile containing the coordinate
     * @param x X coordinate within the tile
     * @param y Y coordinate within the tile
     * @return Elevation in meters (or 0 if no DEM data available)
     */
    float getElevation(const UnwrappedTileID& tileID, float x, float y) const;

    /**
     * @brief Get elevation with exaggeration applied
     * @param tileID The tile containing the coordinate
     * @param x X coordinate within the tile
     * @param y Y coordinate within the tile
     * @return Elevation in meters with exaggeration multiplier applied
     */
    float getElevationWithExaggeration(const UnwrappedTileID& tileID, float x, float y) const;

    /**
     * @brief Get the terrain exaggeration multiplier
     */
    float getExaggeration() const;

    /**
     * @brief Get the source ID providing DEM data
     */
    const std::string& getSourceID() const;

    /**
     * @brief Check if terrain is enabled and has DEM data
     */
    bool isEnabled() const;

    /**
     * @brief Get the terrain implementation
     */
    const Immutable<style::Terrain::Impl>& getImpl() const { return impl; }

    /**
     * @brief Look up the per-tile drape RenderTarget for `tileID`.
     *
     * Returns nullptr if terrain isn't ensuring this tile has a target
     * yet (e.g., the tile isn't in the current DEM source's visible set).
     * Phase 2 calls this from each drapeable layer's `update()` to know
     * where to add its drawables — the returned target's layer-group is
     * the destination for that tile's portion of that layer's drawables.
     *
     * Phase 1/3 only uses this internally to bind the target's texture
     * into the terrain drawable. Once Phase 2 exposes this to the
     * orchestrator + per-layer code, this signature is the public API.
     */
    TerrainDrapeTargetPtr getDrapeTarget(const OverscaledTileID& tileID) const {
        return drapeCache.get(tileID);
    }

    /**
     * @brief Visit every (tileID, RenderTarget) currently in the drape cache.
     *
     * Phase 2 layers call this to enumerate the tiles they need to emit
     * drape drawables for. Iterating the drape cache directly — rather
     * than the layer's own tileCover — sidesteps the zoom-mismatch
     * between the DEM source (which the drape cache mirrors) and other
     * sources at different zooms. Each callback gets the OverscaledTileID
     * the drape target is keyed under and the target pointer itself.
     */
    template <typename Func /* void(const OverscaledTileID&, TerrainDrapeTargetPtr&) */>
    void visitDrapeTargets(Func f) {
        drapeCache.visitAll(f);
    }

    /**
     * @brief Get the terrain mesh for a specific tile
     *
     * Returns a cached mesh or generates a new one. The mesh is a regular grid
     * that will be displaced by DEM data in the vertex shader.
     *
     * @param tileID The tile ID (unused currently - same mesh for all tiles)
     * @return Pointer to vertex buffer and index buffer
     */
    struct TerrainMesh {
        std::shared_ptr<gfx::VertexBuffer<float>> vertexBuffer;
        std::shared_ptr<gfx::IndexBuffer> indexBuffer;
        size_t vertexCount;
        size_t indexCount;
        std::vector<int16_t> vertices;  // Raw vertex data (x, y pairs as short2)
        std::vector<uint16_t> indices;  // Raw index data
    };

    const TerrainMesh& getMesh(gfx::Context& context);

    /**
     * @brief Get the layer group for terrain drawables
     */
    const LayerGroupBasePtr& getLayerGroup() const { return layerGroup; }

    /**
     * @brief Get the terrain layer tweaker
     */
    TerrainLayerTweaker* getTweaker() const { return tweaker.get(); }

    // Immutable terrain configuration
    Immutable<style::Terrain::Impl> impl;

private:
    /**
     * @brief Generate terrain mesh geometry
     *
     * Creates a regular grid mesh (default 128x128) with border frames
     * to prevent stitching artifacts between tiles.
     */
    void generateMesh(gfx::Context& context);

    /**
     * @brief Find the DEM source for the current terrain
     */
    RenderSource* findDEMSource(const UpdateParameters& parameters);

    /**
     * @brief Activate or deactivate the layer group
     */
    void activateLayerGroup(bool activate, UniqueChangeRequestVec& changes);

    // Terrain mesh (shared across all tiles)
    std::optional<TerrainMesh> mesh;

    // Layer group for terrain drawables
    LayerGroupBasePtr layerGroup;

    // Terrain layer tweaker for UBO updates
    std::unique_ptr<TerrainLayerTweaker> tweaker;

    // Track which tiles have terrain drawables
    std::unordered_map<OverscaledTileID, bool> tilesWithDrawables;

    // Per-tile drape RenderTargets. When terrain is active, the 2D layers
    // for each visible tile render into one of these offscreen textures
    // instead of straight to the framebuffer; the terrain shader then
    // samples the matching target as the surface colour of the displaced
    // mesh. Phase 1 just allocates and lifecycle-manages the targets; the
    // actual layer routing into them is Phase 2 (see FINISH_TERRAIN.md).
    TerrainDrapeCache drapeCache;

    // Pixel size of each drape target. Matches our DEM tile size so the
    // texture sampling lines up 1:1 with the elevation grid in the vertex
    // shader. Hard-coded for now; future work may read this from the DEM
    // source metadata to support 256-tile terrarium sources too.
    static constexpr int32_t DRAPE_TARGET_SIZE = 512;

    // Mesh resolution (vertices per side)
    static constexpr size_t MESH_SIZE = 128;

    // Cached DEM source
    RenderSource* demSource = nullptr;

    // Layer index (terrain renders early in 3D pass, use negative index)
    // TEMP: Using positive index to render ON TOP for debugging visibility
    static constexpr int32_t TERRAIN_LAYER_INDEX = 10000;

    /**
     * @brief Create a DEM texture from DEMData
     * @param context Graphics context
     * @param demData DEM elevation data
     * @return Shared pointer to created texture
     */
    std::shared_ptr<gfx::Texture2D> createDEMTexture(gfx::Context& context, const DEMData& demData);

    /**
     * @brief Create a test map texture (checkerboard pattern)
     * This will be replaced with render-to-texture output later
     * @param context Graphics context
     * @return Shared pointer to created texture
     */
    std::shared_ptr<gfx::Texture2D> createTestMapTexture(gfx::Context& context);

    /**
     * @brief Create a terrain drawable for a specific tile
     * @param context Graphics context
     * @param shaders Shader registry
     * @param tileID Tile ID for this drawable
     * @param demTexture DEM texture for elevation data
     * @return Unique pointer to created drawable
     */
    std::unique_ptr<gfx::Drawable> createDrawableForTile(gfx::Context& context,
                                                          gfx::ShaderRegistry& shaders,
                                                          const OverscaledTileID& tileID,
                                                          std::shared_ptr<gfx::Texture2D> demTexture);
};

} // namespace mbgl
