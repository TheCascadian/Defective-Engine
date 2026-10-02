#version 330 core
// Blocky far-terrain geometry. Vertex layout is documented in src/far.c.
layout(location = 0) in ivec2 a_xz;     // corner of the 8-block grid inside the tile
layout(location = 1) in int a_height;
layout(location = 2) in vec3 a_color;
layout(location = 3) in int a_flags;    // bits 0..2 face, bit 3 ocean (height is the sea bed)

uniform ivec3 u_cam_base;
uniform vec3 u_cam_frac;
uniform mat4 u_viewproj;
uniform ivec2 u_tile_origin;
uniform float u_height_scale;
uniform float u_cell;
uniform ivec2 u_cover_origin;
uniform float u_sea;
uniform float u_ramp_start;
uniform float u_ramp_end;

out vec3 v_color;
out float v_shade;
out float v_dist;
out vec2 v_cover_pos;

// Same table as chunk.vert so far columns are lit like near faces.
const float FACE_SHADE[6] = float[6](0.80, 0.80, 1.00, 0.55, 0.70, 0.70);

void main() {
    vec2 local = vec2(a_xz) * u_cell;
    float y = float(a_height) * u_height_scale;
    vec2 rel_xz = vec2(float(u_tile_origin.x - u_cam_base.x) + local.x - u_cam_frac.x,
                       float(u_tile_origin.y - u_cam_base.z) + local.y - u_cam_frac.z);
    if ((a_flags & 8) != 0) y = mix(y, u_sea, smoothstep(u_ramp_start, u_ramp_end, length(rel_xz)));
    vec3 rel = vec3(rel_xz.x, y - float(u_cam_base.y) - u_cam_frac.y, rel_xz.y);
    gl_Position = u_viewproj * vec4(rel, 1.0);
    v_color = a_color;
    v_shade = FACE_SHADE[a_flags & 7];
    v_dist = length(rel);
    v_cover_pos = vec2(u_tile_origin - u_cover_origin * 32) + local;
}
