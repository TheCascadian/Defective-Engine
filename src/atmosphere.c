/* Atmosphere: the day cycle, weather, sky dome, clouds, fog and rain.
 *
 * Decisions:
 *  - The look of the day is data. data/<ns>/atmosphere/default.json lists colour keys over a 0..1 phase and the
 *    engine only interpolates them, so a mod re-themes the sky (an alien dusk, a permanent night) without code.
 *  - Everything that depends on the weather is derived from two smoothed scalars, cloud_amt and rain_amt. That
 *    keeps transitions continuous with no per-effect state machines.
 *  - The sky is one fullscreen triangle placed at the far plane and drawn after the opaque terrain, so the early
 *    depth test rejects every pixel the terrain already covers. Clouds are a ray against one horizontal plane
 *    sampling a small tileable noise texture: two to four texture fetches per visible sky pixel, no geometry.
 *  - Rain is a screen-space streak layer, not particles. It costs a fixed amount of fill rate only while it rains
 *    and the eye is under open sky.
 *  Rejected: a skybox texture (fixed look, cannot follow the data), volumetric clouds (far over budget for an
 *  integrated GPU) and per-block rain particles (thousands of draws on the CPU-bound target). */
#include "dfe.h"

#define CLEAR_CLOUD_COVER 0.22f      /* share of the sky that holds wispy cloud on a clear day */
#define WEATHER_BLEND_RATE 0.12f     /* per second, about 25 s for a full transition */
#define EXPOSURE_RATE 3.0f
#define UNDERWATER_RATE 8.0f
#define UNDERWATER_VIEW_BLOCKS 28.0f
#define UNDERWATER_AMBIENT 0.22f
#define SUN_TILT 0.22f               /* keeps noon off the zenith so the shade direction is never degenerate */
#define CLOUD_TEX_SIZE 128
#define CLOUD_WIND_X 1.0
#define CLOUD_WIND_Z 0.35
#define CLEAR_PERIOD_SCALE 1.6f      /* clear spells last longer than overcast ones */
#define MIN_DAY_LENGTH_S 30.0
#define EXPOSURE_SKY_LOW 4.0f        /* sky light levels at which rain fades in and out */
#define EXPOSURE_SKY_HIGH 13.0f

Atmosphere g_atmo;

static struct {
    Shader sky, rain;
    GLuint vao, cloud_tex;
    bool ready;
    Rng rng;
} G;

/* ------------------------------------------------------------------ helpers */

static V3 v3_mix(V3 a, V3 b, float t) { return v3(lerpf(a.x, b.x, t), lerpf(a.y, b.y, t), lerpf(a.z, b.z, t)); }
static float luma(V3 c) { return 0.30f * c.x + 0.59f * c.y + 0.11f * c.z; }

static V3 grade(V3 c, float desaturate, float dim) {
    float l = luma(c);
    return v3_scale(v3_mix(c, v3(l, l, l), desaturate), dim);
}

static void chase(float *v, float target, float rate, double dt) {
    *v += (target - *v) * (1.0f - expf(-rate * (float)dt));
}

/* ---------------------------------------------------------------- loading */

static bool read_vec3(const Json *arr, V3 *out) {
    if (!arr || arr->type != JSON_ARRAY || json_len(arr) != 3) return false;
    float c[3];
    for (int i = 0; i < 3; i++) {
        const Json *n = json_at(arr, i);
        if (!n || n->type != JSON_NUMBER) return false;
        c[i] = (float)n->num;
    }
    *out = v3(c[0], c[1], c[2]);
    return true;
}

static bool read_key(const Json *k, SkyKey *out, const char *file, const char *mod, int index) {
    SkyKey key = {0};
    key.time = (float)json_num(k, "time", -1.0);
    key.ambient = (float)json_num(k, "ambient", 0.05);
    if (key.time < 0.0f || key.time >= 1.0f) {
        data_error(mod, file, k->line, "keys[%d].time must be in 0 to 1 (0 midnight, 0.25 sunrise, 0.5 noon, 0.75 sunset).", index);
        return false;
    }
    static const char *const names[3] = {"zenith", "horizon", "sky_light"};
    V3 *dest[3] = {&key.zenith, &key.horizon, &key.sky_light};
    for (int i = 0; i < 3; i++) {
        if (!read_vec3(json_get(k, names[i]), dest[i])) {
            data_error(mod, file, k->line, "keys[%d].%s must be an array of three numbers, red green blue in 0 to 1, for example [0.2, 0.4, 0.8].", index, names[i]);
            return false;
        }
    }
    *out = key;
    return true;
}

static void read_keys(const Json *root, const char *file, const char *mod) {
    const Json *keys = json_get(root, "keys");
    int n = keys && keys->type == JSON_ARRAY ? json_len(keys) : 0;
    if (n < 2 || n > ATMO_MAX_KEYS) {
        data_error(mod, file, root->line, "\"keys\" must be an array of 2 to %d colour keys, each with time, zenith, horizon, sky_light and ambient.", ATMO_MAX_KEYS);
        return;
    }
    for (int i = 0; i < n; i++) {
        if (!read_key(json_at(keys, i), &g_atmo.keys[i], file, mod, i)) return;
        if (i > 0 && g_atmo.keys[i].time <= g_atmo.keys[i - 1].time) {
            data_error(mod, file, json_at(keys, i)->line, "keys[%d].time must be larger than the previous key's time. List the keys in increasing time order.", i);
            return;
        }
    }
    g_atmo.key_count = n;
}

static void read_scalars(const Json *root, const char *file, const char *mod) {
    Atmosphere *a = &g_atmo;
    a->day_length_s = json_num(root, "day_length", 1200.0);
    a->start_phase = (float)json_num(root, "start_phase", 0.30);
    const Json *clouds = json_get(root, "clouds");
    a->cloud_altitude = (float)json_num(clouds, "altitude", 172.0);
    a->cloud_scale = (float)json_num(clouds, "scale", 0.0035);
    a->cloud_speed = (float)json_num(clouds, "speed", 1.6);
    const Json *weather = json_get(root, "weather");
    a->weather_min_s = (float)json_num(weather, "min_seconds", 300.0);
    a->weather_max_s = (float)json_num(weather, "max_seconds", 900.0);
    a->rain_share = (float)json_num(weather, "rain_share", 0.4);
    if (a->day_length_s < MIN_DAY_LENGTH_S)
        data_error(mod, file, root->line, "\"day_length\" is %.0f seconds; use at least %.0f so the sky does not flicker.", a->day_length_s, MIN_DAY_LENGTH_S);
    if (a->start_phase < 0.0f || a->start_phase >= 1.0f)
        data_error(mod, file, root->line, "\"start_phase\" must be in 0 to 1 (0.3 is just after sunrise).");
    if (a->cloud_scale <= 0.0f)
        data_error(mod, file, root->line, "clouds.scale must be above 0; 0.0035 gives features about 70 blocks wide.");
    if (a->weather_min_s <= 0.0f || a->weather_max_s < a->weather_min_s)
        data_error(mod, file, root->line, "weather.min_seconds must be above 0 and not larger than weather.max_seconds.");
    if (a->rain_share < 0.0f || a->rain_share > 1.0f)
        data_error(mod, file, root->line, "weather.rain_share must be in 0 to 1.");
}

int registry_load_atmosphere(void) {
    int errors_before = data_error_count();
    memset(&g_atmo, 0, sizeof g_atmo);
    StrList namespaces = {0};
    vfs_list("data", &namespaces);
    char path[160];
    const char *rel = NULL, *owner = "?";
    size_t size = 0;
    u8 *text = NULL;
    for (int n = 0; n < namespaces.n && !text; n++) {
        snprintf(path, sizeof path, "data/%s/atmosphere/default.json", namespaces.d[n]);
        text = vfs_read(path, &size, &owner);
        if (text) rel = path;
    }
    strlist_free(&namespaces);
    if (!text) {
        data_error("?", "data/<namespace>/atmosphere/default.json", 0, "no atmosphere file found. Add one with \"keys\" listing sky colours over the day; the base mod ships an example.");
        return data_error_count() - errors_before;
    }
    char err[200];
    int err_line = 0;
    Json *root = json_parse((const char *)text, size, err, sizeof err, &err_line);
    free(text);
    if (!root) {
        data_error(owner, rel, err_line, "%s. Fix the JSON syntax at that line.", err);
        return data_error_count() - errors_before;
    }
    read_scalars(root, rel, owner);
    read_keys(root, rel, owner);
    g_atmo.loaded = data_error_count() == errors_before;
    json_free(root);
    return data_error_count() - errors_before;
}

/* Re-reads the data file while keeping the running state (time, weather, smoothed values), so an edit shows up
 * without the sky jumping. On any error the previous data stays in force. */
int atmosphere_reload_data(void) {
    Atmosphere old = g_atmo;
    int errors = registry_load_atmosphere();
    Atmosphere fresh = g_atmo;
    if (errors || !fresh.loaded) {
        g_atmo = old;
        return errors ? errors : 1;
    }
    g_atmo = old;
    g_atmo.day_length_s = fresh.day_length_s;
    g_atmo.start_phase = fresh.start_phase;
    g_atmo.cloud_altitude = fresh.cloud_altitude;
    g_atmo.cloud_scale = fresh.cloud_scale;
    g_atmo.cloud_speed = fresh.cloud_speed;
    g_atmo.weather_min_s = fresh.weather_min_s;
    g_atmo.weather_max_s = fresh.weather_max_s;
    g_atmo.rain_share = fresh.rain_share;
    memcpy(g_atmo.keys, fresh.keys, sizeof g_atmo.keys);
    g_atmo.key_count = fresh.key_count;
    return 0;
}

/* ------------------------------------------------------------- evaluation */

static void sample_keys(const Atmosphere *a, float p, V3 *zenith, V3 *horizon, V3 *sky_light, float *ambient) {
    int n = a->key_count, lo = n - 1;
    float t0 = a->keys[n - 1].time - 1.0f;
    for (int i = 0; i < n; i++)
        if (a->keys[i].time <= p) { lo = i; t0 = a->keys[i].time; }
    int hi = (lo + 1) % n;
    float t1 = a->keys[hi].time;
    if (hi == 0 && t0 >= 0.0f) t1 += 1.0f; /* the last key blends into the first one of the next day */
    float f = smoothstepf(0.0f, 1.0f, t1 > t0 ? (p - t0) / (t1 - t0) : 0.0f);
    const SkyKey *k0 = &a->keys[lo], *k1 = &a->keys[hi];
    *zenith = v3_mix(k0->zenith, k1->zenith, f);
    *horizon = v3_mix(k0->horizon, k1->horizon, f);
    *sky_light = v3_mix(k0->sky_light, k1->sky_light, f);
    *ambient = lerpf(k0->ambient, k1->ambient, f);
}

static void compute_lights(Atmosphere *a) {
    float angle = (a->phase - 0.25f) * TAU_F;
    a->sun_dir = v3_norm(v3(cosf(angle), sinf(angle), SUN_TILT));
    a->moon_dir = v3(-a->sun_dir.x, -a->sun_dir.y, -a->sun_dir.z);
    float cloudy = a->cloud_amt;
    float sun_up = smoothstepf(-0.06f, 0.04f, a->sun_dir.y);
    a->sun_vis = sun_up * (1.0f - cloudy * 0.95f);
    a->moon_vis = smoothstepf(-0.06f, 0.04f, a->moon_dir.y) * (1.0f - cloudy * 0.9f) * (1.0f - smoothstepf(-0.05f, 0.35f, a->sun_dir.y));
    a->star_alpha = a->stars ? (1.0f - smoothstepf(-0.12f, 0.05f, a->sun_dir.y)) * (1.0f - cloudy) : 0.0f;
    a->sun_color = v3_mix(v3(1.0f, 0.50f, 0.25f), v3(1.0f, 0.93f, 0.78f), smoothstepf(0.0f, 0.4f, a->sun_dir.y));
    /* Faces are shaded by the stronger of the two lights; overcast flattens the contrast. */
    float sun_s = smoothstepf(-0.05f, 0.25f, a->sun_dir.y);
    float moon_s = 0.45f * smoothstepf(-0.05f, 0.25f, a->moon_dir.y) * (1.0f - sun_s);
    V3 dir = v3_add(v3_scale(a->sun_dir, sun_s), v3_scale(a->moon_dir, moon_s));
    a->shade_dir = v3_len(dir) > 1e-4f ? v3_norm(dir) : v3(0, 1, 0);
    a->shade_strength = MIN(sun_s + moon_s, 1.0f) * (1.0f - 0.6f * cloudy);
}

void atmosphere_evaluate(Atmosphere *a, double game_seconds) {
    double p = game_seconds / a->day_length_s + a->start_phase;
    a->phase = (float)(p - floor(p));
    V3 zenith, horizon, sky_light;
    float ambient;
    sample_keys(a, a->phase, &zenith, &horizon, &sky_light, &ambient);

    float cloud = a->cloud_amt, rain = a->rain_amt;
    float gray = cloud * 0.45f + rain * 0.25f;
    a->horizon = grade(horizon, gray, 1.0f - 0.22f * cloud - 0.12f * rain);
    a->zenith = v3_mix(grade(zenith, gray, 1.0f - 0.30f * cloud - 0.15f * rain), a->horizon, cloud * 0.8f);
    a->sky_light = grade(sky_light, 0.4f * cloud, 1.0f - 0.30f * cloud - 0.15f * rain);
    /* Seabeds get no sky light, so without a floor the view underwater is black. */
    a->ambient = lerpf(ambient, UNDERWATER_AMBIENT, a->underwater);

    float lit = a->under_daylit ? CLAMP(0.15f + luma(a->sky_light) * 0.85f, 0.0f, 1.0f) : 1.0f;
    a->fog_color = v3_mix(a->horizon, v3_scale(a->under_color, lit), a->underwater);
    a->rain_color = v3_scale(v3(0.75f, 0.80f, 0.88f), CLAMP(luma(a->sky_light) * 1.1f + 0.12f, 0.0f, 1.0f));
    compute_lights(a);
}

/* ------------------------------------------------------------------ state */

static Weather next_weather(Weather now) {
    float r = rng_float(&G.rng), r2 = rng_float(&G.rng);
    if (now != WEATHER_CLEAR && r < 0.6f) return WEATHER_CLEAR;
    return r2 < g_atmo.rain_share ? WEATHER_RAIN : WEATHER_OVERCAST;
}

static float roll_timer(Weather w) {
    float t = lerpf(g_atmo.weather_min_s, g_atmo.weather_max_s, rng_float(&G.rng));
    return w == WEATHER_CLEAR ? t * CLEAR_PERIOD_SCALE : t;
}

void atmosphere_init_state(void) {
    Atmosphere *a = &g_atmo;
    a->weather = WEATHER_CLEAR;
    a->cloud_amt = a->rain_amt = a->rain_exposure = a->underwater = 0.0f;
    a->under_color = v3(0.04f, 0.18f, 0.31f);
    a->under_daylit = true;
    a->auto_weather = true;
    a->clouds = a->stars = true;
    G.rng.s = hash64(world_seed() ^ 0xA7405F3DULL);
    a->weather_timer = roll_timer(WEATHER_CLEAR);
    atmosphere_evaluate(a, game_time_get());
}

void atmosphere_set_weather(Weather w, bool instant) {
    Atmosphere *a = &g_atmo;
    a->weather = w;
    a->weather_timer = roll_timer(w);
    if (!instant) return;
    a->cloud_amt = w == WEATHER_CLEAR ? 0.0f : 1.0f;
    a->rain_amt = w == WEATHER_RAIN ? 1.0f : 0.0f;
}

void atmosphere_set_phase(float phase) {
    double day = g_atmo.day_length_s;
    double into = (double)phase - g_atmo.start_phase;
    into -= floor(into);
    game_time_set(floor(game_time_get() / day) * day + into * day);
}

static float eye_exposure(V3 eye) {
    int sky = LIGHT_SKY(world_get_light(ifloor(eye.x), ifloor(eye.y), ifloor(eye.z)));
    return smoothstepf(EXPOSURE_SKY_LOW, EXPOSURE_SKY_HIGH, (float)sky);
}

/* The fluid the eye is in, from the world rather than the player, so a free camera and a benchmark path see it too. */
static bool eye_in_fluid(V3 eye, V3 *color, bool *daylit) {
    u16 s = world_get_state(ifloor(eye.x), ifloor(eye.y), ifloor(eye.z));
    if (s == STATE_UNLOADED || !(g_state_flags[s] & BF_FLUID)) return false;
    const BlockDef *b = block_of_state(s);
    bool lava = b && b->emit[0] > 0; /* lava glows; the only fluid that does today, as in player.c */
    *color = lava ? v3(0.55f, 0.18f, 0.03f) : v3(0.04f, 0.18f, 0.31f);
    *daylit = !lava;
    return true;
}

void atmosphere_update(double dt, V3 eye) {
    Atmosphere *a = &g_atmo;
    V3 fluid_color;
    bool fluid_daylit = true, in_fluid = eye_in_fluid(eye, &fluid_color, &fluid_daylit);
    if (in_fluid) { a->under_color = fluid_color; a->under_daylit = fluid_daylit; }
    dt = CLAMP(dt, 0.0, 0.25);
    if (a->auto_weather) {
        a->weather_timer -= (float)dt;
        if (a->weather_timer <= 0.0f) atmosphere_set_weather(next_weather(a->weather), false);
    }
    chase(&a->cloud_amt, a->weather == WEATHER_CLEAR ? 0.0f : 1.0f, WEATHER_BLEND_RATE, dt);
    chase(&a->rain_amt, a->weather == WEATHER_RAIN ? 1.0f : 0.0f, WEATHER_BLEND_RATE, dt);
    chase(&a->rain_exposure, eye_exposure(eye), EXPOSURE_RATE, dt);
    chase(&a->underwater, in_fluid ? 1.0f : 0.0f, UNDERWATER_RATE, dt);
    atmosphere_evaluate(a, game_time_get());
}

/* ------------------------------------------------------------------ names */

typedef struct PhaseName { const char *name; float phase; } PhaseName;
static const PhaseName PHASE_NAMES[] = {
    {"midnight", 0.00f}, {"dawn", 0.25f}, {"morning", 0.32f}, {"noon", 0.50f},
    {"afternoon", 0.68f}, {"dusk", 0.75f}, {"night", 0.90f},
};

bool atmosphere_parse_phase(const char *text, float *phase) {
    for (size_t i = 0; i < ARRAY_LEN(PHASE_NAMES); i++)
        if (!strcmp(text, PHASE_NAMES[i].name)) { *phase = PHASE_NAMES[i].phase; return true; }
    char *end = NULL;
    float v = strtof(text, &end);
    if (end == text || *end || v < 0.0f || v >= 1.0f) return false;
    *phase = v;
    return true;
}

const char *atmosphere_phase_name(float phase) {
    if (phase < 0.20f || phase >= 0.85f) return "night";
    if (phase < 0.29f) return "dawn";
    if (phase < 0.45f) return "morning";
    if (phase < 0.60f) return "noon";
    if (phase < 0.72f) return "afternoon";
    return "dusk";
}

const char *atmosphere_weather_name(Weather w) {
    static const char *const names[WEATHER_COUNT] = {"clear", "overcast", "rain"};
    return w >= 0 && w < WEATHER_COUNT ? names[w] : "unknown";
}

bool atmosphere_parse_weather(const char *text, Weather *w) {
    for (int i = 0; i < WEATHER_COUNT; i++)
        if (!strcmp(text, atmosphere_weather_name((Weather)i))) { *w = (Weather)i; return true; }
    return false;
}

/* --------------------------------------------------------------------- GL */

/* Tileable value noise, three octaves on wrapped lattices, stretched to the full 0..255 range so the cloud
 * threshold means the same thing whatever the random lattice happened to produce. */
static float lattice(int octave, int period, int x, int y) {
    u64 key = ((u64)octave << 48) ^ ((u64)(u32)(x % period) << 24) ^ (u64)(u32)(y % period);
    return hash_to_unit(hash64(key ^ 0x51ED270BULL));
}

static float value_noise(int octave, int period, float u, float v) {
    float fx = u * (float)period, fy = v * (float)period;
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    float tx = fx - (float)x0, ty = fy - (float)y0;
    tx = tx * tx * (3.0f - 2.0f * tx);
    ty = ty * ty * (3.0f - 2.0f * ty);
    float a = lattice(octave, period, x0, y0), b = lattice(octave, period, x0 + 1, y0);
    float c = lattice(octave, period, x0, y0 + 1), d = lattice(octave, period, x0 + 1, y0 + 1);
    return lerpf(lerpf(a, b, tx), lerpf(c, d, tx), ty);
}

static GLuint build_cloud_texture(void) {
    static const int periods[3] = {4, 8, 16};
    static const float weights[3] = {0.55f, 0.30f, 0.15f};
    float *f = xmalloc((size_t)CLOUD_TEX_SIZE * CLOUD_TEX_SIZE * sizeof(float));
    float lo = 1e9f, hi = -1e9f;
    for (int y = 0; y < CLOUD_TEX_SIZE; y++) {
        for (int x = 0; x < CLOUD_TEX_SIZE; x++) {
            float u = (float)x / CLOUD_TEX_SIZE, v = (float)y / CLOUD_TEX_SIZE, s = 0.0f;
            for (int o = 0; o < 3; o++) s += weights[o] * value_noise(o, periods[o], u, v);
            f[y * CLOUD_TEX_SIZE + x] = s;
            lo = MIN(lo, s);
            hi = MAX(hi, s);
        }
    }
    u8 *px = xmalloc((size_t)CLOUD_TEX_SIZE * CLOUD_TEX_SIZE);
    for (int i = 0; i < CLOUD_TEX_SIZE * CLOUD_TEX_SIZE; i++) px[i] = (u8)(255.0f * (f[i] - lo) / (hi - lo));
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, CLOUD_TEX_SIZE, CLOUD_TEX_SIZE, 0, GL_RED, GL_UNSIGNED_BYTE, px);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    free(f);
    free(px);
    return tex;
}

bool atmosphere_gl_init(void) {
    if (!shader_load(&G.sky, "sky", "assets/dfe/shaders/sky.vert", "assets/dfe/shaders/sky.frag", "#define PASS_SKY 1\n")) return false;
    if (!shader_load(&G.rain, "rain", "assets/dfe/shaders/sky.vert", "assets/dfe/shaders/sky.frag", "#define PASS_RAIN 1\n")) return false;
    glGenVertexArrays(1, &G.vao); /* core profile requires a bound VAO even when no attribute is read */
    G.cloud_tex = build_cloud_texture();
    G.ready = true;
    return true;
}

bool atmosphere_gl_reload_shaders(void) {
    Shader sky = {0}, rain = {0};
    if (!shader_load(&sky, "sky", "assets/dfe/shaders/sky.vert", "assets/dfe/shaders/sky.frag", "#define PASS_SKY 1\n") ||
        !shader_load(&rain, "rain", "assets/dfe/shaders/sky.vert", "assets/dfe/shaders/sky.frag", "#define PASS_RAIN 1\n")) {
        if (sky.program) shader_destroy(&sky);
        if (rain.program) shader_destroy(&rain);
        return false;
    }
    shader_destroy(&G.sky);
    shader_destroy(&G.rain);
    G.sky = sky;
    G.rain = rain;
    return true;
}

void atmosphere_gl_shutdown(void) {
    if (!G.ready) return;
    shader_destroy(&G.sky);
    shader_destroy(&G.rain);
    glDeleteVertexArrays(1, &G.vao);
    glDeleteTextures(1, &G.cloud_tex);
    memset(&G, 0, sizeof G);
}

static void uniform3(Shader *sh, const char *name, V3 v) { glUniform3f(shader_uniform(sh, name), v.x, v.y, v.z); }

void atmosphere_set_uniforms(Shader *sh) {
    const Atmosphere *a = &g_atmo;
    uniform3(sh, "u_sky_color", a->sky_light);
    glUniform1f(shader_uniform(sh, "u_ambient"), a->ambient);
    uniform3(sh, "u_fog_color", a->fog_color);
    uniform3(sh, "u_light_dir", a->shade_dir);
    glUniform1f(shader_uniform(sh, "u_light_shade"), a->shade_strength);
    /* Water glints under the sun and, much more faintly, under the moon. */
    V3 glint = v3_add(v3_scale(a->sun_color, a->sun_vis), v3_scale(v3(0.55f, 0.62f, 0.85f), a->moon_vis * 0.35f));
    uniform3(sh, "u_glint", glint);
}

void atmosphere_adjust_fog(float *start, float *end) {
    const Atmosphere *a = &g_atmo;
    float scale = 1.0f - 0.18f * a->cloud_amt - 0.30f * a->rain_amt;
    *start *= scale;
    *end *= scale;
    *start = lerpf(*start, 0.5f, a->underwater);
    *end = lerpf(*end, UNDERWATER_VIEW_BLOCKS, a->underwater);
}

static void set_view_uniforms(Shader *sh, const Camera *cam, double time_s) {
    glUniform1f(shader_uniform(sh, "u_time"), (float)time_s);
    glUniform1f(shader_uniform(sh, "u_aspect"), cam->proj.m[5] / cam->proj.m[0]);
}

void atmosphere_draw_sky(const Camera *cam, double time_s) {
    if (!G.ready || g_scene_cfg.wireframe) return;
    const Atmosphere *a = &g_atmo;
    Shader *sh = &G.sky;
    shader_use(sh);
    set_view_uniforms(sh, cam, time_s);
    uniform3(sh, "u_right", cam->right);
    uniform3(sh, "u_up", cam->up);
    uniform3(sh, "u_forward", cam->forward);
    glUniform1f(shader_uniform(sh, "u_tan_half"), 1.0f / cam->proj.m[5]);
    uniform3(sh, "u_zenith", a->zenith);
    uniform3(sh, "u_horizon", a->horizon);
    uniform3(sh, "u_fog_color", a->fog_color);
    uniform3(sh, "u_sun_dir", a->sun_dir);
    uniform3(sh, "u_moon_dir", a->moon_dir);
    uniform3(sh, "u_sun_color", a->sun_color);
    glUniform1f(shader_uniform(sh, "u_sun_vis"), a->sun_vis);
    glUniform1f(shader_uniform(sh, "u_moon_vis"), a->moon_vis);
    glUniform1f(shader_uniform(sh, "u_star_alpha"), a->star_alpha);
    glUniform1f(shader_uniform(sh, "u_underwater"), a->underwater);
    /* The texture coordinate under the camera is reduced in double precision: far from the origin a float
     * world position would lose the fraction that places the clouds. */
    double wind = (double)a->cloud_speed * time_s * (double)a->cloud_scale;
    double ox = (double)cam->pos.x * a->cloud_scale + wind * CLOUD_WIND_X;
    double oz = (double)cam->pos.z * a->cloud_scale + wind * CLOUD_WIND_Z;
    glUniform2f(shader_uniform(sh, "u_cloud_origin"), (float)(ox - floor(ox)), (float)(oz - floor(oz)));
    float height = a->clouds ? a->cloud_altitude - cam->pos.y : 0.0f; /* above the layer there is nothing to draw */
    glUniform1f(shader_uniform(sh, "u_cloud_height"), height);
    glUniform1f(shader_uniform(sh, "u_cloud_scale"), a->cloud_scale);
    glUniform1f(shader_uniform(sh, "u_cloud_cover"), lerpf(CLEAR_CLOUD_COVER, 1.0f, a->cloud_amt));
    V3 lit = v3_add(v3_scale(a->sky_light, 1.05f), v3(0.03f, 0.03f, 0.03f));
    uniform3(sh, "u_cloud_light", lit);
    uniform3(sh, "u_cloud_shadow", v3_scale(lit, 0.62f - 0.12f * a->cloud_amt));
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, G.cloud_tex);
    glUniform1i(shader_uniform(sh, "u_cloud"), 3);
    glActiveTexture(GL_TEXTURE0);

    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(G.vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    g_scene_stats.draw_calls++;
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
}

void atmosphere_draw_rain(const Camera *cam, double time_s) {
    const Atmosphere *a = &g_atmo;
    float amount = a->rain_amt * a->rain_exposure * (1.0f - a->underwater);
    if (!G.ready || amount < 0.02f) return;
    Shader *sh = &G.rain;
    shader_use(sh);
    set_view_uniforms(sh, cam, time_s);
    glUniform1f(shader_uniform(sh, "u_rain"), amount);
    uniform3(sh, "u_rain_color", a->rain_color);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(G.vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    g_scene_stats.draw_calls++;
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
}
