#version 330 core
// Resolve pass: stretches the scene, which may have been drawn at a reduced size into the corner of the target,
// over the window, and optionally adds light shafts.
in vec2 v_uv;

uniform sampler2D u_color;
uniform sampler2D u_depth;
uniform vec2 u_scale;             // share of the target that holds the scene, per axis
uniform vec2 u_texel;             // one target texel in uv units
uniform vec2 u_sun_uv;            // sun position over the window in 0..1
uniform vec3 u_shaft_color;       // sun colour scaled by how strongly shafts show; zero disables them

out vec4 o_color;

#ifdef SHAFTS
const int TAPS = 14;
const float DENSITY = 0.85;       // share of the way to the sun the march covers
const float DECAY = 0.93;

// Only open sky lights the shafts: a tap counts when it landed on the far plane, so terrain and trees cut them.
vec3 shafts(vec2 uv) {
    vec2 step_uv = (u_sun_uv - uv) * (DENSITY / float(TAPS));
    // A little jitter per pixel hides the banding from so few taps.
    float jitter = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
    vec2 p = uv + step_uv * jitter;
    float weight = 1.0, sum = 0.0;
    for (int i = 0; i < TAPS; i++) {
        vec2 q = clamp(p * u_scale, vec2(0.0), u_scale - u_texel * 0.5);
        float sky = step(0.99999, texture(u_depth, q).r);
        sum += sky * weight;
        weight *= DECAY;
        p += step_uv;
    }
    return u_shaft_color * (sum / float(TAPS));
}
#endif

void main() {
    // Clamp half a texel inside the scene area so linear filtering never reads the unused part of the target.
    vec2 q = clamp(v_uv * u_scale, u_texel * 0.5, u_scale - u_texel * 0.5);
    vec3 rgb = texture(u_color, q).rgb;
#ifdef SHAFTS
    if (u_shaft_color.r + u_shaft_color.g + u_shaft_color.b > 0.0) {
        // Screen blend, so bright sky is not pushed past white and the shafts only lift what is dark enough to show them.
        rgb = vec3(1.0) - (vec3(1.0) - rgb) * (vec3(1.0) - shafts(v_uv));
    }
#endif
    o_color = vec4(rgb, 1.0);
}
