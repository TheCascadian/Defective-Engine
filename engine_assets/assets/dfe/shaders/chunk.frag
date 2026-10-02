#version 330 core
in vec2 v_uv;
flat in float v_layer;
in vec4 v_light;
in float v_shade;
in vec3 v_tint;
in float v_dist;

uniform sampler2DArray u_tex;
uniform sampler2D u_anim;         // per layer: frame count, frames per 4 seconds
uniform float u_time;
uniform vec3 u_sky_color;         // sky light colour, already scaled by time of day and weather
uniform float u_ambient;
uniform vec3 u_fog_color;
uniform float u_fog_start;
uniform float u_fog_end;

out vec4 o_color;

void main() {
    float layer = v_layer;
    vec2 anim = texelFetch(u_anim, ivec2(int(layer), 0), 0).rg * 255.0;
    if (anim.x > 1.0) layer += mod(floor(u_time * anim.y * 0.25), anim.x);
    vec4 tex = texture(u_tex, vec3(v_uv, layer));
#ifdef PASS_CUTOUT
    if (tex.a < 0.5) discard;
#endif
    vec3 light = v_light.r * u_sky_color + v_light.gba;
    light = max(light, vec3(u_ambient));
    light = min(light, vec3(1.0));
    vec3 rgb = tex.rgb * v_tint * light * v_shade;
    float fog = smoothstep(u_fog_start, u_fog_end, v_dist);
    rgb = mix(rgb, u_fog_color, fog);
#ifdef PASS_TRANSLUCENT
    o_color = vec4(rgb, tex.a * 0.72);
#else
    o_color = vec4(rgb, 1.0);
#endif
}
