#pragma once

#include <mbgl/gfx/types.hpp>
#include <mbgl/util/color.hpp>
#include <mbgl/util/size.hpp>

#include <functional>
#include <map>
#include <memory>
#include <string>

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

    /// @brief Add a layer group to the render target
    /// @param replace Flag to replace if exists
    /// @return whether added
    bool addLayerGroup(LayerGroupBasePtr, bool replace);

    /// @brief Remove a layer group
    /// @param layerIndex index of the layer to remove
    /// @return whether removed
    bool removeLayerGroup(const int32_t layerIndex);

    /// Get the layer group count
    size_t numLayerGroups() const noexcept;

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
};

} // namespace mbgl
