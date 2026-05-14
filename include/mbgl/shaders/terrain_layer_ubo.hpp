#pragma once

#include <mbgl/shaders/layer_ubo.hpp>

namespace mbgl {
namespace shaders {

struct alignas(16) TerrainDrawableUBO {
    /*  0 */ std::array<float, 4 * 4> matrix;
    /* 64 */
};
static_assert(sizeof(TerrainDrawableUBO) == 4 * 16);

struct alignas(16) TerrainTilePropsUBO {
    /*  0 */ std::array<float, 2> dem_tl;
    /*  8 */ float dem_scale;
    /* 12 */ float pad1;
    /* 16 */
};
static_assert(sizeof(TerrainTilePropsUBO) == 16);

/// Evaluated properties that do not depend on the tile.
/// `light_*` mirror the fill-extrusion layer's interpretation of the style's
/// global `light` block, so the diffuse hillshade on the terrain mesh stays
/// consistent with 3D building shading in the same style. Each float3 is
/// padded to float4 (16-byte alignment for Metal).
struct alignas(16) TerrainEvaluatedPropsUBO {
    /*  0 */ float exaggeration;
    /*  4 */ float elevation_offset;
    /*  8 */ float pad1;
    /* 12 */ float pad2;
    /* 16 */ std::array<float, 4> light_color_pad;          // rgb = color
    /* 32 */ std::array<float, 4> light_position_intensity; // xyz = direction, w = intensity
    /* 48 */
};
static_assert(sizeof(TerrainEvaluatedPropsUBO) == 48);

} // namespace shaders
} // namespace mbgl
