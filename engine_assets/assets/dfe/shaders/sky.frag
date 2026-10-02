#version 330 core
in vec2 v_ndc;
out vec4 o_color;

uniform float u_time;
uniform float u_aspect;

#ifdef PASS_SKY
uniform vec3 u_right;
uniform vec3 u_up;
uniform vec3 u_forward;
uniform float u_tan_half;
uniform vec3 u_zenith;
uniform vec3 u_horizon;
uniform vec3 u_fog_color;
uniform vec3 u_sun_dir;
uniform vec3 u_moon_dir;
uniform vec3 u_sun_color;
uniform float u_sun_vis;
uniform float u_moon_vis;
uniform float u_star_alpha;
uniform float u_underwater;
uniform sampler2D u_cloud;
uniform float u_cloud_cover;      // 0 clear, 1 overcast
uniform float u_cloud_height;     // metres of the cloud plane above the camera, 0 or less hides clouds
uniform float u_cloud_scale;
uniform vec2 u_cloud_origin;      // fractional texture coordinate under the camera, computed in double on the CPU
uniform vec3 u_cloud_light;
uniform vec3 u_cloud_shadow;

float hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

vec3 stars(vec3 d) {
    vec3 p = d * 120.0;
    vec3 cell = floor(p);
    float h = hash13(cell);
    if (h < 0.9945) return vec3(0.0);
    float r = length(fract(p) - 0.5);
    float twinkle = 0.75 + 0.25 * sin(u_time * 2.3 + h * 400.0);
    float tint = hash13(cell + 7.0);
    return mix(vec3(0.75, 0.85, 1.0), vec3(1.0, 0.92, 0.8), tint) * smoothstep(0.30, 0.0, r) * twinkle * (0.55 + 0.45 * hash13(cell + 3.0)) * 1.6;
}

void main() {
    vec3 d = normalize(u_forward + u_right * (v_ndc.x * u_tan_half * u_aspect) + u_up * (v_ndc.y * u_tan_half));
    float up = clamp(d.y, 0.0, 1.0);
    // An exponential rise has a finite slope at the horizon, so no visible seam forms where the gradient starts.
    vec3 col = mix(u_horizon, u_zenith, 1.0 - exp(-up * 3.0));

    if (u_star_alpha > 0.01 && d.y > 0.0) col += stars(d) * u_star_alpha * smoothstep(0.0, 0.25, d.y);

    float sd = dot(d, u_sun_dir);
    if (u_sun_vis > 0.01) {
        float glow = 0.55 * pow(max(sd, 0.0), 24.0) + 0.18 * pow(max(sd, 0.0), 5.0);
        float disc = smoothstep(0.99905, 0.99935, sd);
        col += u_sun_color * (glow + disc * 2.5) * u_sun_vis;
    }
    if (u_moon_vis > 0.01) {
        float md = dot(d, u_moon_dir);
        col += vec3(0.82, 0.86, 1.0) * (smoothstep(0.99940, 0.99965, md) * 0.9 + 0.05 * pow(max(md, 0.0), 40.0)) * u_moon_vis;
    }

    if (u_cloud_height > 0.0 && d.y > 0.02) {
        float t = u_cloud_height / d.y;
        vec2 uv = u_cloud_origin + d.xz * (t * u_cloud_scale);
        float n = texture(u_cloud, uv).r * 0.7 + texture(u_cloud, uv * 2.7 + vec2(0.37, 0.11)).r * 0.3;
        float threshold = mix(0.60, 0.28, u_cloud_cover);
        float density = smoothstep(threshold, threshold + 0.16, n);
        // A second lookup shifted toward the sun gives lit edges and dark undersides from one extra fetch.
        float toward = texture(u_cloud, uv + normalize(u_sun_dir.xz + 1e-4) * 0.012).r * 0.7 + texture(u_cloud, (uv + normalize(u_sun_dir.xz + 1e-4) * 0.012) * 2.7 + vec2(0.37, 0.11)).r * 0.3;
        float lit = clamp(0.5 + (n - toward) * 4.0, 0.0, 1.0);
        vec3 ccol = mix(u_cloud_shadow, u_cloud_light, lit);
        float fade = smoothstep(0.02, 0.22, d.y);
        col = mix(col, ccol, density * fade * 0.92);
    }

    o_color = vec4(mix(col, u_fog_color, u_underwater), 1.0);
}
#endif

#ifdef PASS_RAIN
uniform float u_rain;
uniform vec3 u_rain_color;

float hash11(float n) { return fract(sin(n * 12.9898) * 43758.5453); }

void main() {
    vec2 p = v_ndc * vec2(u_aspect, 1.0);
    float a = 0.0;
    for (int i = 0; i < 3; i++) {
        float fi = float(i);
        float scale = 16.0 + 13.0 * fi;
        float speed = 5.0 + 2.0 * fi;
        vec2 q = vec2(p.x * scale + p.y * scale * 0.12, p.y * scale * 0.16 + u_time * speed);
        float col = floor(q.x);
        float h = hash11(col + fi * 91.7);
        float along = fract(q.y + h * 13.0);
        float streak = smoothstep(0.0, 0.04, along) * (1.0 - smoothstep(0.04, 0.34, along));
        float dx = abs(fract(q.x) - 0.5 + (h - 0.5) * 0.5);
        float thin = 1.0 - smoothstep(0.04, 0.11, dx);
        a += streak * thin * step(0.35, hash11(col * 1.7 + fi)) * (0.55 - 0.12 * fi);
    }
    o_color = vec4(u_rain_color, clamp(a, 0.0, 1.0) * 0.62 * u_rain);
}
#endif
