#pragma once

#include <mbgl/shaders/layer_ubo.hpp>

namespace mbgl {
namespace shaders {

// Per-drawable UBO for the terrain mesh.
//
// `dem_tl` + `dem_scale` carry the UV remap that lets a drawable sample a
// DEM texture belonging to a different tile — typically an ancestor whose
// data has finished streaming while this tile's own DEM is still in
// flight. Without parent-fallback DEM sampling, a freshly-paged tile
// would render flat (empty-DEM fallback) and the user would see the mesh
// briefly collapse to sea level until the network catches up. With it,
// the mesh keeps a coarse-but-correct shape and silently sharpens once
// the exact-zoom DEM arrives.
//
// Identity remap is `dem_tl = {0, 0}, dem_scale = 1` — sample the bound
// texture's full [0,1] UV range. For a child at canonical (cx, cy, cz)
// borrowing a parent at canonical (px, py, pz) with dz = cz - pz:
//   dem_scale = 1 / (1 << dz)
//   dem_tl    = ((cx & ((1<<dz)-1)) * dem_scale,
//                (cy & ((1<<dz)-1)) * dem_scale)
// so the child's [0,1]² UV maps to a 1/(1<<dz)² sub-rect of the parent
// texture. Mirrors `_demMatrixCache` in maplibre-gl-js's
// `src/render/terrain.ts:291-305`.
//
// `drape_tl` + `drape_scale` apply the same parent-subrect remap to the
// terrain colour texture. This smooths zoom-in: if the exact child drape
// target has not completed its first render, the drawable can sample the
// nearest ready parent target for one or two frames instead of showing the
// beige fallback.
struct alignas(16) TerrainDrawableUBO {
    /*  0 */ std::array<float, 4 * 4> matrix;
    /* 64 */ std::array<float, 2> dem_tl;
    /* 72 */ float dem_scale;
    /* 76 */ float meters_per_tile;
    /* 80 */ std::array<float, 2> drape_tl;
    /* 88 */ float drape_scale;
    /* 92 */ float pad1;
    /* 96 */
};
static_assert(sizeof(TerrainDrawableUBO) == 96);

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
    /*  8 */ float pad1; // debug colour mode
    /* 12 */ float pad2; // debug vertex/depth mode
    /* 16 */ std::array<float, 4> light_color_pad;          // rgb = color
    /* 32 */ std::array<float, 4> light_position_intensity; // xyz = direction, w = intensity
    /* 48 */
};
static_assert(sizeof(TerrainEvaluatedPropsUBO) == 48);

} // namespace shaders
} // namespace mbgl
