#include <mbgl/renderer/layer_tweaker.hpp>

#include <mbgl/map/transform_state.hpp>
#include <mbgl/style/layer_properties.hpp>
#include <mbgl/renderer/render_tree.hpp>
#include <mbgl/renderer/render_tile.hpp>
#include <mbgl/shaders/layer_ubo.hpp>
#include <mbgl/util/mat4.hpp>
#include <mbgl/util/containers.hpp>
#include <mbgl/util/constants.hpp>
#include <mbgl/tile/tile_id.hpp>

#if MLN_RENDER_BACKEND_METAL
#include <mbgl/util/monotonic_timer.hpp>
#include <chrono>
#endif // MLN_RENDER_BACKEND_METAL

namespace mbgl {

LayerTweaker::LayerTweaker(std::string id_, Immutable<style::LayerProperties> properties)
    : id(std::move(id_)),
      evaluatedProperties(std::move(properties)) {}

bool LayerTweaker::checkTweakDrawable(const gfx::Drawable& drawable) const {
    // Apply to a drawable if it references us, or if doesn't reference anything.
    const auto& tweaker = drawable.getLayerTweaker();
    return !tweaker || tweaker.get() == this;
}

mat4 LayerTweaker::getTileMatrix(const UnwrappedTileID& tileID,
                                 const PaintParameters& parameters,
                                 const std::array<float, 2>& translation,
                                 style::TranslateAnchorType anchor,
                                 bool nearClipped,
                                 bool inViewportPixelUnits,
                                 const gfx::Drawable& drawable,
                                 bool aligned) {
    // from RenderTile::prepare
    mat4 tileMatrix;
    parameters.state.matrixFor(/*out*/ tileMatrix, tileID);
    if (const auto& origin{drawable.getOrigin()}; origin.has_value()) {
        matrix::translate(tileMatrix, tileMatrix, origin->x, origin->y, 0);
    }
    multiplyWithProjectionMatrix(/*in-out*/ tileMatrix, parameters, drawable, nearClipped, aligned);
    return RenderTile::translateVtxMatrix(
        tileID, tileMatrix, translation, anchor, parameters.state, inViewportPixelUnits);
}

mat4 LayerTweaker::getDrapeMatrix(const OverscaledTileID& sourceID, const OverscaledTileID& drapeID) {
    // Source vertex (x_s, y_s) ∈ [0, EXTENT] maps to drape coordinates
    //     (x_d, y_d) = (x_s * r + (sX * r - dX) * EXTENT,
    //                   y_s * r + (sY * r - dY) * EXTENT)
    // where r = 2^(dZ - sZ) handles zoom-level mismatch between the
    // basemap tile and the DEM tile owning the drape target.
    // The ortho projection then maps drape coordinates to clip space,
    // with Y flipped so the drape texture is sampled correctly by the
    // terrain mesh fragment shader.
    const double sZ = sourceID.canonical.z;
    const double dZ = drapeID.canonical.z;
    const double r = std::pow(2.0, dZ - sZ);
    const double tx = (static_cast<double>(sourceID.canonical.x) * r - drapeID.canonical.x) * util::EXTENT;
    const double ty = (static_cast<double>(sourceID.canonical.y) * r - drapeID.canonical.y) * util::EXTENT;

    mat4 m;
    matrix::ortho(m, 0.0, util::EXTENT, -util::EXTENT, 0.0, -1.0, 1.0);
    matrix::translate(m, m, tx, ty - util::EXTENT, 0.0);
    matrix::scale(m, m, r, r, 1.0);
    return m;
}

void LayerTweaker::updateProperties(Immutable<style::LayerProperties> newProps) {
    evaluatedProperties = std::move(newProps);
    propertiesUpdated = true;
}

void LayerTweaker::multiplyWithProjectionMatrix(/*in-out*/ mat4& matrix,
                                                const PaintParameters& parameters,
                                                [[maybe_unused]] const gfx::Drawable& drawable,
                                                bool nearClipped,
                                                bool aligned) {
    // nearClippedMatrix has near plane moved further, to enhance depth buffer precision
    const auto& projMatrixRef = aligned ? parameters.transformParams.alignedProjMatrix
                                        : (nearClipped ? parameters.transformParams.nearClippedProjMatrix
                                                       : parameters.transformParams.projMatrix);
#if !MLN_RENDER_BACKEND_OPENGL
    // If this drawable is participating in depth testing, offset the
    // projection matrix NDC depth range for the drawable's layer and sublayer.
    if (!drawable.getIs3D() && drawable.getEnableDepth()) {
        // copy and adjust the projection matrix
        mat4 projMatrix = projMatrixRef;
        projMatrix[14] -= ((1 + parameters.currentLayer) * PaintParameters::numSublayers -
                           drawable.getSubLayerIndex()) *
                          PaintParameters::depthEpsilon;
        // multiply with the copy
        matrix::multiply(matrix, projMatrix, matrix);
        // early return
        return;
    }
#endif
    matrix::multiply(matrix, projMatrixRef, matrix);
}

} // namespace mbgl
