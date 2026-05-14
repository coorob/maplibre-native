#pragma once

#include <mbgl/shaders/layer_ubo.hpp>
#include <mbgl/shaders/shader_source.hpp>
#include <mbgl/util/immutable.hpp>
#include <mbgl/util/containers.hpp>
#include <mbgl/util/mat4.hpp>

#include <array>
#include <memory>
#include <string>

namespace mbgl {

namespace gfx {
class Drawable;
class UniformBuffer;
using UniformBufferPtr = std::shared_ptr<UniformBuffer>;
} // namespace gfx

namespace style {
class LayerProperties;
enum class TranslateAnchorType : bool;
} // namespace style

class LayerGroupBase;
class OverscaledTileID;
class PaintParameters;
class RenderTree;
class TransformState;
class UnwrappedTileID;

/**
    Base class for layer tweakers, which manipulate layer group per frame
 */
class LayerTweaker {
protected:
    LayerTweaker(std::string id, Immutable<style::LayerProperties> properties);

public:
    LayerTweaker() = delete;
    virtual ~LayerTweaker() = default;

    const std::string& getID() const { return id; }

    virtual void execute(LayerGroupBase&, const PaintParameters&) = 0;

    void updateProperties(Immutable<style::LayerProperties>);

    /// Calculate matrices for this tile.
    /// @param nearClipped If true, the near plane is moved further to enhance depth buffer precision.
    /// @param inViewportPixelUnits If false, the translation is scaled based on the current zoom.
    static mat4 getTileMatrix(const UnwrappedTileID&,
                              const PaintParameters&,
                              const std::array<float, 2>& translation,
                              style::TranslateAnchorType,
                              bool nearClipped,
                              bool inViewportPixelUnits,
                              const gfx::Drawable& drawable,
                              bool aligned = false);

    /// Matrix that maps `sourceID` tile-extent coordinates into the drape
    /// RenderTarget for `drapeID` (ortho-projected, Y-flipped to match the
    /// terrain mesh's texture sample). Used when terrain is active to draw
    /// source-tile layer geometry into the per-DEM-tile drape texture.
    /// `sourceID` and `drapeID` can be at different zooms; the scale and
    /// translation fall out of canonical (z, x, y).
    static mat4 getDrapeMatrix(const OverscaledTileID& sourceID,
                               const OverscaledTileID& drapeID);

    /// True if the two tiles' canonical coordinates intersect (same / parent /
    /// child relationship). Used to decide whether a layer's source tile
    /// should be drawn into a given terrain drape target.
    static bool tilesOverlap(const OverscaledTileID& a, const OverscaledTileID& b);

protected:
    /// Determine whether this tweaker should apply to the given drawable
    bool checkTweakDrawable(const gfx::Drawable&) const;

    /// Multiplies with the projection matrix (either default, near clipped or aligned) for the given drawable
    static void multiplyWithProjectionMatrix(/*in-out*/ mat4& matrix,
                                             const PaintParameters& parameters,
                                             const gfx::Drawable& drawable,
                                             bool nearClipped,
                                             bool aligned);

    std::string id;
    Immutable<style::LayerProperties> evaluatedProperties;

    // Indicates that the evaluated properties have changed
    bool propertiesUpdated = true;
};

} // namespace mbgl
