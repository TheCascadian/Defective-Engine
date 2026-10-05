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
in vec3 v_normal;
// Ruling: PBR runs in the near opaque pass only. Cutout (leaves, plants) and translucent (water, glass) keep the
// flat model, and the far LOD tiles are too coarse for texel relief; this keeps those passes' cost unchanged.
#if defined(PASS_OPAQUE) && !defined(LOD) && !defined(SHADOW)
#define DFE_PBR 1
#endif

uniform sampler2DArray u_tex;
uniform sampler2D u_anim;         // per layer: frame count, frames per 4 seconds, PBR bump (0 = none), metalness
uniform sampler2DArray u_tex_normal; // tangent-space normals, same layers as u_tex
uniform sampler2DArray u_tex_rh;  // roughness, height
uniform float u_bump_strength;    // global multiplier on the per-layer height bump
// PBR tuning, all from data/dfe/pbr.json (see docs/MODDING.md).
uniform vec4 u_pbr_a = vec4(0.0625, 1.0, 16.0, 32.0); // relief depth in blocks, normal-map strength, fade start, fade end
uniform vec4 u_pbr_b = vec4(1.0, 1.0, 0.0, 1.0);      // specular strength, roughness scale, roughness bias, metalness scale
uniform vec4 u_pbr_c = vec4(1.0, 1.0, 0.5, 0.0);      // diffuse response, sky lean, cavity AO, shade floor
uniform vec4 u_pbr_d = vec4(0.6, 1.0, 0.35, 4.0);     // self-shadow strength, self-shadow reach, sky specular, shadow tap cap
uniform vec4 u_pbr_e = vec4(0.07, 0.8, 0.3, 0.6);     // parallax depth, texel bevel, texel bevel width, block bevel
uniform vec4 u_pbr_f = vec4(0.12, 0.25, 0.35, 1.0);   // block bevel width, texel outline, block outline, colour relief
uniform vec2 u_pbr_g = vec2(1.15, 1.1);               // colour contrast, saturation
uniform int u_pbr_pom = 14;                           // parallax steps, 0 = off
uniform int u_pbr_steps = 6;
uniform int u_pbr_on = 1;                             // 0 turns every PBR effect off                          // self-shadow ray steps, 0 = off
uniform float u_light_shade;
uniform float u_time;
uniform vec3 u_sky_color;         // sky light colour, already scaled by time of day and weather
uniform float u_ambient;
uniform vec3 u_fog_color;
uniform float u_fog_start;
uniform float u_fog_end;
uniform float u_near_fog_density;
uniform float u_near_fog_start;
uniform float u_near_fog_end;
uniform vec3 u_light_dir;
uniform vec3 u_glint;             // colour and strength of the light that glints on water
uniform ivec3 u_cam_base;
uniform vec3 u_cam_frac;

#ifdef LOD
in vec2 v_cover_pos;
flat in float v_lod_level;
uniform sampler2D u_cover;        // finest ready level per 32-block column, 0 for real chunks, 255 for none
uniform int u_cover_dim;
uniform ivec2 u_cover_origin;
#endif

#include "assets/dfe/shaders/shadow.glsl"

out vec4 o_color;

vec3 fresnel_schlick(float cos_theta, vec3 f0) {
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cos_theta, 0.0, 1.0), 5.0);
}

// Distance in tile space from the nearest texel edge (x) and block edge (y), 0 at the edge and up to 0.5 inside.
vec2 pbr_edges(vec2 uv) {
    float tsz = float(textureSize(u_tex_rh, 0).x);
    vec2 lt = fract(uv * tsz), lb = fract(uv);
    return vec2(min(min(lt.x, 1.0 - lt.x), min(lt.y, 1.0 - lt.y)), min(min(lb.x, 1.0 - lb.x), min(lb.y, 1.0 - lb.y)));
}

// Composite height, about 0.5 on the flat of a face: the height map, or colour luminance where there is no map, then
// every texel pressed into a soft pillow and a groove run round each block face.
float pbr_height(vec2 uv, float layer, vec2 gx, vec2 gy) {
    float h = textureGrad(u_tex_rh, vec3(uv, layer), gx, gy).g;
    if (u_pbr_f.w > 0.0 && abs(h - 0.502) < 0.004) {
        vec3 c = textureGrad(u_tex, vec3(uv, layer), gx, gy).rgb;
        h = 0.5 + (dot(c, vec3(0.299, 0.587, 0.114)) - 0.5) * u_pbr_f.w * 0.7;
    }
    vec2 ed = pbr_edges(uv);
    h += u_pbr_e.y * 0.5 * (smoothstep(0.0, u_pbr_e.z, ed.x) - 1.0);
    h += u_pbr_e.w * (smoothstep(0.0, u_pbr_f.x, ed.y) - 1.0);
    return h;
}

// Depth below the top of the relief for the parallax walk: 0 on the flat of a face, 1 at the bottom of a groove.
float pbr_depth(vec2 uv, float layer, vec2 gx, vec2 gy) {
    return clamp((0.55 - pbr_height(uv, layer, gx, gy)) / 0.55, 0.0, 1.0);
}

void main() {
    float dist = length(v_rel);
#ifdef LOD
    ivec2 col = ivec2(floor(v_cover_pos / 32.0)) - u_cover_origin;
    if (all(greaterThanEqual(col, ivec2(0))) && all(lessThan(col, ivec2(u_cover_dim)))) {
        vec2 coverage = texelFetch(u_cover, col, 0).rg * 255.0;
        float finest = v_water_depth >= 0.0 ? coverage.r : coverage.g;
        if (finest < v_lod_level - 0.5) discard;
    }
#endif
    float layer = v_layer;
    vec4 anim4 = texelFetch(u_anim, ivec2(int(layer), 0), 0);
    vec2 anim = anim4.rg * 255.0;
    if (anim.x > 1.0) layer += mod(floor(u_time * anim.y * 0.25), anim.x);
    vec4 tex = texture(u_tex, vec3(v_uv, layer));
#ifdef PASS_CUTOUT
    if (tex.a < 0.5) discard;
#endif
#ifdef SHADOW
    o_color = vec4(0.0); // depth-only pass: the alpha test above is all that matters
    return;
#endif
    // PBR: only layers with maps or a "pbr" key (anim B > 0), only cube faces, never water and never the far LOD
    // tiles. The condition is flat per primitive, so the derivatives and implicit-LOD fetches inside stay defined.
    // Everything else takes the original path untouched.
    vec3 n = v_normal;
    float roughness = 0.8, metal = 0.0;
    bool pbr = false;
    float pbr_fade = 0.0; // 1 up close, 0 from the fade end on, where the result equals the flat path's
    float cavity = 1.0, contact = 1.0;
#ifdef DFE_PBR
    // Ruling: the tangent frame is derived here from the face normal rather than passed from chunk.vert. Extra
    // varyings measurably slowed the vertex-heavy opaque pass for every pixel, and leaving chunk.vert untouched
    // keeps a pack that replaces only chunk.vert linking. T follows +u, B points up the image (OpenGL convention).
    pbr = u_pbr_on != 0 && anim4.b > 0.0 && v_water_depth < 0.0;
    if (pbr) {
        vec3 an = abs(v_normal);
        vec3 tangent = an.x > 0.5 ? vec3(0.0, 0.0, -v_normal.x) : vec3(an.y > 0.5 ? 1.0 : v_normal.z, 0.0, 0.0);
        vec3 bitangent = an.y > 0.5 ? vec3(0.0, 0.0, -v_normal.y) : vec3(0.0, 1.0, 0.0);
        pbr_fade = 1.0 - smoothstep(u_pbr_a.z, u_pbr_a.w, dist);
        vec2 gx = dFdx(v_uv), gy = dFdy(v_uv);
        vec3 dpdx = dFdx(v_rel), dpdy = dFdy(v_rel);
        vec3 vdir = -v_rel / max(dist, 0.001);

        // Parallax occlusion: walk the view ray down through the height field in uv space and sample everything at
        // the first hit, so grooves read as cut into the face. The frame comes from screen derivatives, which keeps
        // it right whatever way the face's uv runs.
        vec2 uv = v_uv;
        float pdepth = u_pbr_e.x * pbr_fade;
        if (u_pbr_pom > 0 && pdepth > 0.0) {
            vec3 p2 = cross(dpdy, v_normal), p1 = cross(v_normal, dpdx);
            vec3 cT = normalize(p2 * gx.x + p1 * gy.x + 1e-9), cB = normalize(p2 * gx.y + p1 * gy.y + 1e-9);
            float ez = max(dot(vdir, v_normal), 0.12);
            float steps = float(min(u_pbr_pom, 32));
            vec2 duv = -vec2(dot(vdir, cT), dot(vdir, cB)) / ez * (pdepth / steps);
            float cur = 0.0, ld = 1.0 / steps;
            float dmap = pbr_depth(uv, layer, gx, gy), pcur = 0.0, pd = dmap;
            vec2 puv = uv;
            for (int i = 0; i < 32; i++) {
                if (float(i) >= steps || cur >= dmap) break;
                puv = uv; pcur = cur; pd = dmap;
                uv += duv; cur += ld;
                dmap = pbr_depth(uv, layer, gx, gy);
            }
            float aft = dmap - cur, bef = pd - pcur;
            float w = clamp(aft / min(aft - bef, -1e-5), 0.0, 1.0);
            uv = mix(uv, puv, w);
        }
        tex = textureGrad(u_tex, vec3(uv, layer), gx, gy);
        vec3 tn = textureGrad(u_tex_normal, vec3(uv, layer), gx, gy).rgb * 2.0 - 1.0;
        vec2 rh = textureGrad(u_tex_rh, vec3(uv, layer), gx, gy).rg;
        roughness = clamp(rh.r * u_pbr_b.y + u_pbr_b.z, 0.02, 1.0);
        metal = clamp(anim4.a * u_pbr_b.w, 0.0, 1.0);
        tn = normalize(vec3(tn.xy * u_pbr_a.y, max(tn.z, 0.05)));
        n = normalize(mat3(tangent, bitangent, v_normal) * tn);
        // Surface-gradient bump from the composite height (map, colour relief, texel and block bevels), faded out
        // with distance where it would only shimmer.
        float depth = u_bump_strength * (anim4.b * 255.0 - 1.0) / 127.0 * u_pbr_a.x;
        float hc = pbr_height(uv, layer, gx, gy);
        float h = hc * depth * pbr_fade;
        float hx = dFdx(h), hy = dFdy(h);
        vec3 r1 = cross(dpdy, v_normal), r2 = cross(v_normal, dpdx);
        float det = dot(dpdx, r1);
        vec3 grad = sign(det) * (hx * r1 + hy * r2);
        n = normalize(mix(v_normal, normalize(abs(det) * n - grad), pbr_fade));
        // Cavity and outlines: low texels and the rims around texels and blocks take less ambient light.
        vec2 ed = pbr_edges(uv);
        float rim = (1.0 - u_pbr_f.y * (1.0 - smoothstep(0.0, u_pbr_e.z * 0.6, ed.x))) *
                    (1.0 - u_pbr_f.z * (1.0 - smoothstep(0.0, u_pbr_f.x, ed.y)));
        cavity = (1.0 - u_pbr_c.z * clamp((0.5 - hc) * 2.0, 0.0, 1.0)) * rim;
        cavity = mix(1.0, cavity, pbr_fade);
        // Contact shadow: march the height field toward the sun in tangent space; a ridge above the ray blocks it.
        float lz = dot(u_light_dir, v_normal);
        if (u_pbr_steps > 0 && u_pbr_d.x > 0.0 && lz > 0.05 && pbr_fade > 0.0 && depth > 0.0) {
            vec2 lt = vec2(dot(u_light_dir, tangent), dot(u_light_dir, bitangent)) / lz;
            float h0 = hc * depth;
            float reach = depth * u_pbr_d.y;
            float occ = 0.0;
            for (int i = 1; i <= 16; i++) {
                if (i > u_pbr_steps) break;
                float z = reach * float(i) / float(u_pbr_steps);
                float hs = pbr_height(uv + lt * z, layer, gx, gy) * depth;
                occ = max(occ, smoothstep(0.0, depth * 0.25 + 1e-4, hs - (h0 + z)));
            }
            contact = 1.0 - u_pbr_d.x * occ * pbr_fade;
        }
    }
#endif
    // Only the direct light is shadowed; block light and the ambient floor stay, so shade is never pitch black.
#if defined(DFE_SHADOW_TAP_CAP) && defined(DFE_PBR)
    // The map is sampled with the geometric normal: a bump-mapped normal tipping toward the sun on a face that
    // turns away from it would otherwise look up the block's own shadow and punch dark blotches into the shade.
    float visibility = shadow_visibility_capped(v_rel, v_normal, u_light_dir, pbr ? int(u_pbr_d.w) : 16);
#else
    float visibility = shadow_visibility(v_rel, v_normal, u_light_dir);
#endif
    vec3 light = v_light.r * u_sky_color * (1.0 - u_shadow_strength * (1.0 - visibility)) + v_light.gba;
    light = max(light, vec3(u_ambient));
#ifdef DFE_PBR
    if (pbr) light = max(light, vec3(u_pbr_c.w));
#endif
    light = min(light, vec3(1.0));
    vec3 albedo = tex.rgb * v_tint;
#ifdef DFE_PBR
    if (pbr) {
        float luma = dot(albedo, vec3(0.299, 0.587, 0.114));
        albedo = max((mix(vec3(luma), albedo, u_pbr_g.y) - 0.4) * u_pbr_g.x + 0.4, 0.0);
    }
#endif
    if (v_water_depth >= 0.0) {
        // Water colour comes from the tint alone, which holds the final colour and matches the far terrain
        // constants; the texture only adds a little ripple so the surface is not a flat sheet.
        float ripple = dot(tex.rgb, vec3(0.3333)) - 0.55;
        albedo = v_tint * (0.9 + 0.5 * ripple);
    }
    vec3 rgb;
#ifdef DFE_PBR
    if (pbr) {
        // Same light as the flat path, but the directional part of v_shade is redone with the mapped normal, and
        // the sky ambient leans on how far the normal tips toward the sky. A flat normal map reproduces v_shade.
        float face_dir = mix(1.0, 0.8 + 0.35 * max(dot(v_normal, u_light_dir), 0.0), u_light_shade);
        float ndl = max(dot(n, u_light_dir), 0.0);
        float map_dir = mix(1.0, 0.8 + 0.35 * ndl, u_light_shade);
        map_dir = mix(face_dir, map_dir, u_pbr_c.x); // diffuse response 0 = ignore the map, 1 = default
        float sky = max(1.0 + 0.25 * u_pbr_c.y * (n.y - v_normal.y), 0.0);
        rgb = albedo * (1.0 - metal) * light * v_shade * (map_dir / face_dir) * sky * cavity * contact;
        // One Blinn-Phong lobe with Schlick Fresnel, lit by the sun or moon (u_glint) where the sky reaches.
        vec3 vdir = -v_rel / max(dist, 0.001);
        vec3 hv = normalize(u_light_dir + vdir);
        float shin = exp2(11.0 - 10.0 * roughness);
        vec3 f = fresnel_schlick(clamp(dot(hv, vdir), 0.0, 1.0), mix(vec3(0.04), albedo, metal));
        float lobe = (shin + 8.0) * 0.125 * pow(max(dot(n, hv), 0.0), shin) * ndl;
        rgb += f * lobe * u_glint * v_light.r * visibility * contact * u_pbr_b.x;
        // Sky reflection: lets the shaded side read as glossy or matte too, since the sun lobe is off there.
        vec3 refl = reflect(-vdir, n);
        float gloss = (1.0 - roughness) * (1.0 - roughness);
        vec3 fe = fresnel_schlick(max(dot(n, vdir), 0.0), mix(vec3(0.04), albedo, metal));
        rgb += fe * u_sky_color * v_light.r * gloss * clamp(0.5 + 0.5 * refl.y, 0.0, 1.0) * u_pbr_d.z * cavity;
        rgb = mix(albedo * light * v_shade, rgb, pbr_fade);
    } else
#endif
    {
        rgb = albedo * light * v_shade;
    }
    float fog = smoothstep(u_fog_start, u_fog_end, dist);
    float near_fog = 0.0;
    if (u_near_fog_density > 0.0) {
        float fade = clamp((dist - u_near_fog_start) / max(u_near_fog_end - u_near_fog_start, 1.0), 0.0, 1.0);
        float shadow_bias = mix(0.9, 1.8, 1.0 - visibility);
        near_fog = clamp(u_near_fog_density * shadow_bias * (1.0 - fade * 0.6), 0.0, 1.0);
        near_fog *= smoothstep(0.0, u_near_fog_end, dist);
    }
    if (v_water_depth >= 0.0) {
        // Schlick-style reflection: water seen at a grazing angle mirrors the sky, which keeps a wide ocean from
        // reading as a black sheet and fades it into the horizon haze instead of ending at a hard edge.
        float fresnel = fresnel_schlick(clamp(abs(v_rel.y) / max(dist, 0.001), 0.0, 1.0), vec3(0.04)).x;
        rgb = mix(rgb, u_fog_color, min(fresnel * 1.2, 0.85));
        // Sun glint: mirror the view ray about the surface, nudged by two slow ripples, and look for the light.
        // The world position is reduced modulo 1024 so float precision survives far from the origin.
        vec2 wp = vec2(u_cam_base.xz % 1024) + u_cam_frac.xz + v_rel.xz;
        vec2 wob = vec2(sin(wp.x * 1.9 + u_time * 1.3) + sin(wp.y * 2.7 - u_time * 0.9), sin(wp.y * 2.1 + u_time * 1.1) + sin(wp.x * 3.1 + u_time * 0.7)) * 0.025;
        vec3 vdir = v_rel / max(dist, 0.001);
        vec3 refl = normalize(vec3(vdir.x + wob.x, -vdir.y, vdir.z + wob.y));
        float glint = pow(max(dot(refl, u_light_dir), 0.0), 220.0);
        rgb += u_glint * glint * 1.4 * (1.0 - fog);
    }
    rgb = mix(rgb, u_fog_color, fog);
    if (near_fog > 0.0) rgb = mix(rgb, u_fog_color, near_fog);
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
