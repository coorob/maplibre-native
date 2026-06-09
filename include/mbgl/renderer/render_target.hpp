#pragma once

#include <mbgl/gfx/types.hpp>
#include <mbgl/tile/tile_id.hpp>
#include <mbgl/util/color.hpp>
#include <mbgl/util/size.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>

namespace mbgl {

namespace gfx {
class Context;
class Texture2D;
class OffscreenTexture;
class UploadPass;
using Texture2DPtr = std::shared_ptr<Texture2D>;
} // namespace gfx

class LayerGroupBase;
class PaintParameters;
class RenderOrchestrator;
class RenderTree;

using LayerGroupBasePtr = std::shared_ptr<LayerGroupBase>;

/// Render target class
class RenderTarget {
public:
    RenderTarget(gfx::Context& context, const Size size, const gfx::TextureChannelDataType type);
    ~RenderTarget();

    /// Get the render target texture
    const gfx::Texture2DPtr& getTexture();

    /// Pixel size of the offscreen target.
    Size getSize() const noexcept;

    /// Number of completed offscreen renders for this target.
    uint64_t getCompletedRenderCount() const noexcept { return completedRenderCount; }

    /// Whether the target has at least one completed texture ready to sample.
    bool hasCompletedRender() const noexcept { return completedRenderCount > 0; }

    /// Optional name used by debug tracing to identify offscreen targets.
    void setDebugName(std::string name_) { debugName = std::move(name_); }
    const std::string& getDebugName() const noexcept { return debugName; }

    /// Enable mipmapped sampling for render targets that will be minified
    /// heavily, such as terrain drape textures viewed at steep pitch.
    void setMipmapped(bool enabled);
    bool isMipmapped() const noexcept { return mipmapped; }

    /// @brief Add a layer group to the render target
    /// @param replace Flag to replace if exists
    /// @return whether added
    bool addLayerGroup(LayerGroupBasePtr, bool replace);

    /// @brief Remove a layer group
    /// @param layerIndex index of the layer to remove
    /// @return whether removed
    bool removeLayerGroup(const int32_t layerIndex);

    /// Remove all layer groups that match a predicate.
    /// @return number of groups removed
    std::size_t removeLayerGroupsIf(const std::function<bool(int32_t, const LayerGroupBase&)>& predicate);

    /// Get the layer group count
    size_t numLayerGroups() const noexcept;

    /// Get the total drawable count across all layer groups, including the
    /// synthetic background group used by terrain drape targets.
    size_t numDrawables() const noexcept;

    /// Get the number of layer groups that contain actual map content.
    /// Terrain drape targets always have a synthetic background group at
    /// INT32_MAX; that group alone is not enough for terrain to sample the
    /// target without producing beige, partially styled terrain.
    size_t numContentLayerGroups() const noexcept;

    /// Whether the target has any non-background map content.
    bool hasContentLayerGroups() const noexcept { return numContentLayerGroups() > 0; }

    /// Mark this drape target as needing raster content before terrain can
    /// safely sample it. Vector-only drape content (trail lines, labels) is
    /// not enough for satellite terrain, because the mesh would otherwise
    /// sample empty colour where the raster tile has not arrived yet.
    void requireRasterDrapeContent() noexcept { requiresRasterDrape = true; }
    bool requiresRasterDrapeContent() const noexcept { return requiresRasterDrape; }
    bool hasRasterDrawableCoveringTile(const OverscaledTileID&) const noexcept;

    /// @brief  Get a specific layer group by index
    /// @param layerIndex index
    /// @return the layer group if existant, othewise a shared null pointer
    const LayerGroupBasePtr& getLayerGroup(const int32_t layerIndex) const;

    /// Execute the given function for each contained layer group
    template <typename Func /* void(LayerGroupBase&) */>
    void visitLayerGroups(Func f) {
        for (auto& pair : layerGroupsByLayerIndex) {
            if (pair.second) {
                f(*pair.second);
            }
        }
    }

    /// Execute the given function for each contained layer group in reversed order
    template <typename Func /* void(LayerGroupBase&) */>
    void visitLayerGroupsReversed(Func f) {
        for (auto rit = layerGroupsByLayerIndex.rbegin(); rit != layerGroupsByLayerIndex.rend(); ++rit) {
            if (rit->second) {
                f(*rit->second);
            }
        }
    }

    /// Upload the layer groups
    void upload(gfx::UploadPass& uploadPass);

    /// Render the layer groups
    void render(RenderOrchestrator&, const RenderTree&, PaintParameters&);

    /// Gated debug readback of the current target texture. This uses the
    /// same KLATTRA_DUMP_* filters as render(), but can be called from a
    /// consumer that samples an already-rendered target later in the frame.
    void inspectDebugPixels();

    /// Clear colour applied at the start of the render-target's offscreen
    /// pass each frame. Default is opaque black to match historical
    /// behaviour. Terrain drape targets override this with a low-saturation
    /// debug colour so the displaced terrain mesh is visibly distinct from
    /// "nothing rendered" even before any layer drawables are routed into
    /// the target — useful for verifying the drape pipeline is wired.
    void setClearColor(Color color) { clearColor = color; }
    Color getClearColor() const { return clearColor; }

protected:
    gfx::Context& context;
    std::unique_ptr<gfx::OffscreenTexture> offscreenTexture;
    using LayerGroupMap = std::map<int32_t, LayerGroupBasePtr>;
    LayerGroupMap layerGroupsByLayerIndex;
    Color clearColor{0.0f, 0.0f, 0.0f, 1.0f};
    uint64_t completedRenderCount = 0;
    std::string debugName;
    bool mipmapped = false;
    bool requiresRasterDrape = false;
};

} // namespace mbgl
