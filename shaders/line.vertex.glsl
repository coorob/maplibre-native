// floor(127 / 2) == 63.0
// the maximum allowed miter limit is 2.0 at the moment. the extrude normal is
// stored in a byte (-128..127). we scale regular normals up to length 63, but
// there are also "special" normals that have a bigger length (of up to 126 in
// this case).
// #define scale 63.0
#define scale 0.015873016

layout (location = 0) in vec2 a_pos_normal;
layout (location = 1) in vec4 a_data;
// Raw DEM metres at the centreline; only read for terrain-lifted drawables.
layout (location = 2) in float a_terrain_elevation;

layout (std140) uniform GlobalPaintParamsUBO {
    highp vec2 u_pattern_atlas_texsize;
    highp vec2 u_units_to_pixels;
    highp vec2 u_world_size;
    highp float u_camera_to_center_distance;
    highp float u_symbol_fade_change;
    highp float u_aspect_ratio;
    highp float u_pixel_ratio;
    highp float u_map_zoom;
    lowp float global_pad1;
};

layout (std140) uniform LineDrawableUBO {
    highp mat4 u_matrix;
    mediump float u_ratio;
    // Interpolations
    lowp float u_color_t;
    lowp float u_blur_t;
    lowp float u_opacity_t;
    lowp float u_gapwidth_t;
    lowp float u_offset_t;
    lowp float u_width_t;
    // 1 when this drawable is drawn as 3D geometry on the terrain (lifted line).
    highp float u_terrain_lift;
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

out vec2 v_normal;
// Lifted lines clip to their own tile (see the fragment shader).
out highp vec2 v_tile_pos;
flat out highp float v_terrain_lift;
out vec2 v_width2;
// Unpadded outer/inner radii, in camera pixels.
out highp vec2 v_drape_width;
out float v_gamma_scale;
out highp float v_linesofar;

#pragma mapbox: define highp vec4 color
#pragma mapbox: define lowp float blur
#pragma mapbox: define lowp float opacity
#pragma mapbox: define mediump float gapwidth
#pragma mapbox: define lowp float offset
#pragma mapbox: define mediump float width

void main() {
    #pragma mapbox: initialize highp vec4 color
    #pragma mapbox: initialize lowp float blur
    #pragma mapbox: initialize lowp float opacity
    #pragma mapbox: initialize mediump float gapwidth
    #pragma mapbox: initialize lowp float offset
    #pragma mapbox: initialize mediump float width

    // the distance over which the line edge fades out.
    // Retina devices need a smaller distance to avoid aliasing.
    bool draped = u_drape_texels_per_pixel > 0.0;
    float ANTIALIASING = draped
        ? 0.70710678118 / u_drape_texels_per_pixel + 0.5 * max(blur, 0.0)
        : 1.0 / DEVICE_PIXEL_RATIO / 2.0;

    vec2 a_extrude = a_data.xy - 128.0;
    float a_direction = mod(a_data.z, 4.0) - 1.0;

    v_linesofar = (floor(a_data.z / 4.0) + a_data.w * 64.0) * 2.0;

    vec2 pos = floor(a_pos_normal * 0.5);

    // x is 1 if it's a round cap, 0 otherwise
    // y is 1 if the normal points up, and -1 if it points down
    // We store these in the least significant bit of a_pos_normal
    mediump vec2 normal = a_pos_normal - 2.0 * pos;
    normal.y = normal.y * 2.0 - 1.0;
    v_normal = normal;

    // these transformations used to be applied in the JS and native code bases.
    // moved them into the shader for clarity and simplicity.
    gapwidth = gapwidth / 2.0;
    float halfwidth = width / 2.0;
    offset = -1.0 * offset;

    // Half a texel diagonal covers the pixel footprint at every line angle.
    // The larger drape skirt lets subtexel strokes reach raster samples.
    // Save the original stroke bounds so filtering cannot widen the stroke.
    v_drape_width = vec2(gapwidth + halfwidth * (gapwidth > 0.0 ? 2.0 : 1.0), gapwidth);

    float inset = gapwidth + (gapwidth > 0.0 ? ANTIALIASING : 0.0);
    float outset = gapwidth + halfwidth * (gapwidth > 0.0 ? 2.0 : 1.0) + (halfwidth == 0.0 ? 0.0 : ANTIALIASING);

    // Scale the extrusion vector down to a normal and then up by the line width
    // of this vertex.
    mediump vec2 dist = outset * a_extrude * scale;

    // Calculate the offset when drawing a line that is to the side of the actual line.
    // We do this by creating a vector that points towards the extrude, but rotate
    // it when we're drawing round end points (a_direction = -1 or 1) since their
    // extrude vector points in another direction.
    mediump float u = 0.5 * a_direction;
    mediump float t = 1.0 - abs(u);
    mediump vec2 offset2 = offset * a_extrude * scale * normal.y * mat2(t, -u, u, t);

    // Lifted lines: u_matrix maps raw DEM metres to the terrain mesh's render
    // height (exaggeration, origin and a depth pull are folded in on the CPU).
    bool lifted = u_terrain_lift > 0.5;
    v_terrain_lift = u_terrain_lift;
    v_tile_pos = pos + offset2 / u_ratio + dist / u_ratio;
    float elevation = lifted ? a_terrain_elevation : 0.0;
    vec4 projected_extrude = u_matrix * vec4(dist / u_ratio, 0.0, 0.0);
    gl_Position = u_matrix * vec4(pos + offset2 / u_ratio, elevation, 1.0) + projected_extrude;
    if (lifted) {
        // Give the whole ribbon its centreline's depth. A flat ribbon on a side
        // slope would otherwise sink half its width into the hill.
        vec4 centre = u_matrix * vec4(pos, elevation, 1.0);
        if (centre.w > 0.0) {
            gl_Position.z = centre.z / centre.w * gl_Position.w;
        }
    }

    // calculate how much the perspective view squishes or stretches the extrude
    float extrude_length_without_perspective = length(dist);
    float extrude_length_with_perspective = length(projected_extrude.xy / gl_Position.w * u_units_to_pixels);
    v_gamma_scale = extrude_length_without_perspective / extrude_length_with_perspective;

    v_width2 = vec2(outset, inset);
}
