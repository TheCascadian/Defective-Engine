// Shadow lookup for the chunk shaders. Override this file in a mod (assets/dfe/shaders/shadow.glsl) to change how
// shadows look: soften or harden them, tint them, fade them differently with distance.
//
// Inputs the engine provides: the cascade matrices and settings below. The function must return 1.0 for a fully lit
// point and 0.0 for a fully shadowed one; chunk.frag scales the direct light by u_shadow_strength.

uniform sampler2DArrayShadow u_shadow_map;
uniform mat4 u_shadow_vp[3];      // camera-relative position to light clip space, one per cascade
uniform vec4 u_shadow_info[3];    // x: texel size in blocks, y: depth span in blocks
uniform int u_shadow_count;       // 0 when shadows are off
uniform int u_shadow_taps;        // soft-edge samples, 1 to 16
uniform float u_shadow_strength;  // 0 to 1, how dark full shadow is; already reduced at dusk and under cloud
uniform float u_shadow_softness;  // edge blur radius in texels
uniform float u_shadow_bias;      // depth offset in blocks

const vec2 SHADOW_DISK[16] = vec2[16](
    vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725), vec2(-0.09418410, -0.92938870), vec2(0.34495938, 0.29387760),
    vec2(-0.91588581, 0.45771432), vec2(-0.81544232, -0.87912464), vec2(-0.38277543, 0.27676845), vec2(0.97484398, 0.75648379),
    vec2(0.44323325, -0.97511554), vec2(0.53742981, -0.47373420), vec2(-0.26496911, -0.41893023), vec2(0.79197514, 0.19090188),
    vec2(-0.24188840, 0.99706507), vec2(-0.81409955, 0.91437590), vec2(0.19984126, 0.78641367), vec2(0.14383161, -0.14100790));

// rel: camera-relative position, normal: face normal, light_dir: unit vector towards the light.
float shadow_visibility(vec3 rel, vec3 normal, vec3 light_dir) {
    if (u_shadow_count == 0 || u_shadow_strength <= 0.0) return 1.0;
    float ndl = dot(normal, light_dir);
    if (ndl <= 0.02) return 1.0; // faces turned away are already shaded by the face shade
    for (int i = 0; i < u_shadow_count; i++) {
        vec3 p = rel + normal * (u_shadow_info[i].x * 1.5 + 0.02);
        vec4 c = u_shadow_vp[i] * vec4(p, 1.0);
        float edge = max(abs(c.x), abs(c.y));
        bool last = i == u_shadow_count - 1;
        if (edge >= 0.92 && !last) continue;
        if (edge >= 1.0) return 1.0;
        vec3 q = c.xyz * 0.5 + 0.5;
        float bias = (u_shadow_bias + u_shadow_info[i].x * (1.0 - ndl) * 1.5) / u_shadow_info[i].y;
        float lit;
        if (u_shadow_taps <= 1) {
            lit = texture(u_shadow_map, vec4(q.xy, float(i), q.z - bias));
        } else {
            float a = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
            mat2 rot = mat2(cos(a), sin(a), -sin(a), cos(a));
            vec2 step_uv = u_shadow_softness / vec2(textureSize(u_shadow_map, 0).xy);
            lit = 0.0;
            for (int k = 0; k < 16; k++) {
                if (k >= u_shadow_taps) break;
                lit += texture(u_shadow_map, vec4(q.xy + rot * SHADOW_DISK[k] * step_uv, float(i), q.z - bias));
            }
            lit /= float(min(u_shadow_taps, 16));
        }
        float fade = last ? 1.0 - smoothstep(0.8, 1.0, edge) : 1.0;
        return mix(1.0, lit, fade);
    }
    return 1.0;
}
