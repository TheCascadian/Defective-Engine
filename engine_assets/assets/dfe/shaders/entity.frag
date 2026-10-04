#version 330 core
in vec3 v_normal;
in vec3 v_rel;
flat in vec3 v_color;
flat in vec4 v_light;                   // sky, r, g, b block light at the entity, 0 to 1

uniform vec3 u_sky_color;
uniform float u_ambient;
uniform vec3 u_fog_color;
uniform float u_fog_start;
uniform float u_fog_end;
uniform vec3 u_light_dir;
uniform float u_light_shade;

out vec4 o_color;

void main() {
    // Same shading recipe as terrain faces, so an entity standing next to a block is lit by the same sun.
    float face = v_normal.y > 0.5 ? 1.0 : (v_normal.y < -0.5 ? 0.55 : 0.8);
    float sunlit = max(dot(normalize(v_normal), u_light_dir), 0.0);
    float shade = face * mix(1.0, 0.8 + 0.35 * sunlit, u_light_shade);
    vec3 light = min(max(v_light.r * u_sky_color + v_light.gba, vec3(u_ambient)), vec3(1.0));
    vec3 rgb = v_color * light * shade;
    rgb = mix(rgb, u_fog_color, smoothstep(u_fog_start, u_fog_end, length(v_rel)));
    o_color = vec4(rgb, 1.0);
}
