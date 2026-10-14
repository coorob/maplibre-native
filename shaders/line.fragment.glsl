layout (std140) uniform LineEvaluatedPropsUBO {
    highp vec4 u_color;
    lowp float u_blur;
    lowp float u_opacity;
    mediump float u_gapwidth;
    lowp float u_offset;
    mediump float u_width;
    lowp float u_floorwidth;
    lowp float props_pad1;
    lowp float props_pad2;
};

in vec2 v_width2;
in vec2 v_normal;
in float v_gamma_scale;
in highp vec2 v_tile_pos;
flat in highp float v_terrain_lift;

#pragma mapbox: define highp vec4 color
#pragma mapbox: define lowp float blur
#pragma mapbox: define lowp float opacity

void main() {
    #pragma mapbox: initialize highp vec4 color
    #pragma mapbox: initialize lowp float blur
    #pragma mapbox: initialize lowp float opacity

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

    fragColor = color * (alpha * opacity);

#ifdef OVERDRAW_INSPECTOR
    fragColor = vec4(1.0);
#endif
}
