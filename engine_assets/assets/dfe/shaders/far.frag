#version 330 core
in vec3 v_color;
in vec3 v_normal;
in float v_dist;
in vec2 v_cover_pos;

uniform sampler2D u_cover;        // R8, 255 where real chunks are drawn
uniform int u_cover_dim;
uniform vec3 u_sun_dir;
uniform vec3 u_sky_color;
uniform vec3 u_fog_color;
uniform float u_fog_start;
uniform float u_fog_end;

out vec4 o_color;

void main() {
    ivec2 cell = ivec2(floor(v_cover_pos / 32.0));
    if (cell.x >= 0 && cell.y >= 0 && cell.x < u_cover_dim && cell.y < u_cover_dim &&
        texelFetch(u_cover, cell, 0).r > 0.5) discard;
    vec3 n = normalize(v_normal);
    float sun = max(dot(n, normalize(u_sun_dir)), 0.0);
    // Matches the chunk shader's face shading range so the two layers meet without a visible step.
    float shade = 0.55 + 0.45 * sun;
    vec3 rgb = v_color * shade * u_sky_color;
    float fog = smoothstep(u_fog_start, u_fog_end, v_dist);
    o_color = vec4(mix(rgb, u_fog_color, fog), 1.0);
}
