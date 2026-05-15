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
    /* 64 */
};
static_assert(sizeof(TerrainDrawableUBO) == 4 * 16, "wrong size");

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
    /*  8 */ float pad1;
    /* 12 */ float pad2;
    /* 16 */ float4 light_color_pad;          // rgb = colour
    /* 32 */ float4 light_position_intensity; // xyz = direction, w = intensity
    /* 48 */
};
static_assert(sizeof(TerrainEvaluatedPropsUBO) == 48, "wrong size");

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
    short2 pos [[attribute(terrainUBOCount + 0)]];
    short2 texture_pos [[attribute(terrainUBOCount + 1)]];
};

struct FragmentStage {
    float4 position [[position, invariant]];
    float2 uv;
    float elevation;
    float3 normal;
};

// Decode one Mapbox Terrain-RGB texel to elevation (metres).
static inline float decodeElevation(float4 demSample) {
    float r = demSample.r * 255.0;
    float g = demSample.g * 255.0;
    float b = demSample.b * 255.0;
    return -10000.0 + ((r * 256.0 * 256.0 + g * 256.0 + b) * 0.1);
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

    // Decode the centre elevation at this vertex.
    float elevationMeters = decodeElevation(demTexture.sample(demSampler, uv));

    // Per-vertex smooth normal. Sample the DEM at four neighbour offsets
    // and decode each independently — Terrain-RGB is non-linear in the
    // byte values so we cannot rely on a linear sampler to interpolate
    // elevations for us. We sample at offsets of `stepTexels` away from
    // the centre (rather than the immediate 4-neighbours) because the
    // single-texel gradient was too noisy and produced visible artifacts
    // on slopes; a 4-texel step averages over more local variation and
    // gives a smoother, more natural-looking hillshade.
    const float2 texSize = float2(demTexture.get_width(), demTexture.get_height());
    const float stepTexels = 4.0;
    const float2 texelStep = float2(stepTexels, stepTexels) / texSize;
    float elevXP = decodeElevation(demTexture.sample(demSampler, uv + float2(texelStep.x, 0.0)));
    float elevXN = decodeElevation(demTexture.sample(demSampler, uv - float2(texelStep.x, 0.0)));
    float elevYP = decodeElevation(demTexture.sample(demSampler, uv + float2(0.0, texelStep.y)));
    float elevYN = decodeElevation(demTexture.sample(demSampler, uv - float2(0.0, texelStep.y)));

    // Apply exaggeration for visible relief (default: 1.0, higher exaggerates).
    float elevation = elevationMeters * props.exaggeration;
    float dE_dx = (elevXP - elevXN) * 0.5 * props.exaggeration;
    float dE_dy = (elevYP - elevYN) * 0.5 * props.exaggeration;

    // Normal. The Z constant trades off shading dramatic-ness vs.
    // flatness: too small → harsh shadows on every slope, too large →
    // looks flat-shaded. 100 looks roughly right at zoom 10-12 (the
    // default Klättra view), where 4 texels of the 256-px DEM tile is
    // ~150 m of ground distance and typical mountain slopes are 20-45°.
    float3 normal = normalize(float3(-dE_dx, -dE_dy, 100.0));

    // Create 3D position with elevation as Z coordinate
    float4 position = drawable.matrix * float4(pos.x, pos.y, elevation, 1.0);

    return {
        .position  = position,
        .uv        = uv,
        .elevation = elevation,
        .normal    = normal,
    };
}

half4 fragment fragmentMain(FragmentStage in [[stage_in]],
                            device const TerrainEvaluatedPropsUBO& props [[buffer(idTerrainEvaluatedPropsUBO)]],
                            texture2d<float, access::sample> mapTexture [[texture(1)]],
                            sampler mapSampler [[sampler(1)]]) {
#if defined(OVERDRAW_INSPECTOR)
    return half4(1.0);
#endif

    // Sample the map texture (render-to-texture output) for the surface color
    // Note: Y-coordinate is flipped (1.0 - y) to match OpenGL convention
    float4 mapColor = mapTexture.sample(mapSampler, float2(in.uv.x, 1.0 - in.uv.y));

    // Smooth per-vertex normal interpolated across the triangle, then
    // re-normalised here because the linear interpolator doesn't preserve
    // unit length. The earlier `dfdx`/`dfdy` approach gave flat shading
    // per triangle and produced visible mesh faceting on slopes.
    float3 normal = normalize(in.normal);
    float3 lightDir = normalize(props.light_position_intensity.xyz);
    float lightIntensity = props.light_position_intensity.w;
    float ambient = 1.0 - lightIntensity;
    float diffuse = ambient + lightIntensity * max(dot(normal, lightDir), 0.0);

    if (mapColor.a > 0.01) {
        float3 lit = mapColor.rgb * diffuse * props.light_color_pad.rgb;
        return half4(half3(lit), half(mapColor.a));
    }

    // Fallback: elevation-based color gradient for debugging
    float elevation = in.elevation;
    float normalizedElevation = clamp((elevation - 500.0) / 3500.0, 0.0, 1.0);

    float3 color;
    if (normalizedElevation < 0.33) {
        float t = normalizedElevation / 0.33;
        color = mix(float3(0.2, 0.4, 0.8), float3(0.3, 0.7, 0.3), t);
    } else if (normalizedElevation < 0.66) {
        float t = (normalizedElevation - 0.33) / 0.33;
        color = mix(float3(0.3, 0.7, 0.3), float3(0.6, 0.5, 0.3), t);
    } else {
        float t = (normalizedElevation - 0.66) / 0.34;
        color = mix(float3(0.6, 0.5, 0.3), float3(0.95, 0.95, 0.95), t);
    }

    float gridLine = step(0.98, fract(in.uv.x * 4.0)) + step(0.98, fract(in.uv.y * 4.0));
    color = mix(color, float3(1.0, 1.0, 1.0), gridLine * 0.5);

    return half4(half3(color * diffuse * props.light_color_pad.rgb), 1.0);
}
)";
};

} // namespace shaders
} // namespace mbgl
