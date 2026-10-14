layout (std140) uniform LineSDFTilePropsUBO {
    highp float u_sdfgamma;
    highp float u_mix;
    lowp float tileprops_pad1;
    lowp float tileprops_pad2;
};

layout (std140) uniform LineEvaluatedPropsUBO {
    highp vec4 u_color;
    lowp float u_blur;
    lowp float u_opacity;
    mediump float u_gapwidth;
    lowp float u_offset;
    mediump float u_width;
    lowp float u_floorwidth;
    lowp float props_pad1;
    highp float u_drape_texels_per_pixel;
};

uniform sampler2D u_image;

in vec2 v_normal;
in vec2 v_width2;
in highp vec2 v_drape_width;
in vec2 v_tex_a;
in vec2 v_tex_b;
in float v_gamma_scale;
in highp vec2 v_tile_pos;
flat in highp float v_terrain_lift;

#pragma mapbox: define highp vec4 color
#pragma mapbox: define lowp float blur
#pragma mapbox: define lowp float opacity
#pragma mapbox: define mediump float width
#pragma mapbox: define lowp float floorwidth

// CDF of a square pixel projected onto the line normal. Its two independent
// uniform spans make a trapezoid (a triangle at 45 degrees). This integrates
// both edges of subtexel strokes instead of merely widening their fade ramp.
float drapePixelCDF(float distance, vec2 spans) {
    float a = spans.x;
    float b = spans.y;
    if (b < 0.0001) return clamp(0.5 + distance / a, 0.0, 1.0);
    float tailDistance = max(0.5 * (a + b) - abs(distance), 0.0);
    float tail = tailDistance < b ? tailDistance * tailDistance / (2.0 * a * b)
                                  : (tailDistance - 0.5 * b) / a;
    return distance > 0.0 ? 1.0 - tail : tail;
}

float drapeIntervalCoverage(float distance, float radius, vec2 spans) {
    return clamp(drapePixelCDF(distance + radius, spans) -
                 drapePixelCDF(distance - radius, spans), 0.0, 1.0);
}

void main() {
    #pragma mapbox: initialize highp vec4 color
    #pragma mapbox: initialize lowp float blur
    #pragma mapbox: initialize lowp float opacity
    #pragma mapbox: initialize mediump float width
    #pragma mapbox: initialize lowp float floorwidth

    // Lifted lines cannot use the flat stencil clip: keep only this tile's own
    // square (the tile buffer duplicates its neighbours) and skip the 4x4
    // cells that a deeper rendered tile covers (u_terrain_lift = 1 + mask).
    if (v_terrain_lift > 0.5) {
        // A 2-unit overlap keeps a pixel on the seam from being dropped by both tiles.
        if (any(lessThan(v_tile_pos, vec2(-2.0))) || any(greaterThan(v_tile_pos, vec2(8194.0)))) {
            discard;
        }
        int coveredCells = int(v_terrain_lift + 0.5) - 1;
        if (coveredCells != 0) {
            ivec2 cell = clamp(ivec2(v_tile_pos / 2048.0), ivec2(0), ivec2(3));
            if (((coveredCells >> (cell.y * 4 + cell.x)) & 1) != 0) {
                discard;
            }
        }
    }

    // Calculate the distance of the pixel from the line in pixels.
    float dist = length(v_normal) * v_width2.s;

    // Calculate the antialiasing fade factor. This is either when fading in
    // the line in case of an offset line (v_width2.t) or when fading out
    // (v_width2.s)
    float blur2 = (blur + 1.0 / DEVICE_PIXEL_RATIO) * v_gamma_scale;
    float alpha = clamp(min(dist - (v_width2.t - blur2), v_width2.s - dist) / blur2, 0.0, 1.0);
    if (u_drape_texels_per_pixel > 0.0) {
        // The FBO is orthographic: use its texel footprint directly, without
        // the main viewport's perspective gamma or device pixel ratio.
        vec2 pixelNormal = vec2(dot(v_normal, dFdx(v_normal)), dot(v_normal, dFdy(v_normal)));
        if (dot(pixelNormal, pixelNormal) < 0.00000001) {
            pixelNormal = vec2(dFdx(v_normal.y), dFdy(v_normal.y));
        }
        pixelNormal = abs(normalize(pixelNormal));
        vec2 spans = vec2(max(pixelNormal.x, pixelNormal.y) + max(blur, 0.0) * u_drape_texels_per_pixel,
                          min(pixelNormal.x, pixelNormal.y));
        float targetDistance = dist * u_drape_texels_per_pixel;
        vec2 targetRadii = v_drape_width * u_drape_texels_per_pixel;
        alpha = clamp(drapeIntervalCoverage(targetDistance, targetRadii.x, spans) -
                      drapeIntervalCoverage(targetDistance, targetRadii.y, spans), 0.0, 1.0);
    }

    float sdfdist_a = texture(u_image, v_tex_a).a;
    float sdfdist_b = texture(u_image, v_tex_b).a;
    float sdfdist = mix(sdfdist_a, sdfdist_b, u_mix);
    alpha *= smoothstep(0.5 - u_sdfgamma / floorwidth, 0.5 + u_sdfgamma / floorwidth, sdfdist);

    fragColor = color * (alpha * opacity);

#ifdef OVERDRAW_INSPECTOR
    fragColor = vec4(1.0);
#endif
}
