#version 330 core
// Vertex layout is documented in src/mesher.c. Positions are camera relative so precision holds far from the origin.
layout(location = 0) in uint a_a;
layout(location = 1) in uint a_b;

uniform isamplerBuffer u_origins; // one ivec4 per 256-vertex granule: chunk origin in blocks
uniform ivec3 u_cam_base;
uniform vec3 u_cam_frac;
uniform mat4 u_viewproj;          // projection * rotation-only view
uniform float u_time;
uniform vec3 u_tint[4];           // none, grass, foliage, water shallow
uniform vec3 u_water_deep;
uniform vec3 u_light_dir;         // dominant light, sun or moon
uniform float u_light_shade;      // how strongly the light direction modulates face shade, 0 flat

out vec2 v_uv;
flat out float v_layer;
out vec4 v_light;                  // sky, r, g, b in 0..1
out float v_shade;                 // directional shade times ambient occlusion
out vec3 v_tint;
out vec3 v_rel;                    // camera relative position; distance and view angle are derived per fragment
out vec3 v_normal;                 // face normal, for shadow bias
out float v_water_depth;           // 0 for everything but water, else the depth bucket in 0..1
#ifdef LOD
uniform float u_sea;               // sea level in blocks; only the coarse sea surface is clamped
out vec2 v_cover_pos;              // world x, z in blocks for the coverage lookup
flat out float v_lod_level;
#endif

// Face normals in the mesher's face order: +X, -X, +Y, -Y, +Z, -Z; the two plant faces use up.
const vec3 FACE_NORMAL[8] = vec3[8](vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 1, 0), vec3(0, -1, 0), vec3(0, 0, 1), vec3(0, 0, -1), vec3(0, 1, 0), vec3(0, 1, 0));
const float FACE_SHADE[8] = float[8](0.80, 0.80, 1.00, 0.55, 0.70, 0.70, 0.90, 0.90);
const float AO_CURVE[4] = float[4](0.50, 0.68, 0.84, 1.00);

vec2 face_uv(uint face, vec3 p) {
    switch (face) {
    case 0u: return vec2(-p.z, -p.y);
    case 1u: return vec2(p.z, -p.y);
    case 2u: return vec2(p.x, p.z);
    case 3u: return vec2(p.x, -p.z);
    case 4u: return vec2(p.x, -p.y);
    default: return vec2(-p.x, -p.y);
    }
}

void main() {
    ivec3 ip = ivec3(int(a_a & 1023u), int((a_a >> 10) & 1023u), int((a_a >> 20) & 1023u));
    uint extra = a_a >> 30;
    ivec4 origin = texelFetch(u_origins, gl_VertexID >> 8);
#ifdef LOD
    vec3 local = vec3(ip) * (float(1 << origin.w) / 16.0);
#else
    vec3 local = vec3(ip) * (1.0 / 16.0);
#endif
    vec3 world_rel = vec3(origin.xyz - u_cam_base) + local - u_cam_frac;

    uint face = (a_b >> 10) & 7u;
    uint tint = (a_b >> 29) & 3u;
    bool wind = (a_b >> 31) != 0u;
    if (wind) {
        float weight = face >= 6u ? float(extra) / 3.0 : 1.0;
        vec3 wp = world_rel + vec3(u_cam_base) + u_cam_frac;
        float t = u_time * 1.7 + wp.x * 0.6 + wp.z * 0.45;
        world_rel.xz += vec2(sin(t), cos(t * 0.8)) * (0.045 * weight);
    }
#ifdef LOD
    if (((a_b >> 29) & 3u) == 3u) {
        float scale = float(1 << origin.w);
        float wy = world_rel.y + float(u_cam_base.y) + u_cam_frac.y;
        float top_voxel = floor((wy + 0.001) / scale) - 1.0;
        if (top_voxel == floor(u_sea / scale)) world_rel.y -= wy - (u_sea + 1.0);
    }
    v_cover_pos = world_rel.xz + vec2(u_cam_base.xz) + u_cam_frac.xz;
    v_lod_level = float(origin.w);
#endif
    gl_Position = u_viewproj * vec4(world_rel, 1.0);

    if (face >= 6u) {
        int k = gl_VertexID & 3;
        v_uv = vec2(float(((k + 1) >> 1) & 1), 1.0 - float((k >> 1) & 1));
    } else {
        v_uv = face_uv(face, local);
    }
    v_layer = float(a_b & 1023u);
    v_light = vec4(float((a_b >> 13) & 15u), float((a_b >> 17) & 15u), float((a_b >> 21) & 15u), float((a_b >> 25) & 15u)) * (1.0 / 15.0);
    // Water carries its depth bucket in the extra bits instead of ambient occlusion, so it takes no AO shade.
    float ao = (face >= 6u || tint == 3u) ? 1.0 : AO_CURVE[extra];
    // The fixed per-face shade is the ambient look; the light direction scales it so lit faces change with the sun.
    float sunlit = max(dot(FACE_NORMAL[face], u_light_dir), 0.0);
    v_shade = FACE_SHADE[face] * mix(1.0, 0.8 + 0.35 * sunlit, u_light_shade) * ao;
    v_tint = tint == 3u ? mix(u_tint[3], u_water_deep, float(extra) / 3.0) : u_tint[tint];
    v_water_depth = tint == 3u ? float(extra) / 3.0 : -1.0;
    v_normal = FACE_NORMAL[face];
    v_rel = world_rel;
}
