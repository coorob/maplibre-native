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
    highp vec2 u_dem_tl;
    highp float u_dem_scale;
    highp float u_meters_per_tile;
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
out vec2 v_mapUV;
out float v_elevation;
out float v_meters_per_tile;
out float v_dem_scale;

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
    ivec2 textureSize2D = textureSize(tex, 0);
    vec2 texSize = vec2(textureSize2D);
    vec2 texelCoord = clamp(uv, vec2(0.0), vec2(1.0)) * texSize - 0.5;
    vec2 floorTexel = floor(texelCoord);
    vec2 frac = texelCoord - floorTexel;
    ivec2 maxTexel = textureSize2D - ivec2(1);
    ivec2 texel00 = clamp(ivec2(floorTexel), ivec2(0), maxTexel);
    ivec2 texel10 = min(texel00 + ivec2(1, 0), maxTexel);
    ivec2 texel01 = min(texel00 + ivec2(0, 1), maxTexel);
    ivec2 texel11 = min(texel00 + ivec2(1, 1), maxTexel);
    float e00 = decodeElevation(texelFetch(tex, texel00, 0));
    float e10 = decodeElevation(texelFetch(tex, texel10, 0));
    float e01 = decodeElevation(texelFetch(tex, texel01, 0));
    float e11 = decodeElevation(texelFetch(tex, texel11, 0));
    float e0 = mix(e00, e10, frac.x);
    float e1 = mix(e01, e11, frac.x);
    return mix(e0, e1, frac.y);
}

void main() {
    vec2 pos = vec2(a_pos);
    vec2 uv = pos / 8192.0;

    // Skirt sentinel: perimeter skirt vertices carry texture_pos = (-1,
    // -1). Their pos matches the mesh edge they hang from; the only
    // difference is the elevation gets dropped (see below) so the skirt
    // forms a vertical wall continuing the basemap content underneath.
    bool isSkirt = (a_texture_pos.x < 0.0);

    // Apply per-drawable DEM UV remap so a tile whose own DEM is still
    // streaming can sample the nearest cached ancestor's texture. Identity
    // remap (`scale=1, tl=0`) collapses to the natural uv. Drape colour is
    // sampled from the ideal tile's own target using raw `uv`, so coarse
    // DEM fallback cannot also force coarse map colour.
    vec2 mapUV = uv * u_dem_scale + u_dem_tl;

    // Bilinear-on-decoded centre elevation (avoids RGB-blend noise).
    float elevationMeters = sampleElevationBilinear(u_dem_texture, mapUV);
    // elevation_offset (in metres) lifts the mesh above z=0 so 2D
    // basemap layers (which render at z=0 in the main pass) don't
    // bleed through the lowest valleys.
    float elevation = elevationMeters * u_exaggeration + u_elevation_offset;
    // Drop skirt vertices well below sea level so the perimeter walls
    // reach under the basemap z=0 plane.
    if (isSkirt) {
        elevation -= 5000.0;
    }

    gl_Position = u_matrix * vec4(pos.x, pos.y, elevation, 1.0);
    v_uv = uv;
    v_mapUV = mapUV;
    v_elevation = elevation;
    v_meters_per_tile = u_meters_per_tile;
    v_dem_scale = u_dem_scale;
}
)";
    static constexpr const char* fragment = R"(in vec2 v_uv;
in vec2 v_mapUV;
in float v_elevation;
in float v_meters_per_tile;
in float v_dem_scale;

uniform sampler2D u_dem_texture;
uniform sampler2D u_map_texture;

layout (std140) uniform TerrainEvaluatedPropsUBO {
    highp float u_exaggeration;
    highp float u_elevation_offset;
    highp float u_pad1;
    highp float u_pad2;
    highp vec4 u_light_color_pad;
    highp vec4 u_light_position_intensity;
};

float decodeElevation(vec4 demSample) {
    float r = demSample.r * 255.0;
    float g = demSample.g * 255.0;
    float b = demSample.b * 255.0;
    return -10000.0 + ((r * 256.0 * 256.0 + g * 256.0 + b) * 0.1);
}

float sampleElevationBilinear(sampler2D tex, vec2 uv) {
    ivec2 textureSize2D = textureSize(tex, 0);
    vec2 texSize = vec2(textureSize2D);
    vec2 texelCoord = clamp(uv, vec2(0.0), vec2(1.0)) * texSize - 0.5;
    vec2 floorTexel = floor(texelCoord);
    vec2 frac = texelCoord - floorTexel;
    ivec2 maxTexel = textureSize2D - ivec2(1);
    ivec2 texel00 = clamp(ivec2(floorTexel), ivec2(0), maxTexel);
    ivec2 texel10 = min(texel00 + ivec2(1, 0), maxTexel);
    ivec2 texel01 = min(texel00 + ivec2(0, 1), maxTexel);
    ivec2 texel11 = min(texel00 + ivec2(1, 1), maxTexel);
    float e00 = decodeElevation(texelFetch(tex, texel00, 0));
    float e10 = decodeElevation(texelFetch(tex, texel10, 0));
    float e01 = decodeElevation(texelFetch(tex, texel01, 0));
    float e11 = decodeElevation(texelFetch(tex, texel11, 0));
    float e0 = mix(e00, e10, frac.x);
    float e1 = mix(e01, e11, frac.x);
    return mix(e0, e1, frac.y);
}

bool terrainReliefValidElevation(float elevation) {
    return elevation > -500.0 && elevation < 9000.0;
}

float terrainReliefSampleOrCenter(float sample, float center) {
    if (!terrainReliefValidElevation(sample)) {
        return center;
    }
    return abs(sample - center) > 900.0 ? center : sample;
}

float terrainReliefShade(sampler2D demTexture, vec2 mapUV, float metersPerTile) {
    float strength = clamp(u_light_position_intensity.w, 0.0, 1.5);
    if (strength <= 0.001 || metersPerTile <= 1.0) {
        return 1.0;
    }

    vec2 texSize = vec2(textureSize(demTexture, 0));
    vec2 radiusUV = vec2(24.0) / max(texSize, vec2(1.0));
    vec2 uv = clamp(mapUV, vec2(0.0), vec2(1.0));

    float eC = sampleElevationBilinear(demTexture, uv);
    if (!terrainReliefValidElevation(eC)) {
        return 1.0;
    }
    float eL = sampleElevationBilinear(demTexture, clamp(uv - vec2(radiusUV.x, 0.0), vec2(0.0), vec2(1.0)));
    float eR = sampleElevationBilinear(demTexture, clamp(uv + vec2(radiusUV.x, 0.0), vec2(0.0), vec2(1.0)));
    float eU = sampleElevationBilinear(demTexture, clamp(uv - vec2(0.0, radiusUV.y), vec2(0.0), vec2(1.0)));
    float eD = sampleElevationBilinear(demTexture, clamp(uv + vec2(0.0, radiusUV.y), vec2(0.0), vec2(1.0)));
    eL = terrainReliefSampleOrCenter(eL, eC);
    eR = terrainReliefSampleOrCenter(eR, eC);
    eU = terrainReliefSampleOrCenter(eU, eC);
    eD = terrainReliefSampleOrCenter(eD, eC);

    float sourceMetersPerTile = metersPerTile / max(v_dem_scale, 0.0001);
    float sampleMetersX = max(2.0 * radiusUV.x * sourceMetersPerTile, 1.0);
    float sampleMetersY = max(2.0 * radiusUV.y * sourceMetersPerTile, 1.0);
    float dzdx = clamp((eR - eL) * u_exaggeration / sampleMetersX, -0.75, 0.75);
    float dzdy = clamp((eD - eU) * u_exaggeration / sampleMetersY, -0.75, 0.75);
    vec3 normal = normalize(vec3(-dzdx, dzdy, 1.0));

    vec3 lightDir = normalize(u_light_position_intensity.xyz);
    float neutral = clamp(lightDir.z, 0.25, 0.9);
    float diffuse = dot(normal, lightDir);
    float shade = 1.0 + (diffuse - neutral) * strength;
    float slope = 1.0 - normal.z;
    shade -= slope * strength * 0.06;
    return clamp(shade, 0.78, 1.16);
}

void main() {
#ifdef OVERDRAW_INSPECTOR
    fragColor = vec4(1.0);
    return;
#endif

    // Sample the drape target for topo colour, then apply DEM-derived
    // relief in the final terrain pass instead of baking hillshade into
    // the drape texture.
    // Note: Y-coordinate is flipped (1.0 - y) to match OpenGL convention.
    // Drape targets are keyed by the ideal tile, so sample them with raw
    // ideal-tile UVs. `v_mapUV` remains the DEM parent-fallback sub-rect
    // and is used only for elevation/relief.
    vec4 mapColor = texture(u_map_texture, vec2(v_uv.x, 1.0 - v_uv.y));
    if (mapColor.a > 0.01 || max(max(mapColor.r, mapColor.g), mapColor.b) > 0.001) {
        float shade = terrainReliefShade(u_dem_texture, v_mapUV, v_meters_per_tile);
        fragColor = vec4(clamp(mapColor.rgb * shade, vec3(0.0), vec3(1.0)), 1.0);
        return;
    }

    // No valid drape pixel. Use the Klättra style's base terrain colour
    // instead of the old elevation palette; the elevation fallback turns
    // high ground into white blocks whenever the drape target has small
    // coverage holes while tiles stream or move between fallback parents.
    if (u_pad1 > 2.5 && u_pad1 < 3.5) {
        fragColor = vec4(1.0, 0.0, 1.0, 1.0);
        return;
    }
    fragColor = vec4(0.95686275, 0.91764706, 0.81568627, 1.0);
}
)";
};

} // namespace shaders
} // namespace mbgl
