// Generated code, do not modify this file!
#pragma once
#include <mbgl/shaders/shader_source.hpp>

namespace mbgl {
namespace shaders {

template <>
struct ShaderSource<BuiltIn::TerrainShader, gfx::Backend::Type::OpenGL> {
    static constexpr const char* name = "TerrainShader";
    static constexpr const char* vertex = R"(layout (location = 0) in vec2 a_pos;
layout (location = 1) in vec2 a_texture_pos;

layout (std140) uniform TerrainDrawableUBO {
    highp mat4 u_matrix;
};

layout (std140) uniform TerrainEvaluatedPropsUBO {
    highp float u_exaggeration;
    highp float u_elevation_offset;
    highp float u_pad1;
    highp float u_pad2;
    highp vec4 u_light_color_pad;
    highp vec4 u_light_position_intensity;
};

uniform sampler2D u_dem_texture;

out vec2 v_uv;
out float v_elevation;

float decodeElevation(vec4 demSample) {
    float r = demSample.r * 255.0;
    float g = demSample.g * 255.0;
    float b = demSample.b * 255.0;
    return -10000.0 + ((r * 256.0 * 256.0 + g * 256.0 + b) * 0.1);
}

// Bilinear sample of decoded elevation. The naïve linear-filter sample
// produces tens of metres of noise per vertex because the RGB → metres
// formula is non-linear; we read texel centres exactly and interpolate
// the decoded metres directly.
float sampleElevationBilinear(sampler2D tex, vec2 uv) {
    vec2 texSize = vec2(textureSize(tex, 0));
    vec2 texelCoord = uv * texSize - 0.5;
    vec2 floorTexel = floor(texelCoord);
    vec2 frac = texelCoord - floorTexel;
    vec2 uv00 = (floorTexel + vec2(0.5, 0.5)) / texSize;
    vec2 uv10 = (floorTexel + vec2(1.5, 0.5)) / texSize;
    vec2 uv01 = (floorTexel + vec2(0.5, 1.5)) / texSize;
    vec2 uv11 = (floorTexel + vec2(1.5, 1.5)) / texSize;
    float e00 = decodeElevation(texture(tex, uv00));
    float e10 = decodeElevation(texture(tex, uv10));
    float e01 = decodeElevation(texture(tex, uv01));
    float e11 = decodeElevation(texture(tex, uv11));
    float e0 = mix(e00, e10, frac.x);
    float e1 = mix(e01, e11, frac.x);
    return mix(e0, e1, frac.y);
}

void main() {
    vec2 pos = vec2(a_pos);
    vec2 uv = pos / 8192.0;

    // Bilinear-on-decoded centre elevation (avoids RGB-blend noise).
    float elevationMeters = sampleElevationBilinear(u_dem_texture, uv);
    // elevation_offset (in metres) lifts the mesh above z=0 so 2D
    // basemap layers (which render at z=0 in the main pass) don't
    // bleed through the lowest valleys.
    float elevation = elevationMeters * u_exaggeration + u_elevation_offset;

    gl_Position = u_matrix * vec4(pos.x, pos.y, elevation, 1.0);
    v_uv = uv;
    v_elevation = elevation;
}
)";
    static constexpr const char* fragment = R"(in vec2 v_uv;
in float v_elevation;

uniform sampler2D u_map_texture;

layout (std140) uniform TerrainEvaluatedPropsUBO {
    highp float u_exaggeration;
    highp float u_elevation_offset;
    highp float u_pad1;
    highp float u_pad2;
    highp vec4 u_light_color_pad;
    highp vec4 u_light_position_intensity;
};

void main() {
#ifdef OVERDRAW_INSPECTOR
    fragColor = vec4(1.0);
    return;
#endif

    // Sample the drape RenderTarget that holds the basemap content and
    // return it directly. Matches MapLibre GL JS, whose terrain
    // fragment shader is a straight texture lookup with no lighting.
    // Earlier iterations synthesised diffuse lighting from the DEM
    // gradient, but the gradient is high-frequency in the DEM and the
    // projection of that noise shifted with camera motion, producing a
    // swimming-shadow effect that looked worse than no shading at all.
    // Styles that want hillshade can bake it into the drape via a
    // hillshade layer.
    // Note: Y-coordinate is flipped (1.0 - y) to match OpenGL convention.
    vec4 mapColor = texture(u_map_texture, vec2(v_uv.x, 1.0 - v_uv.y));
    if (mapColor.a > 0.01) {
        fragColor = vec4(mapColor.rgb, mapColor.a);
        return;
    }

    // Fallback: elevation-based colour for the no-drape case (no style
    // layer covering the area). Kept for visual orientation; flat-shaded
    // at 0.85 brightness so the fallback can't reintroduce the shading
    // noise the drape path is designed to avoid.
    float normalizedElevation = clamp((v_elevation - 500.0) / 3500.0, 0.0, 1.0);
    vec3 color;
    if (normalizedElevation < 0.33) {
        float t = normalizedElevation / 0.33;
        color = mix(vec3(0.2, 0.4, 0.8), vec3(0.3, 0.7, 0.3), t);
    } else if (normalizedElevation < 0.66) {
        float t = (normalizedElevation - 0.33) / 0.33;
        color = mix(vec3(0.3, 0.7, 0.3), vec3(0.6, 0.5, 0.3), t);
    } else {
        float t = (normalizedElevation - 0.66) / 0.34;
        color = mix(vec3(0.6, 0.5, 0.3), vec3(0.95, 0.95, 0.95), t);
    }

    float gridLine = step(0.98, fract(v_uv.x * 4.0)) + step(0.98, fract(v_uv.y * 4.0));
    color = mix(color, vec3(1.0, 1.0, 1.0), gridLine * 0.5);

    fragColor = vec4(color * 0.85, 1.0);
}
)";
};

} // namespace shaders
} // namespace mbgl
