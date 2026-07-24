#pragma once

#include <mbgl/style/terrain_impl.hpp>
#include <mbgl/util/immutable.hpp>
#include <mbgl/util/image.hpp>
#include <mbgl/tile/tile_id.hpp>
#include <mbgl/gfx/vertex_buffer.hpp>
#include <mbgl/gfx/index_buffer.hpp>
#include <mbgl/gfx/vertex_vector.hpp>
#include <mbgl/gfx/index_vector.hpp>
#include <mbgl/renderer/render_terrain_drape_cache.hpp>
#include <mbgl/renderer/layer_tweaker.hpp>
#include <mbgl/util/geo.hpp>

#include <chrono>
#include <deque>
#include <memory>
#include <map>
#include <string>
#include <optional>
#include <vector>
#include <cstdint>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

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
     * @brief Release everything this terrain registered with the renderer.
     *
     * Must be called (and the change requests applied) before the
     * orchestrator destroys or replaces this RenderTerrain. The destructor
     * cannot do this — it has no UniqueChangeRequestVec — so a plain
     * reset()/reassignment strands every drape render target (~MBs each,
     * re-rendered as an offscreen pass every frame) and leaves the terrain
     * layer group active in the renderer.
     */
    void teardown(UniqueChangeRequestVec& changes);

    /**
     * @brief Release parked resize predecessors and out-of-cover ancestor
     * fallback targets, emitting the remove requests. A real platform memory
     * warning also activates the launch-gated sticky drape pressure cap;
     * routine lifecycle cleanup must not.
     */
    void reduceMemoryUse(UniqueChangeRequestVec& changes, bool memoryPressure);

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
     * @brief Sample the loaded DEM at a geographic coordinate.
     *
     * Returns std::nullopt when the current terrain cover has not loaded a DEM
     * tile containing the coordinate yet.
     */
    std::optional<float> getElevationAtLatLng(const LatLng& latLng) const;

    /**
     * @brief DEM elevation currently used as the terrain vertical origin.
     *
     * Native camera zoom/altitude is expressed relative to the map plane. In
     * high mountains, rendering absolute sea-level DEM elevations can put the
     * camera inside the mesh at close drone zooms. Terrain is therefore drawn
     * relative to the sampled elevation under the map center.
     */
    std::optional<float> getElevationOriginMeters() const { return elevationOriginMeters; }

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
     * @brief True while the drape system has work that only progresses on
     * rendered frames — unbaked targets, cap-deferred resizes, or cover
     * tiles still waiting for their first drape texture. The orchestrator
     * folds this into needsRepaint so a static camera cannot freeze the
     * backlog on screen (paused-flyover voids that only filled when a
     * screenshot forced frames, 2026-07-04).
     */
    bool hasPendingDrapeWork() const noexcept { return drapeWorkPending; }

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
     * @brief Whether ready terrain drawables cover a source tile.
     *
     * Main-pass suppression must wait until terrain has a current binding
     * with a ready drape texture. An allocated render target is not enough:
     * if the raster layer drops its normal drawable before the terrain
     * drawable exists, the framebuffer shows the map's clear colour.
     *
     * Phase-2 drape-capable layers consult this to decide whether to skip the
     * main 2D pass for a given tile. Skip when the ready terrain pass can
     * cover the entire source tile; fall through to the main pass for
     * partial/coarse overlaps so non-terrain areas stay populated.
     */
    static uint64_t minCompletedDrapeRenders() noexcept {
        const char* value = std::getenv("KLATTRA_DRAPE_READY_COMPLETED_RENDERS");
        if (!value || !*value) {
            return 1;
        }
        char* end = nullptr;
        const auto parsed = std::strtoull(value, &end, 10);
        return end != value && parsed > 0 ? parsed : 1;
    }

    static bool isDrapeTargetReady(const TerrainDrapeTargetPtr& target) noexcept {
        return target &&
               target->getCompletedRenderCount() >= minCompletedDrapeRenders() &&
               target->hasContentLayerGroups();
    }

    static bool isDrapeTargetReadyForTile(const TerrainDrapeTargetPtr& target,
                                          const OverscaledTileID& tileID) noexcept {
        if (!target || target->getCompletedRenderCount() < minCompletedDrapeRenders()) {
            return false;
        }
        (void)tileID;
        return target->hasContentLayerGroups();
    }

    bool hasReadyTerrainCoverage(const OverscaledTileID& sourceTileID) const {
        if (currentBindings.empty()) return false;
        for (const auto& [idealID, binding] : currentBindings) {
            if (binding.drapeReady &&
                binding.drapeTexture &&
                LayerTweaker::tilesOverlap(sourceTileID, idealID)) {
                return true;
            }
        }
        return false;
    }

    bool hasElevationCoverage(const OverscaledTileID& sourceTileID) const {
        if (currentBindings.empty()) return false;
        // Debug/visual experiment: at high pitch, low-zoom vector/raster
        // source tiles that only partially overlap fine DEM drape targets can
        // remain in the main pass as flat translucent polygons over the
        // terrain. Force-skipping any source tile that overlaps a completed
        // drape target lets us isolate that duplicate-main-pass failure mode.
        const bool skipPartialMainPass =
            std::getenv("KLATTRA_TERRAIN_SKIP_PARTIAL_MAIN_PASS") != nullptr;
        for (const auto& [idealID, binding] : currentBindings) {
            if (!binding.drapeReady ||
                !binding.drapeTexture ||
                !LayerTweaker::tilesOverlap(sourceTileID, idealID)) {
                continue;
            }
            if (skipPartialMainPass || idealID.canonical.z <= sourceTileID.canonical.z) {
                return true;
            }
        }
        return false;
    }

    // ---- .53 stage diagnostics ----
    // Per-drape-canvas pipeline timeline: create → raster overlap seen →
    // raster paintable (texture source exists) → raster routed (drape
    // drawable added) → baked with that content → first bound as a visible
    // ideal. The 1 Hz [KLATTRA STAGE] summary decomposes the flyover
    // artifact backlog into WHICH stage the stuck canvases wait in —
    // source cover, decode/upload, routing, or bake. Explicitly enable with
    // KLATTRA_STAGEDIAG=1. Render-thread only.
    static bool stageDiagEnabled();
    // Called from RenderRasterLayer's drape block: a raster render tile
    // overlaps this drape canvas; `paintable` = the raster bucket has a
    // CPU image or an uploaded GPU texture (i.e. could actually route).
    void diagNoteRasterOverlap(const OverscaledTileID& drapeID, uint8_t rasterZ, bool paintable) const;
    // Called when a raster drape drawable is actually added to the canvas.
    void diagNoteRasterRouted(const OverscaledTileID& drapeID, uint8_t rasterZ) const;
    // .54: called when raster drape drawables are REMOVED from a canvas
    // (raster cover shift / stale drop) — a readiness-revocation suspect.
    void diagNoteRasterRevoked(const OverscaledTileID& drapeID, std::size_t removed) const;

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
        // The same mesh drawn by every terrain tile, built once and handed to
        // every drawable builder so the backend uploads ONE vertex and ONE
        // index GPU buffer instead of a fresh ~330 KB copy per drawable
        // (fork review #2 item 8). Type-erased because the layout-vertex
        // struct is local to render_terrain.cpp.
        std::shared_ptr<gfx::VertexVectorBase> sharedLayoutVertices;
        gfx::IndexVectorBasePtr sharedIndexes;
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

    /**
     * @brief Current style's solid background — the terrain shader's
     * no-drape-pixel fallback tone (updated each update() from the render
     * tree; defaults to the topo paper it used to hardcode).
     */
    const Color& getDrapeFallbackColor() const { return drapeFallbackColor; }

    // Immutable terrain configuration
    Immutable<style::Terrain::Impl> impl;

private:
    Color drapeFallbackColor = {0.95686275f, 0.91764706f, 0.81568627f, 1.0f};
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

    // Smoothed-ish local terrain origin, sampled at the current map center
    // whenever a covering DEM tile is available. The shader subtracts this
    // origin (after exaggeration) so close pitched cameras stay above the
    // local surface while preserving nearby relief.
    std::optional<float> elevationOriginMeters;

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

    // Number of consecutive update frames where the terrain cover set has
    // stayed unchanged while the camera is not actively moving. Used to
    // defer expensive 2048px drape targets until the view has settled.
    std::unordered_set<OverscaledTileID> previousIdealIDs;
    uint32_t stableDrapeCoverFrames = 0;

    // Replaced drape targets stay here until their higher-resolution
    // successor completes a render, so quality upgrades do not blank the
    // terrain surface for a frame.
    std::unordered_map<OverscaledTileID, TerrainDrapeTargetPtr> retiredDrapeTargetsByTile;

    // Distance-ring membership per drape tile (0 = near, 1 = mid, 2 = far)
    // with demotion hysteresis — see drapeTargetSizeForTile in
    // render_terrain.cpp. Pruned alongside the drape cache.
    std::unordered_map<OverscaledTileID, uint8_t> drapeRingByTile;

    // Frames a leaving-ideal tile's drawable has been held waiting for its
    // replacement ideals to become drawable-backed (zoom-level transitions).
    // See the prune block in updateDrapeTargets.
    std::unordered_map<OverscaledTileID, uint32_t> pruneHoldAgeByTile;

    // Set each update: drape work exists that only progresses on rendered
    // frames (unbaked targets, cap-deferred resizes, texture-less cover
    // tiles). Read by the orchestrator via hasPendingDrapeWork().
    bool drapeWorkPending = false;

    // Launch-gated sticky response to the shared high-water signal or an
    // explicit platform memory warning. It reduces only out-of-view drape
    // population; visible terrain meshes remain protected by the selection
    // loop in update().
    bool drapePressureCapActive = false;

    // Recent camera centres in global mercator [0,1) units. The displacement
    // across this short window is the ground-velocity estimate that drives
    // the forward drape pre-bake strip (the lookahead block in update()).
    // Using window displacement rather than per-frame deltas means jittery
    // back-and-forth gestures net out to ~zero and produce no strip.
    struct DrapeCameraSample {
        double x;
        double y;
        std::chrono::steady_clock::time_point time;
    };
    std::deque<DrapeCameraSample> drapeCameraSamples;

    // .53 stage-diag registry (see stageDiagEnabled above). One entry per
    // drape canvas ever created this session (capped); timestamps advance
    // monotonically through the pipeline, so "current stage" = furthest
    // stage reached. mutable: the raster layer reports through a const
    // RenderTerrain*; all access is render-thread only.
    struct DrapeStageEntry {
        std::chrono::steady_clock::time_point created{};
        std::chrono::steady_clock::time_point rasterOverlap{};
        std::chrono::steady_clock::time_point rasterAvail{};
        std::chrono::steady_clock::time_point rasterRouted{};
        std::chrono::steady_clock::time_point bakedWithRaster{};
        std::chrono::steady_clock::time_point firstBound{};
        uint8_t bestAvailZ = 0;
        uint8_t routedFromZ = 0;
        // 0 = own target ready at first bind, 1 = ancestor fallback,
        // 2 = background-only bind, 3 = unbound (hole).
        uint8_t firstBoundState = 255;
        // .54 readiness-cycle tracking: canvases don't just become ready
        // once — resizes recreate them empty and raster cover shifts revoke
        // their content. Each ready→unready→ready cycle is a visible
        // artifact window; its duration distribution IS the smear.
        bool wasReadyForTile = false;
        std::chrono::steady_clock::time_point unreadySince{};
        uint32_t regressions = 0;
        uint32_t revokes = 0;
    };
    mutable std::unordered_map<OverscaledTileID, DrapeStageEntry> drapeStageByTile;
    // Completed-transition latency samples (ms), capped; summarised at 1 Hz.
    mutable std::vector<float> stageAvailMs;
    mutable std::vector<float> stageRouteMs;
    mutable std::vector<float> stageBakeMs;
    mutable std::vector<float> stageLateMs;
    // .54: ready→unready→ready cycle durations (ms) and cumulative causes.
    mutable std::vector<float> stageReReadyMs;
    mutable uint64_t stageRevokeEvents = 0;
    mutable uint64_t stageRegressionEvents = 0;
    mutable uint64_t stageResizeEvents = 0;
    // .56: canvas render-target allocation failures (memory pressure). A
    // failed tile retries next frame (the cache no longer stores nulls);
    // this counts the events so green plates can be attributed.
    mutable uint64_t stageAllocFailEvents = 0;

    // Maximum stable-view pixel size of each close-zoom drape target. Moving
    // cameras allocate smaller close targets first and upgrade to this after
    // the cover settles, so fast pans/zooms do not block on multiple 2048²
    // offscreen renders. 512 (= DEM tile dimension) made texture-vs-elevation
    // sampling line up 1:1 in the vertex shader, but looked visibly blurry on
    // flat surfaces (glaciers, lake ice) at close zoom. 2048² restores crisp
    // polygon edges, label antialiasing, and shadow detail once stationary.
    // Lower zooms allocate smaller targets in RenderTerrain::update because
    // terrain cover padding can make many z8/z10 drape targets visible at once.
    static constexpr int32_t DRAPE_TARGET_SIZE = 2048;

    // Mesh resolution (cells per side). A 128×128 grid keeps the terrain
    // surface dense enough for drone-distance relief while avoiding the
    // near-UInt16 vertex-limit mesh that made iOS Metal debugging painful.
    static constexpr size_t MESH_SIZE = 128;

    // Cached DEM source
    RenderSource* demSource = nullptr;

    // Layer index for the terrain mesh's layer group. Style layers start at 0
    // and render in ascending order in the translucent pass. Terrain samples a
    // completed offscreen drape texture, so it must draw after the ordinary 2D
    // satellite/vector layers; otherwise a surviving flat raster drawable can
    // cover the raised mesh and make the scene look like a pitched 2D map.
    static constexpr int32_t TERRAIN_LAYER_INDEX = 1000000;

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
