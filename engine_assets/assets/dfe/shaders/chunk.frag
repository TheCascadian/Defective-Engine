#version 330 core
in vec2 v_uv;
flat in float v_layer;
in vec4 v_light;
in float v_shade;
in vec3 v_tint;
// Distance and view angle come from the interpolated position, not from interpolated scalars: distant terrain is
// meshed into quads dozens of blocks wide, and a ratio such as y / dist is far from linear across one.
in vec3 v_rel;
in float v_water_depth;

uniform sampler2DArray u_tex;
uniform sampler2D u_anim;         // per layer: frame count, frames per 4 seconds
uniform float u_time;
uniform vec3 u_sky_color;         // sky light colour, already scaled by time of day and weather
uniform float u_ambient;
uniform vec3 u_fog_color;
uniform float u_fog_start;
uniform float u_fog_end;

#ifdef LOD
in vec2 v_cover_pos;
flat in float v_lod_level;
uniform sampler2D u_cover;        // finest ready level per 32-block column, 0 for real chunks, 255 for none
uniform int u_cover_dim;
uniform ivec2 u_cover_origin;
#endif

out vec4 o_color;

void main() {
    float dist = length(v_rel);
#ifdef LOD
    ivec2 col = ivec2(floor(v_cover_pos / 32.0)) - u_cover_origin;
    if (all(greaterThanEqual(col, ivec2(0))) && all(lessThan(col, ivec2(u_cover_dim)))) {
        float finest = texelFetch(u_cover, col, 0).r * 255.0;
        if (finest < v_lod_level - 0.5) discard;
    }
#endif
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
    vec3 albedo = tex.rgb * v_tint;
    if (v_water_depth >= 0.0) {
        // Water colour comes from the tint alone, which holds the final colour and matches the far terrain
        // constants; the texture only adds a little ripple so the surface is not a flat sheet.
        float ripple = dot(tex.rgb, vec3(0.3333)) - 0.55;
        albedo = v_tint * (0.9 + 0.5 * ripple);
    }
    vec3 rgb = albedo * light * v_shade;
    if (v_water_depth >= 0.0) {
        // Schlick-style reflection: water seen at a grazing angle mirrors the sky, which keeps a wide ocean from
        // reading as a black sheet and fades it into the horizon haze instead of ending at a hard edge.
        float fresnel = 0.04 + 0.96 * pow(1.0 - clamp(abs(v_rel.y) / max(dist, 0.001), 0.0, 1.0), 5.0);
        rgb = mix(rgb, u_fog_color * max(u_sky_color.g, 0.15), min(fresnel * 1.2, 0.85));
    }
    float fog = smoothstep(u_fog_start, u_fog_end, dist);
    rgb = mix(rgb, u_fog_color, fog);
#ifdef PASS_TRANSLUCENT
    // Shallow water shows the bed through it; deep water is fully opaque. Any bed showing through deep water makes
    // the real chunks (lit bed) and the far tiles (differently lit bed) disagree, which draws their boundary as
    // rectangles, and an unlit bed turns a wide ocean black. The texture alpha is ignored for water for the same
    // reason: it would leave a fixed share of the bed visible at every depth.
    float alpha = v_water_depth < 0.0 ? 0.72 : mix(0.55, 1.0, v_water_depth);
    o_color = vec4(rgb, v_water_depth < 0.0 ? tex.a * alpha : alpha);
#else
    o_color = vec4(rgb, 1.0);
#endif
}
