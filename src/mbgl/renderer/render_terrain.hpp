#pragma once

#include <mbgl/style/terrain_impl.hpp>
#include <mbgl/util/immutable.hpp>
#include <mbgl/util/image.hpp>
#include <mbgl/tile/tile_id.hpp>
#include <mbgl/gfx/vertex_buffer.hpp>
#include <mbgl/gfx/index_buffer.hpp>
#include <mbgl/renderer/render_terrain_drape_cache.hpp>
#include <mbgl/renderer/layer_tweaker.hpp>

#include <memory>
#include <map>
#include <string>
#include <optional>
#include <vector>
#include <cstdint>
#include <cstdlib>
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

    // Per-drawable DEM binding. With parent-fallback DEM sampling, a tile
    // whose own DEM is still streaming can sample the closest available
    // ancestor's texture via a UV sub-rect remap (mirroring gl-js's
    // `_demMatrixCache` in `src/render/terrain.ts:291-305`). The binding
    // captures the texture pointer and remap so the drawable can rebuild
    // when either changes — typically when the exact DEM arrives and
    // takes over from a parent fallback.
    struct DEMBinding {
        DEMBinding(std::shared_ptr<gfx::Texture2D> texture_, const OverscaledTileID& sourceID_)
            : texture(texture_),
              sourceID(sourceID_) {}

        std::shared_ptr<gfx::Texture2D> texture;
        OverscaledTileID sourceID; // tile whose DEM is bound (may be ancestor)
        std::array<float, 2> demTL{{0.0f, 0.0f}};
        float demScale = 1.0f;
        std::shared_ptr<gfx::Texture2D> drapeTexture;
        std::optional<OverscaledTileID> drapeID; // drape target supplying map colour (may be ancestor)
        std::array<float, 2> drapeTL{{0.0f, 0.0f}};
        float drapeScale = 1.0f;
        bool drapeReady = false;
        bool usedEmptyDEM = false;
        bool usedDrapeFallback = false;
    };

    /**
     * @brief Look up the DEM binding (texture + UV remap) for a drawable.
     *
     * The drawable is keyed by its IDEAL OverscaledTileID — the same id
     * that was passed to `drawable->setTileID()` when it was built. The
     * returned binding's `demTL` / `demScale` give the UV sub-rect for
     * sampling the bound DEM texture: identity (`{0,0}, 1`) when the
     * exact tile's data is loaded; a sub-rect when a parent is filling
     * in. Returns nullptr if no binding exists (tile dropped from cover
     * mid-frame, or no DEM data anywhere in the parent chain).
     */
    const DEMBinding* getDEMBinding(const OverscaledTileID& idealID) const {
        auto it = currentBindings.find(idealID);
        return it == currentBindings.end() ? nullptr : &it->second;
    }

    /**
     * @brief Whether the drape pass fully covers a source tile.
     *
     * Main-pass suppression is safe only when an ideal terrain drape target
     * is at the same zoom as the source tile or an ancestor of it. A coarse
     * source tile (for example z4 OpenFreeMap landcover) can overlap a
     * handful of z12 terrain drape targets while still covering a much larger
     * screen area.
     * Treating that partial overlap as "covered" drops the whole z4 main-pass
     * drawable and leaves plain background wherever no terrain target exists.
     *
     * Phase-2 drape-capable layers consult this to decide whether to skip the
     * main 2D pass for a given tile. Skip when the drape pass can cover the
     * entire source tile; fall through to the main pass for partial/coarse
     * overlaps so non-terrain areas stay populated instead of becoming the
     * beige clear colour.
     */
    static uint64_t minCompletedDrapeRenders() noexcept {
        const char* value = std::getenv("KLATTRA_DRAPE_READY_COMPLETED_RENDERS");
        if (!value || !*value) {
            return 2;
        }
        char* end = nullptr;
        const auto parsed = std::strtoull(value, &end, 10);
        return end != value && parsed > 0 ? parsed : 2;
    }

    static bool isDrapeTargetReady(const TerrainDrapeTargetPtr& target) noexcept {
        return target &&
               target->getCompletedRenderCount() >= minCompletedDrapeRenders() &&
               target->hasContentLayerGroups();
    }

    bool hasElevationCoverage(const OverscaledTileID& sourceTileID) const {
        if (drapeCache.size() == 0) return false;
        // Debug/visual experiment: at high pitch, low-zoom vector/raster
        // source tiles that only partially overlap fine DEM drape targets can
        // remain in the main pass as flat translucent polygons over the
        // terrain. Force-skipping any source tile that overlaps a completed
        // drape target lets us isolate that duplicate-main-pass failure mode.
        const bool skipPartialMainPass =
            std::getenv("KLATTRA_TERRAIN_SKIP_PARTIAL_MAIN_PASS") != nullptr;
        // If main-pass suppression waits for the drape target's first
        // completed render, a 2D source tile can be emitted flat while the
        // terrain target is warming up and then remain visible after camera
        // motion settles. This experiment treats an allocated drape target as
        // coverage immediately.
        const bool skipPendingMainPass =
            std::getenv("KLATTRA_TERRAIN_SKIP_PENDING_MAIN_PASS") != nullptr;
        bool covered = false;
        drapeCache.visitAll([&](const OverscaledTileID& drapeID, const TerrainDrapeTargetPtr& target) {
            if (covered) return;
            if (!target ||
                (!skipPendingMainPass &&
                 target->getCompletedRenderCount() < minCompletedDrapeRenders()) ||
                !target->hasContentLayerGroups() ||
                !LayerTweaker::tilesOverlap(sourceTileID, drapeID)) {
                return;
            }
            if (skipPartialMainPass || drapeID.canonical.z <= sourceTileID.canonical.z) covered = true;
        });
        return covered;
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

    /**
     * @brief Remove terrain drawables and drape targets when the DEM source
     * has no renderable cover for the current camera.
     */
    void clearRenderState(UniqueChangeRequestVec& changes);

    // Terrain mesh (shared across all tiles)
    std::optional<TerrainMesh> mesh;

    // Layer group for terrain drawables
    LayerGroupBasePtr layerGroup;

    // Terrain layer tweaker for UBO updates
    std::unique_ptr<TerrainLayerTweaker> tweaker;

    // Existing drawables keyed by the IDEAL tile (the cover slot) — NOT
    // by the source DEM tile. With parent-fallback in play, a single z=10
    // parent DEM can back 16 z=12 ideal drawables, each with its own
    // sub-rect UV remap and matrix. Previously the code keyed by the
    // shared parent, which collapsed all 16 ideals into one z=10-density
    // mesh and produced visibly smoothed mountains during loading.
    std::unordered_map<OverscaledTileID, DEMBinding> currentBindings;

    // Cached 1×1 RGBA texture encoding Mapbox-RGB elevation 0. Used as the
    // DEM input for terrain drawables whose tile is in the cover set but
    // doesn't yet have parsed DEM data (still loading, no archive coverage,
    // or parse failed). Mirrors `_emptyDemTexture` in maplibre-gl-js's
    // `src/render/terrain.ts`. Without this fallback, the loop in
    // `update()` would `continue;` past such tiles and leave holes in the
    // terrain mesh — visible as the flat 2D basemap bleeding through where
    // the mesh should be.
    std::shared_ptr<gfx::Texture2D> emptyDEMTexture;

    // Cached DEM image data, keyed by the overscaled tile ID. Holds a
    // shared_ptr to the underlying image so CPU-side elevation lookups
    // (getElevation) stay valid even if the source bucket gets torn down
    // mid-frame. Refreshed each update() to match the current cover set.
    std::unordered_map<OverscaledTileID, std::shared_ptr<const PremultipliedImage>> demImagesByTile;

    // GPU DEM texture cache keyed by the actual DEM tile's overscaled ID.
    // Persisted across frames so a child drawable doing parent-fallback
    // can borrow a parent's texture without re-uploading it. Pruned each
    // frame to entries that are still reachable from a tile in the cover
    // (either directly or as an ancestor of one).
    std::unordered_map<OverscaledTileID, std::shared_ptr<gfx::Texture2D>> demTexturesByTile;

    // Per-ideal-tile drape RenderTargets. When terrain is active, the 2D
    // layers for each visible terrain cover slot render into one of these
    // offscreen textures instead of straight to the framebuffer; the terrain
    // shader then samples the matching target as the surface colour of the
    // displaced mesh. DEM textures may still use parent fallback, but drape
    // targets stay keyed to the drawable's ideal tile so close views do not
    // inherit coarse parent map colour.
    TerrainDrapeCache drapeCache;

    // Maximum pixel size of each close-zoom drape target. 512 (= DEM tile dimension) made
    // texture-vs-elevation sampling line up 1:1 in the vertex shader, but
    // looked visibly blurry on flat surfaces (glaciers, lake ice) at close
    // zoom — the drape's per-pixel basemap fills are larger than screen
    // pixels there, and there's no way to alias them out without losing
    // detail. 2048² gives a 16× pixel budget per tile (1 MB → 16 MB GPU
    // memory per visible DEM tile, ~96 MB total at 6 visible tiles —
    // comfortable on modern iOS hardware) and restores crisp polygon
    // edges, label antialiasing, and shadow detail on flat drape
    // surfaces. The mesh vertex shader's bilinear sampling handles
    // the 4:1 ratio mismatch with the elevation grid fine. Lower zooms
    // allocate smaller targets in RenderTerrain::update because terrain
    // cover padding can make many z8/z10 drape targets visible at once.
    static constexpr int32_t DRAPE_TARGET_SIZE = 2048;

    // Mesh resolution (vertices per side). The index buffer uses UInt16,
    // so total vertex count must stay under 65,536. With perimeter skirts
    // added by `generateMesh()` to hide basemap bleed-through at tile
    // edges, the budget is:
    //   (MESH_SIZE+1)²  main grid vertices
    // + 4*(MESH_SIZE+1) skirt vertices (one strip per edge)
    // = 65,532 at MESH_SIZE=253 — 3 vertices below the UInt16 ceiling.
    // (The previous mesh-only design fit MESH_SIZE=254 = 65,025 verts.)
    // Switching to UInt32 indices would let us push higher, but 253×253
    // is already overkill for the 514×514 DEM the texture vertex shader
    // samples — every mesh cell oversamples the DEM bilinearly.
    static constexpr size_t MESH_SIZE = 253;

    // Vertical drop (metres of absolute elevation) from each tile-edge
    // skirt vertex to its "below the mesh" anchor. Picked to clear the
    // basemap's flat z=0 plane even when the mesh edge sits on a 2 km
    // peak with exaggeration=2 (worst case in the Klättra coverage):
    //     2000 m × 2 = 4000 m mesh elevation
    // → 4000 m − 5000 m skirt drop = −1000 m, comfortably below z=0.
    // The drape texture is sampled at the edge's UV so the skirt face
    // visually continues the basemap content downward — the camera no
    // longer sees a hard "mesh ends, basemap starts" seam where it
    // looks past the tile boundary or steeply across the mesh edge.
    static constexpr float SKIRT_DROP_METERS = 5000.0f;

    // Cached DEM source
    RenderSource* demSource = nullptr;

    // Layer index for the terrain mesh's layer group. Has to be lower than
    // any user style layer (which start at 0 ascending) so that the terrain
    // mesh draws LAST in the opaque pass — visitLayerGroupsReversed iterates
    // highest-to-lowest, so the lowest index draws last and its opaque pixels
    // overwrite any 2D layer (background, hillshade, etc.) underneath.
    // Without this, the 2D-layer projection-matrix Z offset applied by
    // LayerTweaker::multiplyWithProjectionMatrix can pull 2D layers' depth
    // ahead of the terrain mesh's perspective depth, and the terrain mesh
    // stays hidden.
    static constexpr int32_t TERRAIN_LAYER_INDEX = -1;

    /**
     * @brief Create a DEM texture from DEMData
     * @param context Graphics context
     * @param demData DEM elevation data
     * @return Shared pointer to created texture
     */
    std::shared_ptr<gfx::Texture2D> createDEMTexture(gfx::Context& context, const DEMData& demData);

    /**
     * @brief Lazy-create the 1×1 empty DEM texture used when a tile in
     * cover has no real DEM data yet.
     */
    std::shared_ptr<gfx::Texture2D> getOrCreateEmptyDEMTexture(gfx::Context& context);

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
                                                          const OverscaledTileID& idealID,
                                                          const DEMBinding& binding);
};

} // namespace mbgl
