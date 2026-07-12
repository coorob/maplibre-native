#pragma once

#include <mbgl/shaders/terrain_layer_ubo.hpp>
#include <mbgl/shaders/shader_source.hpp>
#include <mbgl/shaders/mtl/shader_program.hpp>

namespace mbgl {
namespace shaders {

constexpr auto terrainShaderPrelude = R"(

enum {
    idTerrainDrawableUBO = idDrawableReservedVertexOnlyUBO,
    idTerrainTilePropsUBO = idDrawableReservedFragmentOnlyUBO,
    idTerrainEvaluatedPropsUBO = drawableReservedUBOCount,
    terrainUBOCount
};

struct alignas(16) TerrainDrawableUBO {
    /*  0 */ float4x4 matrix;
    /* 64 */ float2 dem_tl;
    /* 72 */ float dem_scale;
    /* 76 */ float meters_per_tile;
    /* 80 */ float2 drape_tl;
    /* 88 */ float drape_scale;
    /* 92 */ float elevation_offset_scale;
    /* 96 */
};
static_assert(sizeof(TerrainDrawableUBO) == 96, "wrong size");

struct alignas(16) TerrainTilePropsUBO {
    /*  0 */ float2 dem_tl;
    /*  8 */ float dem_scale;
    /* 12 */ float pad1;
    /* 16 */
};
static_assert(sizeof(TerrainTilePropsUBO) == 16, "wrong size");

/// Evaluated properties that do not depend on the tile.
/// `light_*` come from the style's global light block (same source as
/// fill-extrusion), so the diffuse hillshade stays consistent. float3 is
/// 16-byte-aligned in Metal so we pad explicitly via float4 in the
/// declaration to keep the C++ host layout matching.
struct alignas(16) TerrainEvaluatedPropsUBO {
    /*  0 */ float exaggeration;
    /*  4 */ float elevation_offset;
    /*  8 */ float pad1; // debug colour mode
    /* 12 */ float pad2; // debug vertex/depth mode
    /* 16 */ float4 light_color_pad;          // rgb = colour
    /* 32 */ float4 light_position_intensity; // xyz = direction, w = intensity
    /* 48 */ float4 fallback_color;           // rgb = no-drape-pixel colour (style background)
    /* 64 */ float4 haze_color;               // rgb = horizon haze tint, a = max opacity
    /* 80 */ float4 haze_params;              // x = start (clip-w), y = 1/(end-start)
    /* 96 */
};
static_assert(sizeof(TerrainEvaluatedPropsUBO) == 96, "wrong size");

)";

template <>
struct ShaderSource<BuiltIn::TerrainShader, gfx::Backend::Type::Metal> {
    static constexpr auto name = "TerrainShader";
    static constexpr auto vertexMainFunction = "vertexMain";
    static constexpr auto fragmentMainFunction = "fragmentMain";

    static const std::array<AttributeInfo, 2> attributes;
    static constexpr std::array<AttributeInfo, 0> instanceAttributes{};
    static const std::array<TextureInfo, 2> textures;

    static constexpr auto prelude = terrainShaderPrelude;
    static constexpr auto source = R"(

struct VertexStage {
    short2 pos [[attribute(0)]];
    short2 texture_pos [[attribute(1)]];
};

struct FragmentStage {
    float4 position [[position, invariant]];
    float2 uv;       // raw [0,1] across the IDEAL tile, used for sampling
                     // the ideal tile's drape target.
    float2 mapUV;    // UV remapped to the DEM source-tile's sub-rect. With
                     // identity remap (exact-zoom data), `mapUV == uv`; with
                     // parent fallback, `mapUV` lands inside the parent's
                     // sub-rect. This is for DEM elevation/relief only.
    float2 drapeUV;  // UV remapped to the drape colour target. Usually the
                     // same as `uv`, but during zoom-in it may land inside a
                     // ready parent target while the exact child target warms.
    float elevation;
    float metersPerTile;
    float demScale;
    float viewW; // clip-space w = view distance in world-pixel units (haze)
};

// Decode one Mapbox Terrain-RGB texel to elevation (metres).
static inline float decodeElevation(float4 demSample) {
    float r = demSample.r * 255.0;
    float g = demSample.g * 255.0;
    float b = demSample.b * 255.0;
    return -10000.0 + ((r * 256.0 * 256.0 + g * 256.0 + b) * 0.1);
}

// Bilinearly sample elevation by decoding the four surrounding texels
// separately and interpolating the metres values. The naïve approach of
// using a linear-filter sampler over Terrain-RGB texels produces tens of
// metres of noise per vertex, because the RGB → metres formula is
// non-linear in the byte values and a 0.5% RGB blend can become a 50 m
// elevation error. The fix is to read exact texel values (sample at the
// texel centres) and interpolate the decoded metres directly.
static inline float sampleElevationBilinear(texture2d<float, access::sample> tex,
                                            sampler sam,
                                            float2 uv) {
    (void)sam;
    uint2 textureSize = uint2(tex.get_width(), tex.get_height());
    float2 texSize = float2(textureSize);
    // Convert UV → texel coords offset by half a texel so that texel
    // centres land on integer texel coordinates after subtraction.
    float2 texelCoord = clamp(uv, float2(0.0), float2(1.0)) * texSize - 0.5;
    float2 floorTexel = floor(texelCoord);
    float2 frac = texelCoord - floorTexel;
    int2 maxTexel = int2(int(textureSize.x) - 1, int(textureSize.y) - 1);
    int2 texel00 = clamp(int2(floorTexel), int2(0), maxTexel);
    int2 texel10 = min(texel00 + int2(1, 0), maxTexel);
    int2 texel01 = min(texel00 + int2(0, 1), maxTexel);
    int2 texel11 = min(texel00 + int2(1, 1), maxTexel);
    float e00 = decodeElevation(tex.read(uint2(texel00), 0));
    float e10 = decodeElevation(tex.read(uint2(texel10), 0));
    float e01 = decodeElevation(tex.read(uint2(texel01), 0));
    float e11 = decodeElevation(tex.read(uint2(texel11), 0));
    float e0 = mix(e00, e10, frac.x);
    float e1 = mix(e01, e11, frac.x);
    return mix(e0, e1, frac.y);
}

static inline bool terrainReliefValidElevation(float elevation) {
    // Terrain-RGB no-data commonly arrives as zeroed texels, which decode to
    // -10000 m. Treat those and other out-of-range values as unavailable so
    // fragment lighting does not amplify DEM void edges into visible speckles.
    return elevation > -500.0 && elevation < 9000.0;
}

static inline float terrainReliefSampleOrCenter(float sample, float center) {
    if (!terrainReliefValidElevation(sample)) {
        return center;
    }
    // Real Scandinavian terrain will not jump almost a kilometre across this
    // shader's sampling baseline. Those cliffs are almost always no-data or
    // tile-edge artefacts, so collapse them to the centre sample.
    return abs(sample - center) > 900.0 ? center : sample;
}

static inline float terrainReliefShade(texture2d<float, access::sample> demTexture,
                                       sampler demSampler,
                                       float2 mapUV,
                                       float metersPerTile,
                                       float demScale,
                                       device const TerrainEvaluatedPropsUBO& props) {
    float strength = clamp(props.light_position_intensity.w, 0.0, 1.5);
    if (strength <= 0.001 || metersPerTile <= 1.0) {
        return 1.0;
    }

    float2 texSize = float2(demTexture.get_width(), demTexture.get_height());
    float2 radiusUV = float2(24.0, 24.0) / max(texSize, float2(1.0, 1.0));
    float2 uv = clamp(mapUV, float2(0.0), float2(1.0));

    float eC = sampleElevationBilinear(demTexture, demSampler, uv);
    if (!terrainReliefValidElevation(eC)) {
        return 1.0;
    }
    float eL = sampleElevationBilinear(demTexture, demSampler, clamp(uv - float2(radiusUV.x, 0.0), float2(0.0), float2(1.0)));
    float eR = sampleElevationBilinear(demTexture, demSampler, clamp(uv + float2(radiusUV.x, 0.0), float2(0.0), float2(1.0)));
    float eU = sampleElevationBilinear(demTexture, demSampler, clamp(uv - float2(0.0, radiusUV.y), float2(0.0), float2(1.0)));
    float eD = sampleElevationBilinear(demTexture, demSampler, clamp(uv + float2(0.0, radiusUV.y), float2(0.0), float2(1.0)));
    eL = terrainReliefSampleOrCenter(eL, eC);
    eR = terrainReliefSampleOrCenter(eR, eC);
    eU = terrainReliefSampleOrCenter(eU, eC);
    eD = terrainReliefSampleOrCenter(eD, eC);

    // metersPerTile is already the SOURCE (possibly ancestor) tile's span and
    // radiusUV is in that source texture's UV space, so the sample distance is
    // simply 2*radiusUV*metersPerTile. Dividing by demScale double-counted the
    // zoom gap during DEM parent fallback (slopes shrank 2^dz -> washed-out
    // hillshade that "popped" when the exact DEM arrived).
    float sourceMetersPerTile = metersPerTile;
    float sampleMetersX = max(2.0 * radiusUV.x * sourceMetersPerTile, 1.0);
    float sampleMetersY = max(2.0 * radiusUV.y * sourceMetersPerTile, 1.0);
    float dzdx = clamp((eR - eL) * props.exaggeration / sampleMetersX, -0.75, 0.75);
    float dzdy = clamp((eD - eU) * props.exaggeration / sampleMetersY, -0.75, 0.75);
    float3 normal = normalize(float3(-dzdx, dzdy, 1.0));

    float3 lightDir = normalize(props.light_position_intensity.xyz);
    float neutral = clamp(lightDir.z, 0.25, 0.9);
    float diffuse = dot(normal, lightDir);
    float shade = 1.0 + (diffuse - neutral) * strength;

    // A tiny slope term gives broad faces some body while the invalid-aware,
    // wider DEM radius keeps texel-scale noise out of the final terrain pass.
    float slope = 1.0 - normal.z;
    shade -= slope * strength * 0.06;
    return clamp(shade, 0.78, 1.16);
}

FragmentStage vertex vertexMain(thread const VertexStage vertx [[stage_in]],
                                device const uint32_t& uboIndex [[buffer(idGlobalUBOIndex)]],
                                device const TerrainDrawableUBO* drawableVector [[buffer(idTerrainDrawableUBO)]],
                                device const TerrainEvaluatedPropsUBO& props [[buffer(idTerrainEvaluatedPropsUBO)]],
                                texture2d<float, access::sample> demTexture [[texture(0)]],
                                sampler demSampler [[sampler(0)]]) {

    device const TerrainDrawableUBO& drawable = drawableVector[uboIndex];

    // Convert vertex position to normalized texture coordinates [0, 1]
    // The mesh was generated with coordinates from 0 to EXTENT (8192)
    float2 pos = float2(vertx.pos);
    float2 uv = pos / 8192.0;

    // Skirt sentinel: `generateMesh()` marks perimeter skirt vertices
    // with `texture_pos = (-1, -1)`. Their `pos.xy` matches the mesh
    // edge they hang from, so the DEM/drape sampling produces the
    // same colour and elevation as the edge — the skirt is a vertical
    // wall extending downward from the edge that continues the
    // basemap content under the mesh and hides the z=0 basemap plane
    // at tile-edge crossings.
    bool isSkirt = (vertx.texture_pos.x < 0);

    // Apply the per-drawable DEM UV remap so a tile whose own DEM hasn't
    // streamed in yet can sample the nearest cached ancestor's texture.
    // Identity remap (`scale=1, tl=0`) collapses to the natural uv.
    // Drape colour is sampled from the ideal tile's own target using raw
    // `uv`, so coarse DEM fallback cannot also force coarse map colour.
    float2 mapUV = uv * drawable.dem_scale + drawable.dem_tl;
    float2 drapeUV = uv * drawable.drape_scale + drawable.drape_tl;

    // Centre elevation at this vertex, bilinearly interpolated on the
    // *decoded* values so vertices between DEM texels don't pick up the
    // non-linear RGB-blend noise.
    float debugVertexMode = props.pad2;
    float elevationMeters = sampleElevationBilinear(demTexture, demSampler, mapUV);
    if (debugVertexMode > 2.5 && debugVertexMode < 3.5) {
        elevationMeters = 0.0;
    }
    // elevation_offset (in metres) lifts the entire mesh uniformly. The
    // 2D basemap layers (background, fills, lines, symbols) render at
    // z=0 in the main pass and the terrain mesh's lowest valleys land
    // around there too — they fight for the same depth and the basemap
    // bleeds through the floor of the mesh, reading as the mountains
    // being half-submerged in a flat "ocean" of basemap colour. Lifting
    // the whole mesh by a few hundred metres pulls the lowest valleys
    // above the basemap z=0 plane and the mesh sits cleanly on top.
    if (!terrainReliefValidElevation(elevationMeters)) {
        elevationMeters = 0.0;
    }
    float elevation = elevationMeters * props.exaggeration +
                      props.elevation_offset * drawable.elevation_offset_scale;
    // Drop skirt vertices well below sea level so the perimeter walls
    // reach under the basemap z=0 plane. 5000 m suffices for the entire
    // Sweden coverage's exaggerated mountains.
    if (isSkirt && !(debugVertexMode > 3.5 && debugVertexMode < 4.5)) {
        elevation -= 5000.0;
    }

    float4 position = drawable.matrix * float4(pos.x, pos.y, elevation, 1.0);
    if (debugVertexMode > 1.5 && debugVertexMode < 2.5) {
        position.z = clamp(position.z, 0.0, position.w);
    } else if (debugVertexMode > 4.5 && debugVertexMode < 5.5) {
        // Diagnostic fallback for comparing against the old forced-depth
        // path. Flattening depth lets coarse/fine terrain LODs fight by draw
        // order and was the source of the zoom-in speckled drape artifacts.
        position.z = position.w * (isSkirt ? 0.75 : 0.5);
    }

    return {
        .position  = position,
        .uv        = uv,
        .mapUV     = mapUV,
        .drapeUV   = drapeUV,
        .elevation = elevation,
        .metersPerTile = drawable.meters_per_tile,
        .demScale = drawable.dem_scale,
        .viewW = position.w,
    };
}

// Horizon haze: fade the far field toward the haze tint so the coarse
// distance cover reads as atmosphere instead of low-resolution imagery.
// haze_params.x = start distance, .y = 1/(end-start), both in clip-w
// units (computed CPU-side from km); haze_color.a caps the strength.
static inline float hazeFactor(float viewW, float4 hazeParams, float hazeAlpha) {
    float f = clamp((viewW - hazeParams.x) * hazeParams.y, 0.0, 1.0);
    return f * f * (3.0 - 2.0 * f) * hazeAlpha;
}

static inline bool validDrapeSample(float4 color) {
    return color.a > 0.01 || max(max(color.r, color.g), color.b) > 0.001;
}

half4 fragment fragmentMain(FragmentStage in [[stage_in]],
                            device const TerrainEvaluatedPropsUBO& props [[buffer(idTerrainEvaluatedPropsUBO)]],
                            texture2d<float, access::sample> demTexture [[texture(0)]],
                            sampler demSampler [[sampler(0)]],
                            texture2d<float, access::sample> mapTexture [[texture(1)]],
                            sampler mapSampler [[sampler(1)]]) {
#if defined(OVERDRAW_INSPECTOR)
    return half4(1.0);
#endif
    if (props.pad1 > 0.5 && props.pad1 < 1.5) {
        float3 debugColor = float3(fract(in.mapUV.x * 8.0),
                                   fract(in.mapUV.y * 8.0),
                                   clamp((in.elevation + 1000.0) / 4000.0, 0.0, 1.0));
        return half4(half3(debugColor), 1.0);
    }

    // Sample the drape target for the topo colour, then apply terrain
    // relief in the final mesh shader. Keeping relief out of the drape
    // render target avoids the low-zoom hillshade tiling artifacts while
    // still letting the shaded surface follow the final DEM mesh.
    // Note: Y-coordinate is flipped (1.0 - y) to match OpenGL convention.
    // `drapeUV` is normally the ideal-tile UV, but can temporarily remap
    // into a ready parent drape target while a zoomed-in child target warms.
    // `mapUV` remains the DEM parent-fallback sub-rect and is used only for
    // elevation/relief.
    float2 drapeUV = float2(in.drapeUV.x, 1.0 - in.drapeUV.y);
    float4 mapColor = (props.pad1 > 1.5 && props.pad1 < 2.5)
                          ? mapTexture.sample(mapSampler, drapeUV, level(0.0))
                          : mapTexture.sample(mapSampler, drapeUV);
    if (!validDrapeSample(mapColor)) {
        // .73 physical-device split. `.72` proved that the drawable and CPU
        // binding hold the same texture object, but the affected fragments
        // still sample zero. First force level 0: if implicit derivative LOD
        // is the defect, this recovers the real imagery without a tint. If
        // the exact level-0 sample is also invalid, classify the already-bad
        // pixel without a synchronous GPU readback:
        //   magenta = this UV is blank but the bound texture has usable data;
        //   cyan    = representative level-0 probes are all blank.
        const float4 levelZeroColor = mapTexture.sample(mapSampler, drapeUV, level(0.0));
        if (validDrapeSample(levelZeroColor)) {
            mapColor = levelZeroColor;
        } else {
            const bool boundLevelZeroHasContent =
                validDrapeSample(mapTexture.sample(mapSampler, float2(0.25, 0.25), level(0.0))) ||
                validDrapeSample(mapTexture.sample(mapSampler, float2(0.75, 0.25), level(0.0))) ||
                validDrapeSample(mapTexture.sample(mapSampler, float2(0.50, 0.50), level(0.0))) ||
                validDrapeSample(mapTexture.sample(mapSampler, float2(0.25, 0.75), level(0.0))) ||
                validDrapeSample(mapTexture.sample(mapSampler, float2(0.75, 0.75), level(0.0)));
            return boundLevelZeroHasContent ? half4(1.0h, 0.0h, 1.0h, 1.0h)
                                            : half4(0.0h, 1.0h, 1.0h, 1.0h);
        }
    }
    if (validDrapeSample(mapColor)) {
        // Drape targets are an opaque composited surface for terrain. Some
        // Metal offscreen paths preserve useful RGB while leaving alpha at
        // zero, so do not use alpha to punch holes in the terrain.
        float shade = terrainReliefShade(demTexture, demSampler, in.mapUV, in.metersPerTile, in.demScale, props);
        float3 lit = clamp(mapColor.rgb * shade, float3(0.0), float3(1.0));
        float haze = hazeFactor(in.viewW, props.haze_params, props.haze_color.a);
        return half4(half3(mix(lit, props.haze_color.rgb, haze)), 1.0);
    }

    // No valid drape pixel. Use the Klättra style's base terrain colour
    // instead of the old elevation palette; the elevation fallback turns
    // high ground into white blocks whenever the drape target has small
    // coverage holes while tiles stream or move between fallback parents.
    if (props.pad1 > 2.5 && props.pad1 < 3.5) {
        return half4(1.0h, 0.0h, 1.0h, 1.0h);
    }
    // .50: style-correct fallback (was hardcoded topo paper #F4EAD0 — the
    // paper plates over empty canvas regions in the satellite flyover),
    // hazed like any other far-field surface.
    float fallbackHaze = hazeFactor(in.viewW, props.haze_params, props.haze_color.a);
    return half4(half3(mix(props.fallback_color.rgb, props.haze_color.rgb, fallbackHaze)), 1.0);
}
)";
};

} // namespace shaders
} // namespace mbgl
