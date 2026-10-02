/* Terrain generator. Height and biome are pure functions of (seed, x, z), so far terrain, structures and
 * neighbouring columns can all agree without exchanging data. Caves are 3D noise sampled on a coarse grid
 * and interpolated. Which blocks fill each role comes from data/<ns>/worldgen/default.json. */
#include "dfe.h"

#include <limits.h>

#define FNL_IMPL
#include "FastNoiseLite.h"

#define BAND_LO (-2)
#define BAND_HI 6
#define CAVE_CELL 4
#define CAVE_GRID_XZ (CHUNK_SIZE / CAVE_CELL + 1)
#define CAVE_MIN_DEPTH 5
#define CAVE_TUNNEL_WIDTH 0.075f
#define CAVE_CHEESE_THRESHOLD 0.62f
#define SUBSURFACE_DEPTH 3
#define MOUNTAIN_HEIGHT_START 110.0f
#define SNOWLINE 150.0f
#define TREELINE 128.0f
#define LINE_WOBBLE 10.0f   /* tree and snow lines move by up to this many blocks so they never read as a ruled stripe */
#define STEEP_SLOPE 2       /* height step to a neighbouring column that exposes bare rock */

typedef enum { BIOME_OCEAN, BIOME_BEACH, BIOME_DESERT, BIOME_TUNDRA, BIOME_SWAMP, BIOME_FOREST, BIOME_PLAINS, BIOME_MOUNTAIN, BIOME_COUNT } Biome;

typedef struct GenConfig {
    bool loaded;
    int sea_level, deep_level;
    u16 stone, deep, dirt, grass, sand, sandstone, gravel, snow, mud, water;
    u32 far_color[BIOME_COUNT];
} GenConfig;

static GenConfig C;
static struct {
    fnl_state cont, mount, ridge, hills, detail, temp, humid, cave_a, cave_b, cheese;
    bool ready;
} N;

struct GenScratch {
    i16 height[CHUNK_AREA];
    u8 biome[CHUNK_AREA];
    float cave[CAVE_GRID_XZ * CAVE_GRID_XZ * (((BAND_HI - BAND_LO + 1) * CHUNK_SIZE) / CAVE_CELL + 1)];
};

/* ---------------------------------------------------------------- config */

static u16 role_block(const Json *roles, const char *role, const char *file, const char *mod) {
    const char *name = json_str(roles, role, NULL);
    if (!name) {
        data_error(mod, file, 1, "worldgen block role \"%s\" is missing. Add it under \"blocks\", for example \"%s\": \"base:stone\".", role, role);
        return STATE_MISSING;
    }
    BlockDef *b = block_find(name);
    if (!b) {
        data_error(mod, file, 1, "worldgen role \"%s\" names unknown block \"%s\". Define it in data/<namespace>/blocks or fix the name.", role, name);
        return STATE_MISSING;
    }
    return b->default_state;
}

static u32 parse_color(const Json *v, u32 fallback) {
    if (!v || v->type != JSON_ARRAY || json_len(v) < 3) return fallback;
    u32 r = (u32)CLAMP((int)json_as_num(json_at(v, 0), 0), 0, 255);
    u32 g = (u32)CLAMP((int)json_as_num(json_at(v, 1), 0), 0, 255);
    u32 b = (u32)CLAMP((int)json_as_num(json_at(v, 2), 0), 0, 255);
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

int registry_load_worldgen_config(void) {
    int errors_before = data_error_count();
    memset(&C, 0, sizeof C);
    C.sea_level = 62;
    C.deep_level = 0;
    const char *rel = NULL;
    StrList namespaces = {0};
    vfs_list("data", &namespaces);
    char path[160];
    const char *owner = "?";
    size_t size = 0;
    u8 *text = NULL;
    /* Later namespaces are searched last so a mod can replace the file only by shadowing the same path. */
    for (int n = 0; n < namespaces.n && !text; n++) {
        snprintf(path, sizeof path, "data/%s/worldgen/default.json", namespaces.d[n]);
        text = vfs_read(path, &size, &owner);
        if (text) rel = path;
    }
    strlist_free(&namespaces);
    if (!text) {
        data_error("?", "data/<namespace>/worldgen/default.json", 0, "no world generation file found. Add one that lists the \"blocks\" used for stone, dirt, grass, sand and water.");
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
    C.sea_level = json_int(root, "sea_level", 62);
    C.deep_level = json_int(root, "deep_level", 0);
    const Json *roles = json_get(root, "blocks");
    if (!roles || roles->type != JSON_OBJECT) {
        data_error(owner, rel, root->line, "\"blocks\" must be an object mapping roles to block names.");
    } else {
        C.stone = role_block(roles, "stone", rel, owner);
        C.deep = role_block(roles, "deep_stone", rel, owner);
        C.dirt = role_block(roles, "dirt", rel, owner);
        C.grass = role_block(roles, "grass", rel, owner);
        C.sand = role_block(roles, "sand", rel, owner);
        C.sandstone = role_block(roles, "sandstone", rel, owner);
        C.gravel = role_block(roles, "gravel", rel, owner);
        C.snow = role_block(roles, "snow", rel, owner);
        C.mud = role_block(roles, "mud", rel, owner);
        C.water = role_block(roles, "water", rel, owner);
    }
    static const char *const names[BIOME_COUNT] = {"ocean", "beach", "desert", "tundra", "swamp", "forest", "plains", "mountain"};
    const Json *colors = json_get(root, "far_colors");
    for (int i = 0; i < BIOME_COUNT; i++) C.far_color[i] = parse_color(colors ? json_get(colors, names[i]) : NULL, 0xFF808080u);
    C.loaded = data_error_count() == errors_before;
    json_free(root);
    return data_error_count() - errors_before;
}

/* ----------------------------------------------------------------- noise */

static fnl_state make_noise(int seed, float freq, int octaves) {
    fnl_state s = fnlCreateState();
    s.seed = seed;
    s.noise_type = FNL_NOISE_OPENSIMPLEX2S;
    s.frequency = freq;
    if (octaves > 1) {
        s.fractal_type = FNL_FRACTAL_FBM;
        s.octaves = octaves;
        s.gain = 0.5f;
        s.lacunarity = 2.0f;
    }
    return s;
}

void gen_init(u64 seed) {
    int s = (int)(hash64(seed) & 0x7FFFFFFF);
    N.cont = make_noise(s + 1, 0.0009f, 3);
    N.mount = make_noise(s + 2, 0.0014f, 2);
    N.ridge = make_noise(s + 3, 0.0042f, 3);
    N.detail = make_noise(s + 4, 0.021f, 3);
    N.hills = make_noise(s + 10, 0.0065f, 3);
    N.temp = make_noise(s + 5, 0.0007f, 2);
    N.humid = make_noise(s + 6, 0.0009f, 2);
    N.cave_a = make_noise(s + 7, 0.016f, 1);
    N.cave_b = make_noise(s + 8, 0.016f, 1);
    N.cheese = make_noise(s + 9, 0.011f, 2);
    N.ready = true;
}

void gen_shutdown(void) { N.ready = false; }
GenScratch *gen_scratch_create(void) { return xcalloc(1, sizeof(GenScratch)); }
void gen_scratch_destroy(GenScratch *s) { free(s); }
void gen_band(int *lo_cy, int *hi_cy) { *lo_cy = BAND_LO; *hi_cy = BAND_HI; }
u16 gen_deep_state(void) { return C.deep; }
int gen_sea_level(void) { return C.sea_level; }

static float smooth01(float t) { t = CLAMP(t, 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }

float gen_height_at(float x, float z) {
    float cont = fnlGetNoise2D(&N.cont, x, z);
    float sea = (float)C.sea_level;
    float h = sea + 4.0f + cont * 30.0f + (cont < 0 ? cont * 24.0f : 0.0f);
    float mask = smooth01((fnlGetNoise2D(&N.mount, x, z) - 0.05f) / 0.5f) * smooth01((cont + 0.1f) * 4.0f);
    float r = 1.0f - fabsf(fnlGetNoise2D(&N.ridge, x, z));
    h += mask * (28.0f + 95.0f * r * r);
    /* Rolling hills break up the broad continental slope; they fade out in the ocean so shelves stay smooth. */
    h += fnlGetNoise2D(&N.hills, x, z) * 13.0f * smooth01((cont + 0.25f) * 3.0f);
    h += fnlGetNoise2D(&N.detail, x, z) * (3.0f + 6.0f * mask);
    return h;
}

static Biome biome_at(float x, float z, float h) {
    float sea = (float)C.sea_level;
    if (h < sea) return BIOME_OCEAN;
    float t = fnlGetNoise2D(&N.temp, x, z) - (h - 90.0f) * 0.004f;
    float m = fnlGetNoise2D(&N.humid, x, z);
    if (h > MOUNTAIN_HEIGHT_START) return BIOME_MOUNTAIN;
    if (h < sea + 2.5f && t > -0.3f) return BIOME_BEACH;
    if (t < -0.3f) return BIOME_TUNDRA;
    if (t > 0.3f && m < 0.05f) return BIOME_DESERT;
    if (m > 0.35f && h < sea + 8.0f) return BIOME_SWAMP;
    return m > 0.0f ? BIOME_FOREST : BIOME_PLAINS;
}

/* ---------------------------------------------------------------- columns */

static void fill_heightmap(GenScratch *s, int cx, int cz, int *max_h) {
    int mh = INT_MIN;
    for (int z = 0; z < CHUNK_SIZE; z++)
        for (int x = 0; x < CHUNK_SIZE; x++) {
            float wx = (float)(cx * CHUNK_SIZE + x), wz = (float)(cz * CHUNK_SIZE + z);
            float h = gen_height_at(wx, wz);
            int i = (z << 5) | x;
            s->height[i] = (i16)floorf(h);
            s->biome[i] = (u8)biome_at(wx, wz, h);
            if (s->height[i] > mh) mh = s->height[i];
        }
    *max_h = mh;
}

/* Carve strength at a lattice point: positive means air. Tunnels are where two noise fields are both near zero. */
static float cave_sample(float x, float y, float z) {
    float a = fnlGetNoise3D(&N.cave_a, x, y * 1.4f, z), b = fnlGetNoise3D(&N.cave_b, x, y * 1.4f, z);
    float tunnel = (CAVE_TUNNEL_WIDTH - sqrtf(a * a + b * b)) * 8.0f;
    float cheese = fnlGetNoise3D(&N.cheese, x, y * 2.0f, z) - CAVE_CHEESE_THRESHOLD;
    return MAX(tunnel, cheese * 4.0f);
}

static int cave_layers(int layers) { return layers * CHUNK_SIZE / CAVE_CELL + 1; }

static void fill_cave_grid(GenScratch *s, int cx, int cz, int y0, int layers, int max_h) {
    int gy_n = cave_layers(layers);
    int gy_used = MIN(gy_n, (max_h - y0) / CAVE_CELL + 2);
    for (int gy = 0; gy < gy_n; gy++)
        for (int gz = 0; gz < CAVE_GRID_XZ; gz++)
            for (int gx = 0; gx < CAVE_GRID_XZ; gx++) {
                float v = -1.0f;
                if (gy < gy_used)
                    v = cave_sample((float)(cx * CHUNK_SIZE + gx * CAVE_CELL), (float)(y0 + gy * CAVE_CELL), (float)(cz * CHUNK_SIZE + gz * CAVE_CELL));
                s->cave[(gy * CAVE_GRID_XZ + gz) * CAVE_GRID_XZ + gx] = v;
            }
}

static float cave_at(const GenScratch *s, int x, int ly, int z) {
    int gx = x / CAVE_CELL, gz = z / CAVE_CELL, gy = ly / CAVE_CELL;
    float fx = (float)(x % CAVE_CELL) * (1.0f / CAVE_CELL), fz = (float)(z % CAVE_CELL) * (1.0f / CAVE_CELL);
    float fy = (float)(ly % CAVE_CELL) * (1.0f / CAVE_CELL);
#define G(dx, dy, dz) s->cave[((gy + (dy)) * CAVE_GRID_XZ + gz + (dz)) * CAVE_GRID_XZ + gx + (dx)]
    float c00 = G(0, 0, 0) + (G(1, 0, 0) - G(0, 0, 0)) * fx, c10 = G(0, 0, 1) + (G(1, 0, 1) - G(0, 0, 1)) * fx;
    float c01 = G(0, 1, 0) + (G(1, 1, 0) - G(0, 1, 0)) * fx, c11 = G(0, 1, 1) + (G(1, 1, 1) - G(0, 1, 1)) * fx;
#undef G
    float b0 = c00 + (c10 - c00) * fz, b1 = c01 + (c11 - c01) * fz;
    return b0 + (b1 - b0) * fy;
}

/* Altitude lines follow the low-frequency hill noise, so they undulate with the terrain instead of being level. */
static float treeline_at(float x, float z) { return TREELINE + fnlGetNoise2D(&N.hills, x, z) * LINE_WOBBLE; }
static float snowline_at(float x, float z) { return SNOWLINE + fnlGetNoise2D(&N.hills, x + 4096.0f, z - 4096.0f) * LINE_WOBBLE; }

u32 gen_far_color_at(float x, float z, float height) {
    Biome b = biome_at(x, z, height);
    if (b != BIOME_MOUNTAIN) return C.far_color[b];
    if (height > snowline_at(x, z)) return C.far_color[BIOME_TUNDRA];
    if (height > treeline_at(x, z)) return C.far_color[BIOME_MOUNTAIN];
    return C.far_color[BIOME_PLAINS];
}

/* Mountains are meadow below the tree line, rock above it and snow on the upper slopes; any biome shows bare
 * rock where the ground is steep, which is what makes cliffs and ridges read as mountain rather than as a
 * green heap. The same rules drive the far-terrain colour, apart from slope, so the two layers agree. */
static u16 surface_block(Biome b, int y, float detail, bool steep, float treeline, float snowline) {
    if (steep && b != BIOME_OCEAN && b != BIOME_BEACH && b != BIOME_DESERT) return C.stone;
    switch (b) {
    case BIOME_OCEAN: return detail > 0.15f ? C.gravel : C.sand;
    case BIOME_BEACH: case BIOME_DESERT: return C.sand;
    case BIOME_TUNDRA: return C.snow;
    case BIOME_SWAMP: return y <= C.sea_level + 3 ? C.mud : C.grass;
    case BIOME_MOUNTAIN:
        if ((float)y > snowline) return C.snow;
        if ((float)y > treeline) return detail > 0.35f ? C.gravel : C.stone;
        return C.grass;
    default: return C.grass;
    }
}

static u16 subsurface_block(Biome b) {
    switch (b) {
    case BIOME_OCEAN: case BIOME_BEACH: return C.sand;
    case BIOME_DESERT: return C.sandstone;
    case BIOME_MOUNTAIN: return C.dirt;
    case BIOME_SWAMP: return C.mud;
    default: return C.dirt;
    }
}

void gen_column(GenScratch *s, int cx, int cz, u16 *states) {
    int layers = BAND_HI - BAND_LO + 1, y0 = BAND_LO * CHUNK_SIZE, H = layers * CHUNK_SIZE;
    int max_h;
    fill_heightmap(s, cx, cz, &max_h);
    fill_cave_grid(s, cx, cz, y0, layers, max_h);
    for (int z = 0; z < CHUNK_SIZE; z++)
        for (int x = 0; x < CHUNK_SIZE; x++) {
            int col = (z << 5) | x;
            int h = s->height[col];
            Biome bi = (Biome)s->biome[col];
            float detail = fnlGetNoise2D(&N.detail, (float)(cx * CHUNK_SIZE + x) * 3.1f, (float)(cz * CHUNK_SIZE + z) * 3.1f);
            int step = 0;
            for (int k = 0; k < 4; k++) {
                int nx = CLAMP(x + (k == 0) - (k == 1), 0, CHUNK_SIZE - 1), nz = CLAMP(z + (k == 2) - (k == 3), 0, CHUNK_SIZE - 1);
                step = MAX(step, abs(s->height[(nz << 5) | nx] - h));
            }
            bool steep = step >= STEEP_SLOPE;
            float treeline = treeline_at((float)(cx * CHUNK_SIZE + x), (float)(cz * CHUNK_SIZE + z));
            float snowline = snowline_at((float)(cx * CHUNK_SIZE + x), (float)(cz * CHUNK_SIZE + z));
            for (int ly = 0; ly < H; ly++) {
                int y = y0 + ly;
                u16 st;
                if (y > h) {
                    st = y <= C.sea_level ? C.water : STATE_AIR;
                } else {
                    int depth = h - y;
                    if (depth == 0) st = surface_block(bi, y, detail, steep, treeline, snowline);
                    else if (depth <= SUBSURFACE_DEPTH) st = subsurface_block(bi);
                    else st = y < C.deep_level ? C.deep : C.stone;
                    /* No carving near the surface or on the band floor keeps caves sealed from the sky and from the filler below. */
                    if (depth >= CAVE_MIN_DEPTH && ly > 2 && cave_at(s, x, ly, z) > 0.0f) st = STATE_AIR;
                }
                states[((size_t)ly << 10) | col] = st;
            }
        }
}
