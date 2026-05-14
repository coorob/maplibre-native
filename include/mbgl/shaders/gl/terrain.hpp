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

void main() {
    vec2 pos = vec2(a_pos);
    vec2 uv = pos / 8192.0;

    // Sample DEM texture (Mapbox Terrain RGB encoding) and decode the
    // elevation in metres. Same formula as the Metal vertex shader.
    vec4 demSample = texture(u_dem_texture, uv);
    float r = demSample.r * 255.0;
    float g = demSample.g * 255.0;
    float b = demSample.b * 255.0;
    float elevationMeters = -10000.0 + ((r * 256.0 * 256.0 + g * 256.0 + b) * 0.1);

    float elevation = elevationMeters * u_exaggeration;

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

    // Sample the drape RenderTarget that holds the basemap content. Y is
    // flipped to match the texture's Y-up convention against the mesh's
    // y-down tile coords.
    vec4 mapColor = texture(u_map_texture, vec2(v_uv.x, 1.0 - v_uv.y));

    // Reconstruct a surface normal from screen-space derivatives of the
    // elevation varying so the 3D mesh self-shades even when the drape
    // surface is a flat colour.
    float dE_dx = dFdx(v_elevation);
    float dE_dy = dFdy(v_elevation);
    vec3 normal = normalize(vec3(-dE_dx, -dE_dy, 1.0));
    vec3 lightDir = normalize(u_light_position_intensity.xyz);
    float lightIntensity = u_light_position_intensity.w;
    float ambient = 1.0 - lightIntensity;
    float diffuse = ambient + lightIntensity * max(dot(normal, lightDir), 0.0);

    if (mapColor.a > 0.01) {
        vec3 lit = mapColor.rgb * diffuse * u_light_color_pad.rgb;
        fragColor = vec4(lit, mapColor.a);
        return;
    }

    // Fallback: elevation-based colour gradient when the drape is empty.
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

    fragColor = vec4(color * diffuse * u_light_color_pad.rgb, 1.0);
}
)";
};

} // namespace shaders
} // namespace mbgl
