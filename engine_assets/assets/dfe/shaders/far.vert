#version 330 core
// Heightmap tile. x and z come from gl_VertexID; see src/far.c for the vertex format.
layout(location = 0) in int a_height;
layout(location = 1) in vec3 a_color;
layout(location = 2) in vec2 a_normal_xz;
layout(location = 3) in float a_water;  // 1 for ocean vertices, whose height is the sea bed

uniform ivec3 u_cam_base;
uniform vec3 u_cam_frac;
uniform mat4 u_viewproj;
uniform ivec2 u_tile_origin;
uniform float u_height_scale;
uniform float u_cell;
uniform int u_side;
uniform ivec2 u_cover_origin;
uniform float u_sea;
uniform float u_ramp_start;
uniform float u_ramp_end;

out vec3 v_color;
out vec3 v_normal;
out float v_dist;
out vec2 v_cover_pos;

void main() {
    int gx = gl_VertexID % u_side, gz = gl_VertexID / u_side;
    vec2 local = vec2(gx, gz) * u_cell;
    float y = float(a_height) * u_height_scale;
    vec2 rel_xz = vec2(float(u_tile_origin.x - u_cam_base.x) + local.x - u_cam_frac.x,
                       float(u_tile_origin.y - u_cam_base.z) + local.y - u_cam_frac.z);
    if (a_water > 0.5) y = mix(y, u_sea, smoothstep(u_ramp_start, u_ramp_end, length(rel_xz)));
    vec3 rel = vec3(rel_xz.x, y - float(u_cam_base.y) - u_cam_frac.y, rel_xz.y);
    gl_Position = u_viewproj * vec4(rel, 1.0);
    v_color = a_color;
    float ny = sqrt(max(0.0, 1.0 - dot(a_normal_xz, a_normal_xz)));
    v_normal = vec3(a_normal_xz.x, ny, a_normal_xz.y);
    v_dist = length(rel);
    v_cover_pos = vec2(u_tile_origin - u_cover_origin * 32) + local;
}
