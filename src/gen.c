/* Terrain generator. Height and biome are pure functions of (seed, x, z), so far terrain, structures and
 * neighbouring columns can all agree without exchanging data. Caves are 3D noise sampled on a coarse grid
 * and interpolated. Which blocks fill each role comes from data/<ns>/worldgen/default.json. */
#include "dfe.h"
#include "trees.h"
#include "epoch.h"
#include "biome.h"
#include "gen_internal.h"

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
#define STONE_BLOB_DEPTH 48 /* below this the rock is plain; the blob noise is only worth its cost where the player digs */
#define ALT_JITTER 28.0f   /* blocks; how far altitude thresholds (mountain start, tree line, snow line) are dithered */
#define BIOME_JITTER 0.11f  /* climate noise units; the width of the blended band between two biomes */
#define LINE_WOBBLE 10.0f   /* tree and snow lines move by up to this many blocks so they never read as a ruled stripe */
#define STEEP_SLOPE 5.0f    /* local relief before ordinary soil gives way to exposed rock */
#define TREE_RING 3 /* columns around a chunk whose terrain, biome and water tree placement may read */
#define TREE_RING_SPAN (CHUNK_SIZE + 2 * TREE_RING)

typedef enum { BIOME_OCEAN, BIOME_BEACH, BIOME_DESERT, BIOME_TUNDRA, BIOME_SWAMP, BIOME_FOREST, BIOME_PLAINS, BIOME_MOUNTAIN, BIOME_COUNT } Biome;

typedef struct GenBiomeDef {
    char id[64];
    char surface_name[64];
    char subsurface_name[64];
    u16 surface_state, subsurface_state;
    float temp_min, temp_max;
    float humid_min, humid_max;
    int min_h, max_h;
    int tree_chance;
    int feature_chance;
    char feature_id[64];
} GenBiomeDef;

typedef struct GenOreDef {
    int habitat;
    float water_min, water_max;
    char id[64];
    char ore_name[64];
    char replace_name[64];
    char biome_ids[8][64];
    u16 ore_state, replace_state;
    int min_y, max_y;
    int rarity;
    int size;
    int density;
    int biome_count;
} GenOreDef;

typedef struct GenConfig {
    bool loaded;
    int sea_level, deep_level;
    u16 stone, deep, dirt, grass, sand, sandstone, gravel, snow, mud, water;
    /* Decoration roles are optional: a world generation file that omits one simply has none of that feature. */
    /* Ground variety: each is optional, and a world generation file without it falls back to the plain block. */
    u16 coarse_dirt, podzol, red_sand, clay, andesite, granite, moss;
    u16 log, leaves, tall_grass, flower_red, flower_yellow, mushroom, dead_bush, coal, iron, gold, diamond;
} GenConfig;

#define MAX_WORLDGEN_BIOMES 32
#define MAX_WORLDGEN_ORES 64
#define MAX_WORLDGEN_FEATURES 64
#define MAX_WORLDGEN_STRUCTURES 32

/* Where a feature, structure or ore may sit relative to water. The default is DRY: never in a river, lake or the sea. */
typedef enum { HAB_DRY, HAB_ANY, HAB_RIVER, HAB_LAKE, HAB_SHORE, HAB_WETLAND } Habitat;
#define SHORE_DISTANCE 6.0f

typedef struct GenFeatureDef {
    int habitat;
    float water_min, water_max; /* allowed water_dist range in blocks; the defaults leave it unbounded */
    char id[64];
    char block_name[64];
    u16 block_state;
    char biome_ids[8][64];
    int biome_count;
    int chance;
    int permille; /* when nonzero, chance in thousandths of columns instead of whole percent */
    int min_y, max_y;
    int radius;
} GenFeatureDef;

typedef struct GenStructureBlock {
    int x, y, z;
    u16 state;
} GenStructureBlock;

typedef struct GenStructureDef {
    int habitat;
    float water_min, water_max;
    char id[64];
    char biome_ids[8][64];
    int biome_count;
    int chance;
    int min_y, max_y;
    GenStructureBlock blocks[32];
    int block_count;
    /* forever_worlds_policy: how the structure meets terrain that differs from what it was designed for. */
    int on_water, on_solid;
    u16 foundation;
} GenStructureDef;
enum { POL_WATER_RAISE, POL_WATER_CARVE_FOUNDATION, POL_WATER_FLOOD, POL_WATER_IGNORE };
enum { POL_SOLID_CARVE, POL_SOLID_IGNORE, POL_SOLID_REJECT };

static GenBiomeDef g_biomes[MAX_WORLDGEN_BIOMES];
static GenOreDef g_ores[MAX_WORLDGEN_ORES];
static GenFeatureDef g_features[MAX_WORLDGEN_FEATURES];
static GenStructureDef g_structures[MAX_WORLDGEN_STRUCTURES];
static int g_biome_count, g_ore_count, g_feature_count, g_structure_count;
/* True when the table holds the built-in biomes, whose indices match the Biome enum; the hand-tuned rules for rivers,
 * tree lines, plants and trees apply to them. Biomes defined by a mod are driven by their data alone. */
static bool g_default_biomes;
static bool custom_biomes(void) { return g_biome_count > 0 && !g_default_biomes; }
_Static_assert((int)HAB_WETLAND == (int)BIOME_HAB_WETLAND && (int)HAB_SHORE == (int)BIOME_HAB_SHORE && (int)HAB_RIVER == (int)BIOME_HAB_RIVER, "biome.h habitat order");
_Static_assert(MAX_WORLDGEN_BIOMES == BIOME_MAX, "biome table size");
/* Kernel 1 (data-driven biomes, see biome.c) is in force for an epoch that asks for it, as long as the built-in biome
 * set is in use.  Table slots 0..7 are the original biomes in both kernels, so a biome index means the same thing
 * across an epoch seam.  Everything new is behind these tests; the kernel 0 branches are the original code. */
static bool k1_for(const EpochParams *p) { return p->kernel >= 1 && !custom_biomes() && biome_count() > 0; }
static int g_k_ocean = -1, g_k_shore = -1, g_k_fallback;

static GenConfig C;
static HydroParams g_hp;
static u64 g_hydro_calls;
static struct {
    fnl_state cont, cont_warp, cont2, coast, ocean, ocean_warp, river2, river3, river_side, basin, mount, belt, ridge, hills, detail, river, river_warp, river_wiggle, warp, blend, patch_a, patch_b, blob, temp, humid, weird, cave_a, cave_b, cheese;
    i64 seed;
    bool ready;
} N;

struct GenScratch {
    i16 height[CHUNK_AREA];
    u8 biome[CHUNK_AREA];
    /* Hydrology is sampled once while the heightmap is built.  Decorations and
     * voxel filling must consume this snapshot, not ask the global generator
     * for the same column again. */
    GenHydrologySample hydro[CHUNK_AREA];
    /* Lazy ring around the chunk for trees: a tree rooted outside the chunk reads its neighbours' ground, biome and water. */
    GenHydrologySample ring_hydro[TREE_RING_SPAN * TREE_RING_SPAN];
    u8 ring_biome[TREE_RING_SPAN * TREE_RING_SPAN];
    u8 ring_valid[TREE_RING_SPAN * TREE_RING_SPAN];
    float cave[CAVE_GRID_XZ * CAVE_GRID_XZ * (((BAND_HI - BAND_LO + 1) * CHUNK_SIZE) / CAVE_CELL + 1)];
    /* Forever Worlds.  plan is NULL for the common case (nothing older nearby), and then none of this is touched. */
    const BlendPlan *plan;
    BlendCell cell[CHUNK_AREA];
    float ring_tree[TREE_RING_SPAN * TREE_RING_SPAN];
    u8 decor_old[CHUNK_AREA]; /* seam carve result: 1 = the cell keeps the older epoch's decoration rules */
    u32 decor_epoch[CHUNK_AREA];
    Cgm cgm;
    bool cgm_valid, cgm_overflow;
};

/* ---------------------------------------------------------- generation epochs */

/* The epoch the world generates new columns with.  Identity (the default, and always when Forever Worlds is off) keeps
 * every formula below on its original path, so output is bit for bit what it was before epochs existed. */
static EpochParams g_cur_ep = {.sea_level = 62.0f, .height_scale = 1.0f, .tunnel_width_scale = 1.0f, .tree_scale = 1.0f, .plant_scale = 1.0f, .ore_scale = 1.0f, .structure_scale = 1.0f, .identity = true};
static _Thread_local const EpochParams *T_ep; /* set while a different epoch's field is sampled */
static const EpochParams *EP(void) { return T_ep ? T_ep : &g_cur_ep; }
static const EpochParams *ep_of(u32 id) { const Epoch *e = id == epoch_current() ? NULL : epoch_get(id); return e ? &e->p : &g_cur_ep; }
static bool k1(void) { return k1_for(EP()); }
static const char *biome_id_of(int i) {
    if (i >= 0 && i < g_biome_count) return g_biomes[i].id;
    const BiomeDef *b = biome_get(i);
    return b ? b->id : "";
}

void gen_refresh_epoch(void) {
    const Epoch *e = forever_worlds_active() ? epoch_get(epoch_current()) : NULL;
    if (e) g_cur_ep = e->p; else epoch_params_default(&g_cur_ep);
    epoch_invalidate();
}

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

static u16 optional_role_block(const Json *roles, const char *role, const char *file, const char *mod) {
    if (!json_str(roles, role, NULL)) return STATE_AIR;
    return role_block(roles, role, file, mod);
}

static bool parse_block_name(const Json *v, const char *role, u16 *out, const char *rel, const char *owner) {
    const char *name = json_as_str(v, NULL);
    if (!name) {
        data_error(owner, rel, v ? v->line : 1, "worldgen field \"%s\" must be a block id such as \"base:stone\".", role);
        return false;
    }
    BlockDef *b = block_find(name);
    if (!b) {
        data_error(owner, rel, v ? v->line : 1, "worldgen field \"%s\" refers to unknown block \"%s\".", role, name);
        return false;
    }
    *out = b->default_state;
    return true;
}

static void add_biome_def(const char *id, const char *surface, const char *subsurface, float tmin, float tmax, float hmin, float hmax, int min_h, int max_h) {
    if (g_biome_count >= MAX_WORLDGEN_BIOMES) return;
    memset(&g_biomes[g_biome_count], 0, sizeof g_biomes[g_biome_count]);
    snprintf(g_biomes[g_biome_count].id, sizeof g_biomes[g_biome_count].id, "%s", id ? id : "biome");
    if (surface) snprintf(g_biomes[g_biome_count].surface_name, sizeof g_biomes[g_biome_count].surface_name, "%s", surface);
    if (subsurface) snprintf(g_biomes[g_biome_count].subsurface_name, sizeof g_biomes[g_biome_count].subsurface_name, "%s", subsurface);
    g_biomes[g_biome_count].surface_state = surface ? block_find(surface)->default_state : C.grass;
    g_biomes[g_biome_count].subsurface_state = subsurface ? block_find(subsurface)->default_state : C.dirt;
    g_biomes[g_biome_count].temp_min = tmin; g_biomes[g_biome_count].temp_max = tmax;
    g_biomes[g_biome_count].humid_min = hmin; g_biomes[g_biome_count].humid_max = hmax;
    g_biomes[g_biome_count].min_h = min_h; g_biomes[g_biome_count].max_h = max_h;
    g_biome_count++;
}

static void add_default_biomes(void) {
    add_biome_def("base:ocean", "base:sand", "base:sand", -1.0f, 0.2f, 0.0f, 1.0f, -4096, C.sea_level);
    add_biome_def("base:beach", "base:sand", "base:sand", 0.2f, 1.0f, 0.0f, 1.0f, C.sea_level - 2, C.sea_level + 2);
    add_biome_def("base:desert", "base:sand", "base:sandstone", 0.3f, 1.0f, -1.0f, 0.05f, 0, 255);
    add_biome_def("base:tundra", "base:snow", "base:dirt", -1.0f, -0.3f, 0.0f, 1.0f, 0, 255);
    add_biome_def("base:swamp", "base:mud", "base:mud", -0.25f, 0.25f, 0.35f, 1.0f, 0, C.sea_level + 8);
    add_biome_def("base:forest", "base:grass_block", "base:dirt", -0.1f, 0.6f, 0.0f, 1.0f, 0, 255);
    add_biome_def("base:plains", "base:grass_block", "base:dirt", -0.1f, 0.6f, -1.0f, 0.0f, 0, 255);
    add_biome_def("base:mountain", "base:grass_block", "base:dirt", 0.0f, 1.0f, -1.0f, 1.0f, 90, 255);
}

static void add_default_ores(void) {
    if (C.coal) { snprintf(g_ores[g_ore_count].id, sizeof g_ores[g_ore_count].id, "%s", "base:coal_ore"); snprintf(g_ores[g_ore_count].ore_name, sizeof g_ores[g_ore_count].ore_name, "%s", "base:coal_ore"); snprintf(g_ores[g_ore_count].replace_name, sizeof g_ores[g_ore_count].replace_name, "%s", "base:stone"); g_ores[g_ore_count].ore_state = C.coal; g_ores[g_ore_count].replace_state = C.stone; g_ores[g_ore_count].min_y = 0; g_ores[g_ore_count].max_y = 140; g_ores[g_ore_count].rarity = 1100; g_ores[g_ore_count].size = 3; g_ores[g_ore_count].density = 1; g_ore_count++; }
    if (C.iron) { snprintf(g_ores[g_ore_count].id, sizeof g_ores[g_ore_count].id, "%s", "base:iron_ore"); snprintf(g_ores[g_ore_count].ore_name, sizeof g_ores[g_ore_count].ore_name, "%s", "base:iron_ore"); snprintf(g_ores[g_ore_count].replace_name, sizeof g_ores[g_ore_count].replace_name, "%s", "base:stone"); g_ores[g_ore_count].ore_state = C.iron; g_ores[g_ore_count].replace_state = C.stone; g_ores[g_ore_count].min_y = 0; g_ores[g_ore_count].max_y = 90; g_ores[g_ore_count].rarity = 700; g_ores[g_ore_count].size = 3; g_ores[g_ore_count].density = 1; g_ore_count++; }
    if (C.gold) { snprintf(g_ores[g_ore_count].id, sizeof g_ores[g_ore_count].id, "%s", "base:gold_ore"); snprintf(g_ores[g_ore_count].ore_name, sizeof g_ores[g_ore_count].ore_name, "%s", "base:gold_ore"); snprintf(g_ores[g_ore_count].replace_name, sizeof g_ores[g_ore_count].replace_name, "%s", "base:stone"); g_ores[g_ore_count].ore_state = C.gold; g_ores[g_ore_count].replace_state = C.stone; g_ores[g_ore_count].min_y = 0; g_ores[g_ore_count].max_y = 40; g_ores[g_ore_count].rarity = 300; g_ores[g_ore_count].size = 2; g_ores[g_ore_count].density = 1; g_ore_count++; }
    if (C.diamond) { snprintf(g_ores[g_ore_count].id, sizeof g_ores[g_ore_count].id, "%s", "base:diamond_ore"); snprintf(g_ores[g_ore_count].ore_name, sizeof g_ores[g_ore_count].ore_name, "%s", "base:diamond_ore"); snprintf(g_ores[g_ore_count].replace_name, sizeof g_ores[g_ore_count].replace_name, "%s", "base:stone"); g_ores[g_ore_count].ore_state = C.diamond; g_ores[g_ore_count].replace_state = C.stone; g_ores[g_ore_count].min_y = 0; g_ores[g_ore_count].max_y = 24; g_ores[g_ore_count].rarity = 130; g_ores[g_ore_count].size = 2; g_ores[g_ore_count].density = 1; g_ore_count++; }
}

static void parse_biome_array(const Json *items, const char *rel, const char *owner) {
    if (!items || items->type != JSON_ARRAY) return;
    for (int i = 0; i < items->count; i++) {
        const Json *b = items->items[i];
        if (!b || b->type != JSON_OBJECT) { data_error(owner, rel, b ? b->line : 1, "each biome definition must be an object."); continue; }
        const char *id = json_str(b, "id", NULL);
        if (!id) { data_error(owner, rel, b->line, "biome definitions need an \"id\" such as \"mymod:badlands\"."); continue; }
        const Json *temp = json_get(b, "temperature");
        const Json *humid = json_get(b, "humidity");
        float tmin = -1.0f, tmax = 1.0f, hmin = -1.0f, hmax = 1.0f;
        if (temp && temp->type == JSON_ARRAY && json_len(temp) == 2) { tmin = (float)json_as_num(json_at(temp, 0), tmin); tmax = (float)json_as_num(json_at(temp, 1), tmax); }
        else if (temp) { float v = (float)json_as_num(temp, 0.0); tmin = v - 0.1f; tmax = v + 0.1f; }
        if (humid && humid->type == JSON_ARRAY && json_len(humid) == 2) { hmin = (float)json_as_num(json_at(humid, 0), hmin); hmax = (float)json_as_num(json_at(humid, 1), hmax); }
        else if (humid) { float v = (float)json_as_num(humid, 0.0); hmin = v - 0.1f; hmax = v + 0.1f; }
        const char *surface = json_str(b, "surface", NULL);
        const char *subsurface = json_str(b, "subsurface", NULL);
        u16 surface_state = surface ? block_find(surface) ? block_find(surface)->default_state : STATE_MISSING : C.grass;
        u16 subsurface_state = subsurface ? block_find(subsurface) ? block_find(subsurface)->default_state : STATE_MISSING : C.dirt;
        if (surface && block_find(surface) == NULL) { data_error(owner, rel, b->line, "biome \"%s\" names unknown surface block \"%s\".", id, surface); continue; }
        if (subsurface && block_find(subsurface) == NULL) { data_error(owner, rel, b->line, "biome \"%s\" names unknown subsurface block \"%s\".", id, subsurface); continue; }
        int min_h = json_int(b, "min_height", 0), max_h = json_int(b, "max_height", 255);
        if (g_biome_count >= MAX_WORLDGEN_BIOMES) { data_error(owner, rel, b->line, "too many biome definitions; the limit is %d. Remove some or combine them.", MAX_WORLDGEN_BIOMES); break; }
        memset(&g_biomes[g_biome_count], 0, sizeof g_biomes[g_biome_count]);
        snprintf(g_biomes[g_biome_count].id, sizeof g_biomes[g_biome_count].id, "%s", id);
        if (surface) snprintf(g_biomes[g_biome_count].surface_name, sizeof g_biomes[g_biome_count].surface_name, "%s", surface);
        if (subsurface) snprintf(g_biomes[g_biome_count].subsurface_name, sizeof g_biomes[g_biome_count].subsurface_name, "%s", subsurface);
        g_biomes[g_biome_count].surface_state = surface_state;
        g_biomes[g_biome_count].subsurface_state = subsurface_state;
        g_biomes[g_biome_count].temp_min = tmin; g_biomes[g_biome_count].temp_max = tmax;
        g_biomes[g_biome_count].humid_min = hmin; g_biomes[g_biome_count].humid_max = hmax;
        g_biomes[g_biome_count].min_h = min_h; g_biomes[g_biome_count].max_h = max_h;
        g_biomes[g_biome_count].tree_chance = json_int(b, "tree_chance", 0);
        g_biomes[g_biome_count].feature_chance = json_int(b, "feature_chance", 0);
        if (json_str(b, "feature", NULL)) snprintf(g_biomes[g_biome_count].feature_id, sizeof g_biomes[g_biome_count].feature_id, "%s", json_str(b, "feature", ""));
        g_biome_count++;
    }
}

static bool parse_habitat(const Json *o, const char *kind, const char *id, const char *rel, const char *owner, int *habitat, float *wmin, float *wmax);

static void parse_ore_array(const Json *items, const char *rel, const char *owner) {
    if (!items || items->type != JSON_ARRAY) return;
    for (int i = 0; i < items->count; i++) {
        const Json *o = items->items[i];
        if (!o || o->type != JSON_OBJECT) { data_error(owner, rel, o ? o->line : 1, "each ore definition must be an object."); continue; }
        const char *id = json_str(o, "id", NULL);
        const char *ore = json_str(o, "ore", NULL);
        const char *replace = json_str(o, "replace", NULL);
        if (!id || !ore || !replace) { data_error(owner, rel, o->line, "ore definitions need \"id\", \"ore\" and \"replace\" fields."); continue; }
        if (g_ore_count >= MAX_WORLDGEN_ORES) { data_error(owner, rel, o->line, "too many ore definitions; the limit is %d.", MAX_WORLDGEN_ORES); break; }
        BlockDef *ore_def = block_find(ore), *replace_def = block_find(replace);
        if (!ore_def || !replace_def) { data_error(owner, rel, o->line, "ore \"%s\" or replacement \"%s\" is unknown.", ore, replace); continue; }
        memset(&g_ores[g_ore_count], 0, sizeof g_ores[g_ore_count]);
        snprintf(g_ores[g_ore_count].id, sizeof g_ores[g_ore_count].id, "%s", id);
        snprintf(g_ores[g_ore_count].ore_name, sizeof g_ores[g_ore_count].ore_name, "%s", ore);
        snprintf(g_ores[g_ore_count].replace_name, sizeof g_ores[g_ore_count].replace_name, "%s", replace);
        g_ores[g_ore_count].ore_state = ore_def->default_state;
        g_ores[g_ore_count].replace_state = replace_def->default_state;
        g_ores[g_ore_count].min_y = json_int(o, "min_y", 0);
        g_ores[g_ore_count].max_y = json_int(o, "max_y", 128);
        g_ores[g_ore_count].rarity = json_int(o, "rarity", 1000);
        g_ores[g_ore_count].size = json_int(o, "size", 2);
        g_ores[g_ore_count].density = json_int(o, "density", 1);
        if (!parse_habitat(o, "ore", id, rel, owner, &g_ores[g_ore_count].habitat, &g_ores[g_ore_count].water_min, &g_ores[g_ore_count].water_max)) continue;
        if (!json_get(o, "habitat")) g_ores[g_ore_count].habitat = HAB_ANY; /* ore is underground, so it ignores water unless asked */
        const Json *biomes = json_get(o, "biomes");
        g_ores[g_ore_count].biome_count = 0;
        if (biomes && biomes->type == JSON_ARRAY) for (int k = 0; k < MIN(biomes->count, 8); k++) {
            const char *name = json_as_str(biomes->items[k], "");
            if (name[0]) snprintf(g_ores[g_ore_count].biome_ids[g_ores[g_ore_count].biome_count++], sizeof g_ores[g_ore_count].biome_ids[0], "%s", name);
        }
        g_ore_count++;
    }
}

static bool parse_habitat(const Json *o, const char *kind, const char *id, const char *rel, const char *owner, int *habitat, float *wmin, float *wmax) {
    *habitat = HAB_DRY; *wmin = 0.0f; *wmax = HY_FAR;
    const char *h = json_str(o, "habitat", NULL);
    if (h) {
        static const char *names[] = {"dry", "any", "river", "lake", "shore", "wetland"};
        int found = -1;
        for (int i = 0; i < 6; i++) if (!strcmp(h, names[i])) found = i;
        if (found < 0) { data_error(owner, rel, o->line, "%s \"%s\" has unknown habitat \"%s\". Use dry, any, river, lake, shore or wetland.", kind, id, h); return false; }
        *habitat = found;
    }
    const Json *wd = json_get(o, "water_distance");
    if (wd && wd->type == JSON_OBJECT) {
        *wmin = CLAMP((float)json_num(wd, "min", 0.0), 0.0f, HY_FAR);
        *wmax = CLAMP((float)json_num(wd, "max", HY_FAR), 0.0f, HY_FAR);
        if (*wmin > *wmax) { data_error(owner, rel, o->line, "%s \"%s\" has water_distance min above max.", kind, id); return false; }
    }
    return true;
}

static void parse_hydrology_params(const Json *root, const char *rel, const char *owner) {
    hydro_params_default(&g_hp);
    const Json *h = json_get(root, "hydrology");
    if (!h) return;
    if (h->type != JSON_OBJECT) { data_error(owner, rel, h->line, "\"hydrology\" must be an object of drainage parameters."); return; }
#define HP(f) g_hp.f = (float)json_num(h, #f, g_hp.f)
    HP(river_min_area); HP(width_base); HP(width_scale); HP(width_exp); HP(depth_base); HP(depth_scale);
    HP(bank_grad_min); HP(bank_grad_max); HP(valley_reach); HP(lake_min_depth); HP(lake_max_depth); HP(fall_drop); HP(rapids_drop);
#undef HP
    g_hp.lake_max_cells = json_int(h, "lake_max_cells", g_hp.lake_max_cells);
    char first[48];
    int changed = hydro_params_sanitize(&g_hp, first, sizeof first);
    if (changed) data_error(owner, rel, h->line, "%d hydrology value(s) were outside the safe range and were clamped; the first is \"%s\". See docs/HYDROLOGY.md for the limits.", changed, first);
}

static void parse_feature_array(const Json *items, const char *rel, const char *owner);
static void parse_structure_array(const Json *items, const char *rel, const char *owner);
static void put_if_air(u16 *states, int cx, int cz, int wx, int y, int wz, u16 state);

int gen_biome_count(void) { return g_biome_count; }
int gen_ore_count(void) { return g_ore_count; }
int gen_feature_count(void) { return g_feature_count; }
int gen_structure_count(void) { return g_structure_count; }

/* Feature, ore and structure arrays inside a biome file use the formats of the worldgen file, and are restricted to
 * that biome. */
static void parse_ore_array(const Json *items, const char *rel, const char *owner);
static void biome_extra(const Json *root, const char *id, const char *rel, const char *owner) {
    int f0 = g_feature_count, o0 = g_ore_count, s0 = g_structure_count;
    parse_feature_array(json_get(root, "features"), rel, owner);
    parse_ore_array(json_get(root, "ores"), rel, owner);
    parse_structure_array(json_get(root, "structures"), rel, owner);
    for (int i = f0; i < g_feature_count; i++) { g_features[i].biome_count = 1; snprintf(g_features[i].biome_ids[0], sizeof g_features[i].biome_ids[0], "%s", id); }
    for (int i = o0; i < g_ore_count; i++) { g_ores[i].biome_count = 1; snprintf(g_ores[i].biome_ids[0], sizeof g_ores[i].biome_ids[0], "%s", id); }
    for (int i = s0; i < g_structure_count; i++) { g_structures[i].biome_count = 1; snprintf(g_structures[i].biome_ids[0], sizeof g_structures[i].biome_ids[0], "%s", id); }
}

int registry_load_worldgen_config(void) {
    int errors_before = data_error_count();
    g_biome_count = g_ore_count = g_feature_count = g_structure_count = 0;
    g_default_biomes = false;
    memset(g_biomes, 0, sizeof g_biomes);
    memset(g_ores, 0, sizeof g_ores);
    memset(g_features, 0, sizeof g_features);
    memset(g_structures, 0, sizeof g_structures);
    memset(&C, 0, sizeof C);
    C.sea_level = 62;
    C.deep_level = 0;
    hydro_params_default(&g_hp);
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
        C.coarse_dirt = optional_role_block(roles, "coarse_dirt", rel, owner);
        C.podzol = optional_role_block(roles, "podzol", rel, owner);
        C.red_sand = optional_role_block(roles, "red_sand", rel, owner);
        C.clay = optional_role_block(roles, "clay", rel, owner);
        C.andesite = optional_role_block(roles, "andesite", rel, owner);
        C.granite = optional_role_block(roles, "granite", rel, owner);
        C.moss = optional_role_block(roles, "moss", rel, owner);
        C.log = optional_role_block(roles, "log", rel, owner);
        C.leaves = optional_role_block(roles, "leaves", rel, owner);
        C.tall_grass = optional_role_block(roles, "tall_grass", rel, owner);
        C.flower_red = optional_role_block(roles, "flower_red", rel, owner);
        C.flower_yellow = optional_role_block(roles, "flower_yellow", rel, owner);
        C.mushroom = optional_role_block(roles, "mushroom", rel, owner);
        C.dead_bush = optional_role_block(roles, "dead_bush", rel, owner);
        C.coal = optional_role_block(roles, "coal_ore", rel, owner);
        C.iron = optional_role_block(roles, "iron_ore", rel, owner);
        C.gold = optional_role_block(roles, "gold_ore", rel, owner);
        C.diamond = optional_role_block(roles, "diamond_ore", rel, owner);
    }
    parse_hydrology_params(root, rel, owner);
    parse_biome_array(json_get(root, "biomes"), rel, owner);
    parse_ore_array(json_get(root, "ores"), rel, owner);
    parse_feature_array(json_get(root, "features"), rel, owner);
    parse_structure_array(json_get(root, "structures"), rel, owner);
    if (!g_biome_count) { add_default_biomes(); g_default_biomes = true; }
    if (!g_ore_count) add_default_ores();
    biome_reset();
    if (g_default_biomes) {
        biome_load_all(biome_extra);
        g_k_ocean = g_k_shore = -1;
        for (int i = 0, n = biome_count(); i < n; i++) {
            int role = biome_get(i)->role;
            if (role == BIOME_ROLE_OCEAN && g_k_ocean < 0) g_k_ocean = i;
            if (role == BIOME_ROLE_SHORE && g_k_shore < 0) g_k_shore = i;
        }
        if (g_k_ocean < 0) g_k_ocean = 0;
        g_k_fallback = biome_find("base:plains") >= 0 ? biome_find("base:plains") : 0;
    }
    trees_reset();
    trees_load();
    {
        const char *ids[MAX_WORLDGEN_BIOMES];
        int count = g_biome_count;
        for (int i = 0; i < g_biome_count; i++) ids[i] = g_biomes[i].id;
        /* Kernel 1's table extends the built-in one (same first eight), so species can name the new biomes. */
        if (g_default_biomes && biome_count() > g_biome_count) { count = biome_count(); for (int i = 0; i < count; i++) ids[i] = biome_get(i)->id; }
        trees_bind_biomes(ids, count, !g_default_biomes);
    }
    C.loaded = data_error_count() == errors_before;
    json_free(root);
    return data_error_count() - errors_before;
}

static bool biome_name_matches(const char *biome_id, const char *name) {
    if (!biome_id || !name) return false;
    return !strcmp(biome_id, name);
}

static bool biome_included(int biome_index, const char *const *names, int count) {
    if (count == 0) return true;
    const char *biome_id = biome_id_of(biome_index);
    for (int i = 0; i < count; i++) if (biome_name_matches(biome_id, names[i])) return true;
    return false;
}

static void parse_feature_array(const Json *items, const char *rel, const char *owner) {
    if (!items || items->type != JSON_ARRAY) return;
    for (int i = 0; i < items->count; i++) {
        const Json *f = items->items[i];
        if (!f || f->type != JSON_OBJECT) { data_error(owner, rel, f ? f->line : 1, "each feature definition must be an object."); continue; }
        const char *id = json_str(f, "id", NULL), *block = json_str(f, "block", NULL);
        if (!id || !block) { data_error(owner, rel, f->line, "feature definitions need \"id\" and \"block\" fields."); continue; }
        if (g_feature_count >= MAX_WORLDGEN_FEATURES) { data_error(owner, rel, f->line, "too many feature definitions; the limit is %d.", MAX_WORLDGEN_FEATURES); break; }
        BlockDef *def = block_find(block);
        if (!def) { data_error(owner, rel, f->line, "feature \"%s\" names unknown block \"%s\".", id, block); continue; }
        GenFeatureDef d = {0};
        snprintf(d.id, sizeof d.id, "%s", id); snprintf(d.block_name, sizeof d.block_name, "%s", block);
        d.block_state = def->default_state; d.chance = json_int(f, "chance", 100); d.permille = json_int(f, "chance_per_mille", 0);
        d.min_y = json_int(f, "min_y", 0); d.max_y = json_int(f, "max_y", 255); d.radius = json_int(f, "radius", 1);
        bool ok = d.chance >= 0 && d.chance <= 100 && d.permille >= 0 && d.permille <= 1000 && d.min_y <= d.max_y && d.radius >= 0;
        if (!ok) { data_error(owner, rel, f->line, "feature \"%s\" has invalid chance, chance_per_mille, height, or radius.", id); continue; }
        if (!parse_habitat(f, "feature", id, rel, owner, &d.habitat, &d.water_min, &d.water_max)) continue;
        const Json *biomes = json_get(f, "biomes");
        d.biome_count = 0;
        if (biomes && biomes->type == JSON_ARRAY) for (int k = 0; k < MIN(biomes->count, 8); k++) {
            const char *name = json_as_str(biomes->items[k], "");
            if (name[0]) snprintf(d.biome_ids[d.biome_count++], sizeof d.biome_ids[0], "%s", name);
        }
        g_features[g_feature_count] = d;
        g_feature_count++;
    }
}

static void parse_structure_array(const Json *items, const char *rel, const char *owner) {
    if (!items || items->type != JSON_ARRAY) return;
    for (int i = 0; i < items->count; i++) {
        const Json *s = items->items[i];
        if (!s || s->type != JSON_OBJECT) { data_error(owner, rel, s ? s->line : 1, "each structure definition must be an object."); continue; }
        const char *id = json_str(s, "id", NULL);
        const Json *blocks = json_get(s, "blocks");
        if (!id || !blocks || blocks->type != JSON_ARRAY || !blocks->count) { data_error(owner, rel, s->line, "structure \"%s\" needs an \"id\" and a non-empty \"blocks\" array.", id ? id : "?" ); continue; }
        if (g_structure_count >= MAX_WORLDGEN_STRUCTURES) { data_error(owner, rel, s->line, "too many structure definitions; the limit is %d.", MAX_WORLDGEN_STRUCTURES); break; }
        if (strlen(id) >= sizeof(((StructureClaim *)0)->id)) { data_error(owner, rel, s->line, "structure id \"%s\" is too long; the limit is %d characters.", id, (int)sizeof(((StructureClaim *)0)->id) - 1); continue; }
        GenStructureDef d = {0};
        snprintf(d.id, sizeof d.id, "%s", id); d.chance = json_int(s, "chance", 100);
        d.min_y = json_int(s, "min_y", 0); d.max_y = json_int(s, "max_y", 255);
        bool ok = d.chance >= 0 && d.chance <= 100 && d.min_y <= d.max_y;
        if (!ok) { data_error(owner, rel, s->line, "structure \"%s\" has invalid chance or height.", id); continue; }
        if (!parse_habitat(s, "structure", id, rel, owner, &d.habitat, &d.water_min, &d.water_max)) continue;
        const Json *biomes = json_get(s, "biomes");
        d.biome_count = 0;
        if (biomes && biomes->type == JSON_ARRAY) for (int k = 0; k < MIN(biomes->count, 8); k++) {
            const char *name = json_as_str(biomes->items[k], "");
            if (name[0]) snprintf(d.biome_ids[d.biome_count++], sizeof d.biome_ids[0], "%s", name);
        }
        d.on_water = POL_WATER_IGNORE; d.on_solid = POL_SOLID_IGNORE; d.foundation = STATE_MISSING;
        const Json *pol = json_get(s, "forever_worlds_policy");
        if (pol) {
            if (pol->type != JSON_OBJECT) { data_error(owner, rel, pol->line, "structure \"%s\": forever_worlds_policy must be an object with on_water, on_solid and foundation_material.", id); continue; }
            const char *w = json_str(pol, "on_water", "ignore"), *so = json_str(pol, "on_solid", "ignore"), *fm = json_str(pol, "foundation_material", NULL);
            static const char *const wn[] = {"raise", "carve_foundation", "flood", "ignore"}, *const sn[] = {"carve", "ignore", "reject_structure"};
            int wi = -1, si = -1;
            for (int k = 0; k < 4; k++) if (!strcmp(w, wn[k])) wi = k;
            for (int k = 0; k < 3; k++) if (!strcmp(so, sn[k])) si = k;
            if (wi < 0) { data_error(owner, rel, pol->line, "structure \"%s\": on_water \"%s\" is not one of raise, carve_foundation, flood, ignore.", id, w); continue; }
            if (si < 0) { data_error(owner, rel, pol->line, "structure \"%s\": on_solid \"%s\" is not one of carve, ignore, reject_structure.", id, so); continue; }
            if (fm) {
                BlockDef *fb = block_find(fm);
                if (!fb) { data_error(owner, rel, pol->line, "structure \"%s\": foundation_material references unknown block \"%s\".", id, fm); continue; }
                d.foundation = fb->default_state;
            }
            d.on_water = wi; d.on_solid = si;
        }
        d.block_count = 0;
        for (int b = 0; b < blocks->count; b++) {
            const Json *entry = blocks->items[b];
            if (!entry || entry->type != JSON_ARRAY || entry->count < 4) { data_error(owner, rel, s->line, "structure \"%s\" block entries must be [x, y, z, \"block\"].", id); ok=false; continue; }
            if (d.block_count >= 32) { data_error(owner, rel, s->line, "structure \"%s\" has too many blocks; the limit is 32.", id); ok=false; break; }
            int x = (int)json_as_num(json_at(entry, 0), 0); int y = (int)json_as_num(json_at(entry, 1), 0); int z = (int)json_as_num(json_at(entry, 2), 0);
            const char *name = json_as_str(json_at(entry, 3), NULL);
            BlockDef *def = name ? block_find(name) : NULL;
            if (!def) { data_error(owner, rel, s->line, "structure \"%s\" references unknown block \"%s\".", id, name ? name : "<null>"); ok=false; continue; }
            d.blocks[d.block_count++] = (GenStructureBlock){x,y,z,def->default_state};
        }
        if (ok && d.block_count > 0) g_structures[g_structure_count++] = d;
    }
}

static void parse_worldgen_data(void) {
    g_biome_count = g_ore_count = g_feature_count = g_structure_count = 0;
    g_default_biomes = false;
    memset(g_biomes, 0, sizeof g_biomes);
    memset(g_ores, 0, sizeof g_ores);
    memset(g_features, 0, sizeof g_features);
    memset(g_structures, 0, sizeof g_structures);
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

static u64 g_gen_serial; /* changes with every gen_init, so cached tree sites from an earlier world are never reused */

void gen_init(u64 seed) {
    g_gen_serial++;
    int s = (int)(hash64(seed) & 0x7FFFFFFF);
    N.seed = (i64)(hash64(seed ^ 0xDEC0DEull) & 0x7FFFFFFFFFFFull);
    N.cont = make_noise(s + 1, 0.0009f, 3);
    N.mount = make_noise(s + 2, 0.0010f, 2);
    /* Continent shape: a large fractal warp bends the landmasses into bays and peninsulas, a second scale adds
     * archipelagos and inland seas, and a fine coast noise (applied only near sea level) roughens the shorelines. */
    N.cont_warp = make_noise(s + 21, 0.00055f, 3);
    N.cont_warp.domain_warp_type = FNL_DOMAIN_WARP_OPENSIMPLEX2;
    N.cont_warp.domain_warp_amp = 750.0f;
    N.cont_warp.fractal_type = FNL_FRACTAL_DOMAIN_WARP_PROGRESSIVE;
    N.cont2 = make_noise(s + 22, 0.0024f, 2);
    N.coast = make_noise(s + 23, 0.0085f, 4);
    /* Salt water has its own, much broader world-space field.  It is intentionally not derived from continents:
     * terrain decides the seabed, while this field decides where an open ocean exists. */
    N.ocean = make_noise(s + 27, 0.00032f, 2);
    N.ocean_warp = make_noise(s + 28, 0.00022f, 2);
    N.ocean_warp.domain_warp_type = FNL_DOMAIN_WARP_OPENSIMPLEX2;
    N.ocean_warp.domain_warp_amp = 520.0f;
    N.river2 = make_noise(s + 24, 0.0046f, 1);
    N.river2.noise_type = FNL_NOISE_OPENSIMPLEX2S;
    N.river3 = make_noise(s + 25, 0.0105f, 1);
    N.river3.noise_type = FNL_NOISE_OPENSIMPLEX2S;
    N.river_side = make_noise(s + 26, 0.0020f, 1);
    N.basin = make_noise(s + 29, 0.0065f, 2);
    N.belt = make_noise(s + 11, 0.00055f, 2);
    N.ridge = make_noise(s + 3, 0.0042f, 3);
    N.detail = make_noise(s + 4, 0.021f, 3);
    N.hills = make_noise(s + 10, 0.0038f, 3);
    /* Use a smooth, warped flow field for the river mask instead of Voronoi cells. Cellular noise creates hard
     * polygonal seams at the drainage edges, which read as sharply angular terrain facets. */
    N.river = make_noise(s + 12, 0.0018f, 1);
    N.river.noise_type = FNL_NOISE_OPENSIMPLEX2S;
    N.river_warp = make_noise(s + 14, 0.0011f, 2);
    N.river_warp.domain_warp_type = FNL_DOMAIN_WARP_OPENSIMPLEX2;
    N.river_warp.domain_warp_amp = 520.0f;
    N.river_wiggle = make_noise(s + 15, 0.0030f, 2);
    N.river_wiggle.domain_warp_type = FNL_DOMAIN_WARP_OPENSIMPLEX2;
    N.river_wiggle.domain_warp_amp = 110.0f;
    N.warp = make_noise(s + 13, 0.0007f, 1);
    N.warp.domain_warp_type = FNL_DOMAIN_WARP_OPENSIMPLEX2;
    N.warp.domain_warp_amp = 150.0f;
    N.blend = make_noise(s + 16, 0.045f, 2);
    N.patch_a = make_noise(s + 17, 0.011f, 2);
    N.patch_b = make_noise(s + 18, 0.052f, 2);
    N.blob = make_noise(s + 19, 0.040f, 2);
    N.temp = make_noise(s + 5, 0.0007f, 2);
    N.humid = make_noise(s + 6, 0.0009f, 2);
    N.weird = make_noise(s + 30, 0.0006f, 2); /* kernel 1 only: biome variants (flower forest, ...) */
    N.cave_a = make_noise(s + 7, 0.016f, 1);
    N.cave_b = make_noise(s + 8, 0.016f, 1);
    N.cheese = make_noise(s + 9, 0.011f, 2);
    N.ready = true;
    hydro_init(seed, C.sea_level, &g_hp);
}

void gen_shutdown(void) { hydro_shutdown(); N.ready = false; }
GenScratch *gen_scratch_create(void) { return xcalloc(1, sizeof(GenScratch)); }
void gen_scratch_destroy(GenScratch *s) { free(s); }
void gen_band(int *lo_cy, int *hi_cy) { *lo_cy = BAND_LO; *hi_cy = BAND_HI; }
u16 gen_deep_state(void) { return C.deep; }
int gen_sea_level(void) { return C.sea_level; }

static float smooth01(float t) { t = CLAMP(t, 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }

static float terrain_base_height(float wx, float wz);

static void terrain_coordinates(float x, float z, float *wx, float *wz) {
    *wx = x;
    *wz = z;
    fnlDomainWarp2D(&N.warp, wx, wz);
}

/* The continental field is deliberately kept separate from coastline detail.  The latter may make a shore irregular,
 * but it must never decide whether an area belongs to the open ocean: using it for that purpose turns isolated low
 * patches on land into full sea-level water columns. */
static float continent_macro_at(float wx, float wz) {
    return fnlGetNoise2D(&N.cont, wx, wz) * 0.78f + fnlGetNoise2D(&N.cont2, wx, wz) * 0.30f;
}

/* Continent value, roughly -1 (deep ocean) to 1 (inland); terrain uses the detailed version for natural shores. */
static float continent_at(float wx, float wz) {
    float x = wx, z = wz;
    fnlDomainWarp2D(&N.cont_warp, &x, &z);
    float c = continent_macro_at(x, z);
    /* Coast roughness fades with distance from the shoreline (c about 0.0 to 0.1), so interiors and deep sea stay smooth. */
    float near_coast = 1.0f - smooth01(fabsf(c - 0.05f) / 0.28f);
    c += fnlGetNoise2D(&N.coast, x, z) * 0.17f * near_coast;
    return EP()->identity ? c : c + EP()->cont_bias;
}

/* This broad field distinguishes open-ocean basins from isolated low terrain. */
static float ocean_field_at(float x, float z) {
    fnlDomainWarp2D(&N.ocean_warp, &x, &z);
    return fnlGetNoise2D(&N.ocean, x, z);
}

static bool ocean_at(float x, float z) {
    float wx, wz;
    terrain_coordinates(x, z, &wx, &wz);
    return terrain_base_height(wx, wz) < (float)C.sea_level
        && continent_at(wx, wz) < 0.12f;
}

static float terrain_base_height(float wx, float wz) {
    float cont = continent_at(wx, wz);
    float sea = (float)C.sea_level;
    float land = smooth01((cont + 0.17f) / 0.40f);
    float h = sea - 32.0f + land * 47.0f + cont * 8.0f;
    float mountain_region = smooth01((fnlGetNoise2D(&N.mount, wx, wz) + 0.12f) / 0.65f);
    float belt_signal = 1.0f - fabsf(fnlGetNoise2D(&N.belt, wx, wz));
    float belt = smooth01((belt_signal - 0.48f) / 0.32f) * land * mountain_region;
    float ridge = 1.0f - fabsf(fnlGetNoise2D(&N.ridge, wx, wz));
    const EpochParams *ep = EP();
    if (!ep->identity) ridge = CLAMP(ridge * (1.0f + ep->weirdness_bias), 0.0f, 1.0f);
    h += land * mountain_region * 12.0f + belt * (24.0f + 74.0f * ridge * ridge);
    h += fnlGetNoise2D(&N.hills, wx, wz) * (ep->identity ? 8.0f : 8.0f * (1.0f - ep->erosion_bias)) * land;
    h += fnlGetNoise2D(&N.detail, wx, wz) * (1.25f + 2.75f * belt);
    /* Blend ocean basins into a contoured lowland floor. A hard sea+1 clamp made rejected basins into enormous,
     * perfectly level shelves; the soft blend keeps dry ground above water without erasing its terrain shape. */
    float ocean = smooth01((0.08f - ocean_field_at(wx, wz)) / 0.30f);
    float dry_floor = sea + 1.0f + (cont + 1.0f)
                    + (fnlGetNoise2D(&N.hills, wx, wz) + 1.0f)
                    + (fnlGetNoise2D(&N.detail, wx, wz) + 1.0f) * 0.5f;
    float dry_height = MAX(h, dry_floor);
    h = dry_height + (h - dry_height) * ocean;
    /* Dished basins in low and middle ground. The hydrology flood fill turns each closed one into a pond, lake or, where
     * several run together, an inland sea; ridges and mountain belts are left alone. */
    float dish = smooth01((fnlGetNoise2D(&N.basin, wx, wz) - 0.30f) / 0.30f);
    h -= dish * 9.0f * (1.0f - belt) * smooth01((h - sea - 5.0f) / 8.0f);
    if (!ep->identity) h = sea + (h - sea) * ep->height_scale + ep->height_offset;
    return h;
}


float gen_terrain_height_raw(float x, float z) {
    float wx, wz;
    terrain_coordinates(x, z, &wx, &wz);
    return terrain_base_height(wx, wz);
}

bool gen_ocean_cell(float x, float z) { return ocean_at(x, z); }

u64 gen_hydrology_calls(void) { return g_hydro_calls; }
void gen_hydrology_calls_reset(void) { g_hydro_calls = 0; }

/* Turns the region drainage raster into one column: valley walls ease the terrain toward the channel, the channel is
 * carved below a single water plane, and lakes flood any ground below their level. See docs/HYDROLOGY.md. */
void gen_hydrology_at(float x, float z, GenHydrologySample *out) {
    g_hydro_calls++;
    memset(out, 0, sizeof *out);
    float wx, wz;
    terrain_coordinates(x, z, &wx, &wz);
    float base = terrain_base_height(wx, wz), sea = (float)C.sea_level;
    out->ground_y = base;
    out->water_dist = HY_FAR;
    if (base < sea && continent_at(wx, wz) < 0.12f) {
        out->type = 4;
        out->flags = GEN_HYD_OCEAN;
        out->water_y = sea;
        out->water_dist = 0.0f;
        return;
    }
    HydroRaw r;
    hydro_query(x, z, &r);
    out->water_dist = MIN(MAX(r.edge, 0.0f), HY_FAR);
    if (r.lake_dist < out->water_dist) out->water_dist = r.lake_dist;
    if (r.lakeness > 0.25f && base < r.lake_level - 0.3f) {
        out->wet = true;
        out->type = 5;
        out->flags = GEN_HYD_LAKE;
        out->water_y = r.lake_level;
        out->bed_y = base;
        out->ground_y = base;
        out->water_dist = 0.0f;
        out->channel = r.lake_level - base > 3.0f ? 0.58f : 0.45f;
        return;
    }
    if (!r.river) return;
    float e = r.edge, hw = MAX(r.half_width, 0.5f), W = r.level;
    out->flow = r.flow;
    out->downstream_x = x + r.dir_x * 2.0f;
    out->downstream_z = z + r.dir_z * 2.0f;
    out->type = hw < 3.0f ? 1 : (hw < 7.0f ? 2 : 3);
    if (e < hw + 6.0f) out->flags = (u8)(((r.flags & HYF_FALL) ? GEN_HYD_FALL : 0) | ((r.flags & HYF_RAPIDS) ? GEN_HYD_RAPIDS : 0) | ((r.flags & HYF_ESTUARY) ? GEN_HYD_ESTUARY : 0));
    if (e < 0.0f) {
        float u = CLAMP((e + hw) / hw, 0.0f, 1.0f);
        float depth = MIN(g_hp.depth_base + g_hp.depth_scale * sqrtf(hw), 9.0f);
        out->wet = true;
        out->water_y = W;
        out->bed_y = W - depth * (1.0f - u * u);
        out->ground_y = out->bed_y;
        out->channel = 1.0f - 0.45f * u * u;
        out->water_dist = 0.0f;
        return;
    }
    /* Valley wall: climb from the bank at a slope that follows the local terrain grade, never above the natural ground,
     * and fade into the natural terrain over the last stretch of the reach so no cliff is left at the valley edge. */
    float g = g_hp.bank_grad_min + (g_hp.bank_grad_max - g_hp.bank_grad_min) * CLAMP(r.grade, 0.0f, 1.0f);
    float wall = MAX(MIN(base, W + 0.35f + g * e), W + 0.35f * (1.0f - smooth01(e / 8.0f)));
    float fade = 1.0f - smooth01((e - (g_hp.valley_reach - 10.0f)) / 10.0f);
    out->ground_y = base + (wall - base) * fade;
    out->channel = 0.5f * (1.0f - smooth01(e / 6.0f));
    out->flags |= GEN_HYD_BANK;
}

float gen_height_at(float x, float z) {
    GenHydrologySample hydro;
    gen_hydrology_at(x, z, &hydro);
    return hydro.ground_y;
}

/* Climate moisture rises toward water, so wetlands form around rivers and lakes and deserts do not touch them. */
static float water_wetness(const GenHydrologySample *hydro) {
    if (!hydro) return 0.0f;
    if (hydro->wet || (hydro->flags & GEN_HYD_OCEAN)) return 1.0f;
    return 1.0f - smooth01(hydro->water_dist / 24.0f);
}

/* Kernel 1: ocean and beach stay hydro- and height-driven exactly as in kernel 0; land is the biome whose climate
 * centre is nearest in (temperature, humidity), among those whose height band and weirdness range admit the column.
 * The same jittered climate and dithered altitude as kernel 0 apply, so borders stay ragged.  Ties go to the lower
 * table index; there are only compares and plain float arithmetic here. */
static int biome_index_k1(float x, float z, float h, const GenHydrologySample *hydro) {
    float sea = (float)C.sea_level;
    if (h < sea) return g_k_ocean;
    float t = fnlGetNoise2D(&N.temp, x, z) - (h - 90.0f) * 0.004f;
    float m = fnlGetNoise2D(&N.humid, x, z) + 0.55f * water_wetness(hydro);
    if (!EP()->identity) { t += EP()->temp_bias; m += EP()->humid_bias; }
    t += fnlGetNoise2D(&N.blend, x, z) * BIOME_JITTER;
    m += fnlGetNoise2D(&N.blend, x + 5000.0f, z - 5000.0f) * BIOME_JITTER;
    float hj = fnlGetNoise2D(&N.blend, x - 3000.0f, z + 3000.0f) * ALT_JITTER;
    if (g_k_shore >= 0 && h + hj * 0.25f < sea + 2.5f && t > -0.3f) return g_k_shore;
    float hh = h + hj, w = fnlGetNoise2D(&N.weird, x, z);
    int best = -1;
    float best_score = 0.0f;
    for (int i = 0, n = biome_count(); i < n; i++) {
        const BiomeDef *b = biome_get(i);
        if (b->role != BIOME_ROLE_LAND || hh < b->height_min || hh > b->height_max || w < b->weird_min || w > b->weird_max) continue;
        float dt = (t - b->temp_center) / b->temp_extent, dm = (m - b->humid_center) / b->humid_extent;
        float score = dt * dt + dm * dm - (float)b->priority * 0.02f;
        if (best < 0 || score < best_score) { best = i; best_score = score; }
    }
    return best >= 0 ? best : g_k_fallback;
}

static int biome_index_at(float x, float z, float h, const GenHydrologySample *hydro) {
    if (k1()) return biome_index_k1(x, z, h, hydro);
    if (!custom_biomes()) {
        float sea = (float)C.sea_level;
        if (h < sea) return BIOME_OCEAN;
        float t = fnlGetNoise2D(&N.temp, x, z) - (h - 90.0f) * 0.004f;
        float m = fnlGetNoise2D(&N.humid, x, z) + 0.55f * water_wetness(hydro);
        if (!EP()->identity) { t += EP()->temp_bias; m += EP()->humid_bias; }
        /* Fine-grained jitter on both climate axes breaks every boundary into interleaved patches, so one biome
         * thins out into the next over a stretch instead of ending on a line. */
        t += fnlGetNoise2D(&N.blend, x, z) * BIOME_JITTER;
        m += fnlGetNoise2D(&N.blend, x + 5000.0f, z - 5000.0f) * BIOME_JITTER;
        /* Altitude thresholds are dithered too, so a biome change with height is a ragged transition and not a level line. */
        float hj = fnlGetNoise2D(&N.blend, x - 3000.0f, z + 3000.0f) * ALT_JITTER;
        if (h + hj > MOUNTAIN_HEIGHT_START) return BIOME_MOUNTAIN;
        if (h + hj * 0.25f < sea + 2.5f && t > -0.3f) return BIOME_BEACH;
        if (t < -0.3f) return BIOME_TUNDRA;
        if (t > 0.3f && m < 0.05f) return BIOME_DESERT;
        /* Swamp is low, wet ground: high moisture near the sea, or a shore where water is close and the ground is low. */
        if (m > 0.35f && h + hj * 0.5f < sea + 8.0f) return BIOME_SWAMP;
        if (hydro && water_wetness(hydro) > 0.4f && m > 0.2f && h < sea + 14.0f) return BIOME_SWAMP;
        return m > 0.0f ? BIOME_FOREST : BIOME_PLAINS;
    }
    float t = fnlGetNoise2D(&N.temp, x, z) - (h - 90.0f) * 0.004f;
    float m = fnlGetNoise2D(&N.humid, x, z) + 0.55f * water_wetness(hydro);
    if (!EP()->identity) { t += EP()->temp_bias; m += EP()->humid_bias; }
    for (int i = 0; i < g_biome_count; i++) {
        const GenBiomeDef *b = &g_biomes[i];
        if (h < b->min_h || h > b->max_h) continue;
        if (t < b->temp_min || t > b->temp_max) continue;
        if (m < b->humid_min || m > b->humid_max) continue;
        return i;
    }
    return 0;
}

/* ---------------------------------------------------------------- columns */

static float raw_height_with(const EpochParams *p, float x, float z) {
    const EpochParams *saved = T_ep;
    T_ep = p;
    float wx, wz;
    terrain_coordinates(x, z, &wx, &wz);
    float h = terrain_base_height(wx, wz);
    T_ep = saved;
    return h;
}

static int biome_with(const EpochParams *p, float x, float z, float h, const GenHydrologySample *hydro) {
    const EpochParams *saved = T_ep;
    T_ep = p;
    int b = biome_index_at(x, z, h, hydro);
    T_ep = saved;
    return b;
}

/* Ground height and biome of one block under a blend plan.  The older epoch's height is the current hydrology-final
 * ground plus the difference between the two epochs' raw terrain, so rivers and lakes stay where hydrology put them;
 * the blend adds weighted differences to that ground, which is exact when the epochs agree.  Biome is the weighted vote
 * of each epoch's own biome choice. */
static void blend_surface(const BlendPlan *plan, int wx, int wz, const GenHydrologySample *hy, float *h_out, int *biome_out, BlendCell *cell) {
    blend_weights(plan, wx, wz, cell);
    float x = (float)wx, z = (float)wz, ground = hy->ground_y;
    if (cell->n == 1 && cell->id[0] == plan->current) {
        *h_out = ground;
        *biome_out = biome_index_at(x, z, ground, hy);
        return;
    }
    float raw_cur = raw_height_with(&g_cur_ep, x, z), dl[EP_BLEND_MAX], sum = 0.0f;
    for (int i = 0; i < cell->n; i++) {
        dl[i] = cell->id[i] == plan->current ? 0.0f : raw_height_with(ep_of(cell->id[i]), x, z) - raw_cur;
        sum += cell->a[i] * dl[i];
    }
    float h = ground + sum;
    int cand[EP_BLEND_MAX] = {0};
    for (int i = 0; i < cell->n; i++) cand[i] = biome_with(ep_of(cell->id[i]), x, z, ground + dl[i], hy);
    int best = cand[0];
    float best_w = -1.0f;
    for (int i = 0; i < cell->n; i++) {
        float w = 0.0f;
        for (int k = 0; k < cell->n; k++) if (cand[k] == cand[i]) w += cell->a[k];
        if (w > best_w + 1e-6f || (fabsf(w - best_w) <= 1e-6f && cand[i] < best)) { best_w = w; best = cand[i]; }
    }
    *h_out = h;
    *biome_out = best;
}

static void fill_heightmap(GenScratch *s, int cx, int cz, int *max_h) {
    int mh = INT_MIN;
    for (int z = 0; z < CHUNK_SIZE; z++)
        for (int x = 0; x < CHUNK_SIZE; x++) {
            float wx = (float)(cx * CHUNK_SIZE + x), wz = (float)(cz * CHUNK_SIZE + z);
            int i = (z << 5) | x;
            GenHydrologySample *hydro = &s->hydro[i];
            gen_hydrology_at(wx, wz, hydro);
            float h = hydro->ground_y;
            if (s->plan) {
                int b;
                blend_surface(s->plan, cx * CHUNK_SIZE + x, cz * CHUNK_SIZE + z, hydro, &h, &b, &s->cell[i]);
                hydro->ground_y = h;
                s->height[i] = (i16)floorf(h);
                s->biome[i] = (u8)b;
                if (s->height[i] > mh) mh = s->height[i];
                continue;
            }
            s->height[i] = (i16)floorf(h);
            s->biome[i] = (u8)biome_index_at(wx, wz, h, hydro);
            if (s->height[i] > mh) mh = s->height[i];
        }
    *max_h = mh;
}

static float cave_sample(float x, float y, float z);

/* Blended 3D cave density: each epoch's own field at the point, weighted by the blend. */
static float blend_cave(const BlendPlan *plan, float px, float py, float pz) {
    BlendCell c;
    blend_weights(plan, (int)px, (int)pz, &c);
    if (c.n == 1 && c.id[0] == plan->current) return cave_sample(px, py, pz);
    const EpochParams *saved = T_ep;
    float acc = 0.0f;
    for (int e = 0; e < c.n; e++) { T_ep = ep_of(c.id[e]); acc += c.a[e] * cave_sample(px, py, pz); }
    T_ep = saved;
    return acc;
}

/* Carve strength at a lattice point: positive means air. Tunnels are where two noise fields are both near zero. */
static float cave_sample(float x, float y, float z) {
    float a = fnlGetNoise3D(&N.cave_a, x, y * 1.4f, z), b = fnlGetNoise3D(&N.cave_b, x, y * 1.4f, z);
    const EpochParams *ep = EP();
    float tunnel = ((ep->identity ? CAVE_TUNNEL_WIDTH : CAVE_TUNNEL_WIDTH * ep->tunnel_width_scale) - sqrtf(a * a + b * b)) * 8.0f;
    float cheese = fnlGetNoise3D(&N.cheese, x, y * 2.0f, z) - (ep->identity ? CAVE_CHEESE_THRESHOLD : CAVE_CHEESE_THRESHOLD + ep->cheese_threshold_delta);
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
                if (gy < gy_used) {
                    float px = (float)(cx * CHUNK_SIZE + gx * CAVE_CELL), py = (float)(y0 + gy * CAVE_CELL), pz = (float)(cz * CHUNK_SIZE + gz * CAVE_CELL);
                    v = s->plan ? blend_cave(s->plan, px, py, pz) : cave_sample(px, py, pz);
                }
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

enum { PK_TREE, PK_PLANT, PK_ORE, PK_STRUCT, PK_TREELINE, PK_SNOWLINE };
static float param_of(const EpochParams *p, int k) {
    switch (k) {
    case PK_TREE: return p->tree_scale;
    case PK_PLANT: return p->plant_scale;
    case PK_ORE: return p->ore_scale;
    case PK_STRUCT: return p->structure_scale;
    case PK_TREELINE: return p->treeline_offset;
    default: return p->snowline_offset;
    }
}

static float cell_param(const BlendCell *c, int k) {
    float v = 0.0f;
    for (int i = 0; i < c->n; i++) v += c->a[i] * param_of(ep_of(c->id[i]), k);
    return v;
}

/* Probability scales are not blended as numbers: a half-way chance would salt the seam with single plants.  Each cell
 * takes one epoch's rule, and the seam carve below decides where the two regions meet. */
static float decor_scale(const GenScratch *s, int col, int k) {
    if (!s->plan) return param_of(EP(), k);
    return param_of(s->decor_old[col] ? ep_of(s->decor_epoch[col]) : &g_cur_ep, k);
}

/* Altitude lines follow the low-frequency hill noise, so they undulate with the terrain instead of being level. */
static float treeline_at(float x, float z) { return TREELINE + fnlGetNoise2D(&N.hills, x, z) * LINE_WOBBLE + EP()->treeline_offset; }
static float snowline_at(float x, float z) { return SNOWLINE + fnlGetNoise2D(&N.hills, x + 4096.0f, z - 4096.0f) * LINE_WOBBLE + EP()->snowline_offset; }

/* Mountains are meadow below the tree line, rock above it and snow on the upper slopes; any biome shows bare
 * rock where the ground is steep, which is what makes cliffs and ridges read as mountain rather than as a
 * green heap. Distant voxel tiles use the same rules, so near and far terrain are made of the same blocks. */
static u16 pick(u16 variant, u16 plain) { return variant == STATE_AIR ? plain : variant; }

static u16 rock_block(float patch) {
    if (patch > 0.25f) return pick(C.andesite, C.stone);
    if (patch < -0.25f) return pick(C.granite, C.stone);
    return C.stone;
}

/* patch is a broad noise (patches tens of blocks across) and speck a fine one; between them each biome mixes several
 * ground blocks, so no stretch of land is a single flat colour. */
static BiomeEnv surface_env(int y, float detail, float slope, float river, float water_dist, float treeline, float snowline, float patch, float speck) {
    BiomeEnv e = {.y = y, .slope = slope, .river = river, .water_dist = water_dist, .treeline = treeline, .snowline = snowline, .patch = {patch, speck, detail}};
    e.speck_height = (speck + patch) * ALT_JITTER * 0.4f;
    return e;
}

static u16 surface_block(int biome_index, int y, float detail, float slope, float river, float treeline, float snowline, float patch, float speck, float water_dist) {
    if (k1()) {
        const BiomeDef *bd = biome_get(biome_index);
        BiomeEnv e = surface_env(y, detail, slope, river, water_dist, treeline, snowline, patch, speck);
        u16 st = bd ? biome_surface_state(bd, &e) : STATE_MISSING;
        return st != STATE_MISSING ? st : C.grass;
    }
    if (custom_biomes()) {
        const GenBiomeDef *b = &g_biomes[biome_index];
        if (slope >= STEEP_SLOPE && b->surface_state != C.sand && b->surface_state != C.water && b->surface_state != C.sandstone) return C.stone;
        return b->surface_state != STATE_MISSING ? b->surface_state : C.grass;
    }
    Biome b = (Biome)biome_index;
    if (river > 0.55f && b != BIOME_OCEAN) return speck > 0.3f ? pick(C.clay, C.gravel) : C.gravel;
    if (river > 0.4f && b != BIOME_OCEAN && b != BIOME_DESERT) return C.mud;
    if (slope >= STEEP_SLOPE && b != BIOME_OCEAN && b != BIOME_BEACH) return b == BIOME_DESERT ? C.sandstone : rock_block(patch);
    switch (b) {
    case BIOME_OCEAN:
        if (patch > 0.45f) return pick(C.clay, C.sand);
        return detail > 0.15f || speck > 0.5f ? C.gravel : C.sand;
    case BIOME_BEACH: return speck > 0.5f ? C.gravel : (patch < -0.5f && speck > 0.0f ? C.sandstone : C.sand);
    case BIOME_DESERT:
        if (patch > 0.3f) return pick(C.red_sand, C.sand);
        if (patch < -0.45f && speck > -0.1f) return C.sandstone;
        return speck > 0.62f ? C.gravel : C.sand;
    case BIOME_TUNDRA:
        if (patch > 0.5f) return C.gravel;
        if (patch > 0.28f && speck > 0.25f) return rock_block(patch);
        return C.snow;
    case BIOME_SWAMP:
        if (y <= C.sea_level + 1 && speck > 0.1f) return pick(C.clay, C.mud);
        if (y <= C.sea_level + 3) return C.mud;
        return patch > 0.2f ? pick(C.moss, C.grass) : (speck > 0.55f ? C.mud : C.grass);
    case BIOME_FOREST:
        if (patch > 0.1f) return pick(C.podzol, C.grass);
        if (patch < -0.55f) return pick(C.moss, C.grass);
        return speck > 0.62f ? pick(C.coarse_dirt, C.grass) : C.grass;
    case BIOME_PLAINS:
        if (patch > 0.55f && speck > 0.0f) return pick(C.coarse_dirt, C.grass);
        return speck > 0.78f ? pick(C.coarse_dirt, C.grass) : C.grass;
    case BIOME_MOUNTAIN:
        float yj = (float)y + (speck + patch) * ALT_JITTER * 0.4f;
        if (yj > snowline) return detail > 0.25f ? C.snow : rock_block(patch);
        if (yj > treeline) return detail > 0.15f ? C.gravel : rock_block(patch);
        if (speck > 0.72f) return rock_block(patch);
        return patch > 0.35f && speck > 0.3f ? pick(C.coarse_dirt, C.grass) : C.grass;
    default: return C.grass;
    }
}

/* Kernel 1 subsurface: layered by depth, and by height so a biome can stripe its cliffs. */
static u16 subsurface_k1(int biome_index, int y, int depth, const BiomeEnv *env) {
    const BiomeDef *bd = biome_get(biome_index);
    BiomeEnv e = *env;
    e.y = y;
    u16 st = bd ? biome_sub_state(bd, depth, &e) : STATE_MISSING;
    return st != STATE_MISSING ? st : C.dirt;
}

static int subsurface_depth_of(int biome_index) {
    const BiomeDef *bd = k1() ? biome_get(biome_index) : NULL;
    return bd && bd->subsurface_depth > 0 ? bd->subsurface_depth : SUBSURFACE_DEPTH;
}

static u16 subsurface_block(int biome_index) {
    if (k1()) {
        BiomeEnv e = {0};
        return subsurface_k1(biome_index, 0, 1, &e);
    }
    if (custom_biomes()) {
        const GenBiomeDef *b = &g_biomes[biome_index];
        return b->subsurface_state != STATE_MISSING ? b->subsurface_state : C.dirt;
    }
    Biome b = (Biome)biome_index;
    switch (b) {
    case BIOME_OCEAN: case BIOME_BEACH: return C.sand;
    case BIOME_DESERT: return C.sandstone;
    case BIOME_MOUNTAIN: return C.dirt;
    case BIOME_SWAMP: return C.mud;
    default: return C.dirt;
    }
}

/* ------------------------------------------------------------ decoration */

/* Everything below is a pure function of (seed, x, y, z), so a feature that crosses a column border is built
 * identically by both columns and no generation order or neighbour data is needed. */
#define SALT_ORE 0x0befull
#define SALT_PLANT 0x91a7ull
#define SALT_TREE 0x7ee5ull
#define ORE_CELL_SHIFT 1           /* ore appears in 2x2x2 clumps; each clump block is kept with 3 in 4 odds */
#define ORE_RANGE 65536u
#define COAL_ODDS 1100u            /* out of ORE_RANGE per clump */
#define IRON_ODDS 700u
#define GOLD_ODDS 300u
#define DIAMOND_ODDS 130u
#define COAL_MAX_Y 140
#define IRON_MAX_Y 90
#define GOLD_MAX_Y 40
#define DIAMOND_MAX_Y 24
#define ORE_MIN_DEPTH 3
#define TREE_LATTICE_ODDS 28       /* one candidate per this many columns, thinned further by biome */
#define TREE_CLEARANCE 4.0f        /* blocks of no new trees beside an older column, whose own trees never saw them */
#define TREE_MARGIN 2              /* canopy radius, so trees rooted in a neighbour column still reach this one */
#define TREE_MIN_TRUNK 4
#define TREE_TRUNK_RANGE 3
#define TREE_CANOPY_RADIUS 2
#define TREE_TREELINE_MARGIN 4.0f
#define PLANT_GRASS_PERCENT 14
#define PLANT_FLOWER_PERCENT 1
#define PLANT_MUSHROOM_PERCENT 2
#define PLANT_DEAD_BUSH_PERCENT 2

static bool habitat_ok(int habitat, float wmin, float wmax, const GenHydrologySample *h) {
    bool ocean = (h->flags & GEN_HYD_OCEAN) != 0;
    float d = (h->wet || ocean) ? 0.0f : h->water_dist;
    switch (habitat) {
    case HAB_DRY: if (h->wet || ocean) return false; break;
    case HAB_RIVER: if (!h->wet || h->type == 5) return false; break;
    case HAB_LAKE: if (!h->wet || h->type != 5) return false; break;
    case HAB_SHORE: if (h->wet || ocean || d > SHORE_DISTANCE) return false; break;
    case HAB_WETLAND: if (ocean || d > 2.0f * SHORE_DISTANCE) return false; break;
    default: break;
    }
    return d >= wmin && d <= wmax;
}

/* Habitat of a column for spawners and dump tools: one of dry, river, lake, shore, wetland, ocean. */
const char *gen_habitat_at(const GenHydrologySample *h) {
    if (h->flags & GEN_HYD_OCEAN) return "ocean";
    if (h->wet) return h->type == 5 ? "lake" : "river";
    if (h->water_dist <= SHORE_DISTANCE) return "shore";
    if (h->water_dist <= 2.0f * SHORE_DISTANCE) return "wetland";
    return "dry";
}

static u16 ore_at(int wx, int y, int wz, int depth, int biome_index, const GenHydrologySample *hydro, float scale) {
    if (depth < ORE_MIN_DEPTH) return STATE_AIR;
    if (g_ore_count > 0) {
        const char *biome_id = biome_id_of(biome_index);
        for (int i = 0; i < g_ore_count; i++) {
            const GenOreDef *def = &g_ores[i];
            if (y < def->min_y || y > def->max_y) continue;
            bool match = true;
            if (def->biome_count > 0) {
                match = false;
                for (int b = 0; b < def->biome_count; b++) if (!strcmp(def->biome_ids[b], biome_id)) { match = true; break; }
            }
            if (!match || !habitat_ok(def->habitat, def->water_min, def->water_max, hydro)) continue;
            u64 hh = hash3(N.seed ^ SALT_ORE, (wx + i) >> ORE_CELL_SHIFT, y >> ORE_CELL_SHIFT, (wz + i) >> ORE_CELL_SHIFT);
            unsigned roll = (unsigned)(hh & (ORE_RANGE - 1));
            int rarity = scale == 1.0f || def->rarity <= 0 ? def->rarity : MAX(1, (int)((float)def->rarity / MAX(scale, 0.01f)));
            if (rarity <= 0 || (int)(roll % (unsigned)rarity) != 0) continue;
            return def->ore_state;
        }
        return STATE_AIR;
    }
    u64 hh = hash3(N.seed ^ SALT_ORE, wx >> ORE_CELL_SHIFT, y >> ORE_CELL_SHIFT, wz >> ORE_CELL_SHIFT);
    unsigned roll = (unsigned)(hh & (ORE_RANGE - 1));
    if (((hh >> 16) & 3) == 0) return STATE_AIR;
    unsigned edge = 0;
    #define ODDS(n) (scale == 1.0f ? (n) : (unsigned)((float)(n) * scale))
    if (C.diamond && y < DIAMOND_MAX_Y && roll < (edge += ODDS(DIAMOND_ODDS))) return C.diamond;
    if (C.gold && y < GOLD_MAX_Y && roll < (edge += ODDS(GOLD_ODDS))) return C.gold;
    if (C.iron && y < IRON_MAX_Y && roll < (edge += ODDS(IRON_ODDS))) return C.iron;
    if (C.coal && y < COAL_MAX_Y && roll < (edge += ODDS(COAL_ODDS))) return C.coal;
    #undef ODDS
    return STATE_AIR;
}

static u16 plant_for(int biome_index, u64 roll) {
    if (custom_biomes()) return STATE_AIR;
    Biome b = (Biome)biome_index;
    unsigned pct = (unsigned)(roll % 100);
    switch (b) {
    case BIOME_PLAINS: case BIOME_FOREST: case BIOME_MOUNTAIN:
        if (pct < PLANT_FLOWER_PERCENT) return C.flower_red;
        if (pct < 2 * PLANT_FLOWER_PERCENT) return C.flower_yellow;
        if (b == BIOME_FOREST && pct < 2 * PLANT_FLOWER_PERCENT + PLANT_MUSHROOM_PERCENT) return C.mushroom;
        return pct < 2 * PLANT_FLOWER_PERCENT + PLANT_GRASS_PERCENT ? C.tall_grass : STATE_AIR;
    case BIOME_SWAMP: return pct < PLANT_MUSHROOM_PERCENT * 3 ? C.mushroom : (pct < 40 ? C.tall_grass : STATE_AIR);
    case BIOME_DESERT: return pct < PLANT_DEAD_BUSH_PERCENT ? C.dead_bush : STATE_AIR;
    default: return STATE_AIR;
    }
}

/* Kernel 1 plants: the biome's list in order with one roll per column, so the entries' chances add up to the real
 * density.  An entry that does not suit the ground, water distance or noise region adds nothing to the running total. */
static u16 plant_k1(const BiomeDef *b, u64 roll, u16 below, const GenHydrologySample *hy, int wx, int wz) {
    unsigned r = (unsigned)(roll % 1000), acc = 0;
    for (int i = 0; i < b->plant_count; i++) {
        const BiomePlant *p = &b->plants[i];
        bool on = false;
        for (int k = 0; k < p->on_count && !on; k++) on = p->on[k] == below;
        if (!on || !habitat_ok(p->habitat, p->water_min, p->water_max, hy)) continue;
        if (p->has_noise) {
            float v = fnlGetNoise2D(p->noise ? &N.patch_b : &N.patch_a, (float)wx, (float)wz);
            if (v < p->noise_lo || v >= p->noise_hi) continue;
        }
        acc += (unsigned)(p->chance * 10.0f + 0.5f);
        if (r < acc) return p->state;
    }
    return STATE_AIR;
}

static bool column_k1(const GenScratch *s, int col) {
    return k1_for(s->plan && s->decor_old[col] ? ep_of(s->decor_epoch[col]) : &g_cur_ep);
}

static void place_plants(const GenScratch *s, u16 *states, int cx, int cz) {
    int y0 = BAND_LO * CHUNK_SIZE, H = (BAND_HI - BAND_LO + 1) * CHUNK_SIZE;
    for (int z = 0; z < CHUNK_SIZE; z++)
        for (int x = 0; x < CHUNK_SIZE; x++) {
            int col = (z << 5) | x, h = s->height[col], ly = h + 1 - y0;
            const GenHydrologySample *hy = &s->hydro[col];
            if (h <= C.sea_level || ly < 1 || ly >= H || hy->wet || (hy->flags & GEN_HYD_OCEAN)) continue;
            size_t at = ((size_t)ly << 10) | col, below = ((size_t)(ly - 1) << 10) | col;
            if (states[at] != STATE_AIR) continue;
            int biome_index = s->biome[col];
            bool sandy = states[below] == C.sand;
            bool reed_ground = hy->water_dist < SHORE_DISTANCE && (states[below] == C.sand || states[below] == C.mud || states[below] == C.dirt);
            bool kern = column_k1(s, col);
            if (!kern && states[below] != C.grass && states[below] != C.podzol && states[below] != C.moss && !(!custom_biomes() && (Biome)biome_index == BIOME_DESERT && sandy) && !reed_ground) continue;
            u64 proll = hash3(N.seed ^ SALT_PLANT, cx * CHUNK_SIZE + x, 0, cz * CHUNK_SIZE + z);
            u16 plant;
            if (kern) {
                const BiomeDef *bd = biome_get(biome_index);
                plant = bd ? plant_k1(bd, proll, states[below], hy, cx * CHUNK_SIZE + x, cz * CHUNK_SIZE + z) : STATE_AIR;
            } else plant = reed_ground && states[below] != C.grass ? STATE_AIR : plant_for(biome_index, proll);
            /* Reeds and sedge: a dense band of tall grass hugging the bank, only on bare ground beside real water. */
            if (plant == STATE_AIR && !kern && !custom_biomes() && C.tall_grass && hy->water_dist < SHORE_DISTANCE && (proll >> 20) % 100 < 45) plant = C.tall_grass;
            if (plant != STATE_AIR) {
                float ps = decor_scale(s, col, PK_PLANT);
                if (ps != 1.0f && (float)((proll >> 40) % 1000) >= 1000.0f * ps) plant = STATE_AIR; /* thinned by the cell's own epoch */
            }
            if (plant != STATE_AIR) states[at] = plant;
        }
}

static bool feature_matches_biome(int biome_index, const GenFeatureDef *feature) {
    if (feature->biome_count <= 0) return true;
    const char *id = biome_id_of(biome_index);
    for (int i = 0; i < feature->biome_count; i++) if (!strcmp(id, feature->biome_ids[i])) return true;
    return false;
}

static void place_features(const GenScratch *s, u16 *states, int cx, int cz) {
    for (int f = 0; f < g_feature_count; f++) {
        const GenFeatureDef *feature = &g_features[f];
        for (int z = 0; z < CHUNK_SIZE; z++)
            for (int x = 0; x < CHUNK_SIZE; x++) {
                int col = (z << 5) | x;
                int biome_index = s->biome[col];
                if (!feature_matches_biome(biome_index, feature)) continue;
                if (!habitat_ok(feature->habitat, feature->water_min, feature->water_max, &s->hydro[col])) continue;
                int wx = cx * CHUNK_SIZE + x, wz = cz * CHUNK_SIZE + z;
                int y = s->height[col];
                if (y < feature->min_y || y > feature->max_y) continue;
                u64 roll = hash3(N.seed ^ 0xF1E5ULL, wx, y, wz);
                if (feature->permille > 0 ? (unsigned)(roll % 1000) >= (unsigned)feature->permille : (unsigned)(roll % 100) >= (unsigned)feature->chance) continue;
                int r = MAX(0, feature->radius);
                for (int dy = -r; dy <= r; dy++)
                    for (int dz = -r; dz <= r; dz++)
                        for (int dx = -r; dx <= r; dx++)
                            if (dx * dx + dy * dy + dz * dz <= r * r + 1) {
                                int wy = y + 1 + dy;
                                int px = wx + dx, pz = wz + dz;
                                put_if_air(states, cx, cz, px, wy, pz, feature->block_state);
                            }
            }
    }
}

static bool structure_matches_biome(int biome_index, const GenStructureDef *structure) {
    if (structure->biome_count <= 0) return true;
    const char *id = biome_id_of(biome_index);
    for (int i = 0; i < structure->biome_count; i++) if (!strcmp(id, structure->biome_ids[i])) return true;
    return false;
}

static bool box_hit(const StructureClaim *c, const int lo[3], const int hi[3]) {
    for (int k = 0; k < 3; k++) if (hi[k] < c->min[k] || lo[k] > c->max[k]) return false;
    return true;
}

/* Records a structure's box.  A column holds few claims, so a repeat of an id merges into one union box (still a
 * conservative exclusion zone); only more than CGM_MAX_CLAIMS different ids overflows. */
static void claim_add(GenScratch *s, const char *id, const int lo[3], const int hi[3], u64 seed) {
    Cgm *c = &s->cgm;
    for (int i = 0; i < c->claim_count; i++)
        if (!strncmp(c->claims[i].id, id, sizeof c->claims[i].id - 1)) {
            for (int k = 0; k < 3; k++) { c->claims[i].min[k] = MIN(c->claims[i].min[k], lo[k]); c->claims[i].max[k] = MAX(c->claims[i].max[k], hi[k]); }
            return;
        }
    StructureClaim cl = {0};
    snprintf(cl.id, sizeof cl.id, "%s", id);
    for (int k = 0; k < 3; k++) { cl.min[k] = lo[k]; cl.max[k] = hi[k]; }
    cl.seed = seed;
    if (cgm_add_claim(c, &cl) != CGM_OK) s->cgm_overflow = true;
}

static void put_forced(u16 *states, int cx, int cz, int wx, int y, int wz, u16 state) {
    int x = wx - cx * CHUNK_SIZE, z = wz - cz * CHUNK_SIZE, ly = y - BAND_LO * CHUNK_SIZE;
    if (x < 0 || x >= CHUNK_SIZE || z < 0 || z >= CHUNK_SIZE || ly < 0 || ly >= (BAND_HI - BAND_LO + 1) * CHUNK_SIZE) return;
    states[((size_t)ly << 10) | (size_t)((z << 5) | x)] = state;
}

static u16 state_at(const u16 *states, int cx, int cz, int wx, int y, int wz) {
    int x = wx - cx * CHUNK_SIZE, z = wz - cz * CHUNK_SIZE, ly = y - BAND_LO * CHUNK_SIZE;
    if (x < 0 || x >= CHUNK_SIZE || z < 0 || z >= CHUNK_SIZE || ly < 0 || ly >= (BAND_HI - BAND_LO + 1) * CHUNK_SIZE) return STATE_MISSING;
    return states[((size_t)ly << 10) | (size_t)((z << 5) | x)];
}

static void place_structures(GenScratch *s, u16 *states, int cx, int cz) {
    for (int st = 0; st < g_structure_count; st++) {
        const GenStructureDef *structure = &g_structures[st];
        int pct_base = structure->chance;
        for (int z = 0; z < CHUNK_SIZE; z++)
            for (int x = 0; x < CHUNK_SIZE; x++) {
                int col = (z << 5) | x;
                int biome_index = s->biome[col];
                if (!structure_matches_biome(biome_index, structure)) continue;
                if (!habitat_ok(structure->habitat, structure->water_min, structure->water_max, &s->hydro[col])) continue;
                int wx = cx * CHUNK_SIZE + x, wz = cz * CHUNK_SIZE + z;
                int y = s->height[col];
                if (y < structure->min_y || y > structure->max_y) continue;
                u64 roll = hash3(N.seed ^ 0xA11CULL, wx, y, wz);
                float sc = decor_scale(s, col, PK_STRUCT);
                int pct = sc == 1.0f ? pct_base : (int)((float)pct_base * sc + 0.5f);
                if ((unsigned)(roll % 100) >= (unsigned)pct) continue;
                int base_y = y + 1;
                const GenHydrologySample *hy = &s->hydro[col];
                bool watery = hy->wet || (hy->flags & GEN_HYD_OCEAN);
                int water_top = (hy->flags & GEN_HYD_OCEAN) ? C.sea_level : (int)floorf(hy->water_y);
                bool policy = s->plan != NULL;
                u16 found = structure->foundation != STATE_MISSING ? structure->foundation : C.stone;
                if (policy && watery && structure->on_water == POL_WATER_RAISE) base_y = water_top + 1;
                int lo[3] = {INT_MAX, INT_MAX, INT_MAX}, hi[3] = {INT_MIN, INT_MIN, INT_MIN};
                for (int b = 0; b < structure->block_count; b++) {
                    const GenStructureBlock *bl = &structure->blocks[b];
                    int p[3] = {wx + bl->x, base_y + bl->y, wz + bl->z};
                    for (int k = 0; k < 3; k++) { lo[k] = MIN(lo[k], p[k]); hi[k] = MAX(hi[k], p[k]); }
                }
                bool skip = false;
                if (policy) {
                    /* Older epochs' structures are exclusion zones; so is a box that overlaps solid ground under reject_structure. */
                    for (int c = 0; c < s->plan->n_claims && !skip; c++) skip = box_hit(&s->plan->claims[c], lo, hi);
                    if (!skip && structure->on_solid == POL_SOLID_REJECT)
                        for (int b = 0; b < structure->block_count && !skip; b++) {
                            const GenStructureBlock *bl = &structure->blocks[b];
                            u16 at = state_at(states, cx, cz, wx + bl->x, base_y + bl->y, wz + bl->z);
                            skip = at != STATE_MISSING && at != STATE_AIR && at != C.water;
                        }
                }
                if (skip) continue;
                if (policy && watery && structure->on_water == POL_WATER_RAISE)
                    for (int b = 0; b < structure->block_count; b++) { /* a pillar of foundation under every footprint column */
                        const GenStructureBlock *bl = &structure->blocks[b];
                        if (bl->y != 0) continue;
                        for (int fy = y; fy < base_y; fy++) put_forced(states, cx, cz, wx + bl->x, fy, wz + bl->z, found);
                    }
                for (int b = 0; b < structure->block_count; b++) {
                    const GenStructureBlock *bl = &structure->blocks[b];
                    int px = wx + bl->x, py = base_y + bl->y, pz = wz + bl->z;
                    if (!policy) { put_if_air(states, cx, cz, px, py, pz, bl->state); continue; }
                    if (structure->on_solid == POL_SOLID_CARVE) { put_forced(states, cx, cz, px, py, pz, bl->state); continue; }
                    if (watery && structure->on_water == POL_WATER_CARVE_FOUNDATION) {
                        if (state_at(states, cx, cz, px, py, pz) == C.water) put_forced(states, cx, cz, px, py, pz, bl->state);
                        else put_if_air(states, cx, cz, px, py, pz, bl->state);
                        if (bl->y == 0) put_forced(states, cx, cz, px, py - 1, pz, found);
                        continue;
                    }
                    put_if_air(states, cx, cz, px, py, pz, bl->state);
                }
                if (policy && watery && structure->on_water == POL_WATER_FLOOD) /* open cells inside the box below the waterline fill */
                    for (int fy = lo[1]; fy <= MIN(hi[1], water_top); fy++)
                        for (int fz = lo[2]; fz <= hi[2]; fz++)
                            for (int fx = lo[0]; fx <= hi[0]; fx++)
                                if (state_at(states, cx, cz, fx, fy, fz) == STATE_AIR) put_forced(states, cx, cz, fx, fy, fz, C.water);
                if (forever_worlds_active()) claim_add(s, structure->id, lo, hi, roll);
            }
    }
}

static void put_if_air(u16 *states, int cx, int cz, int wx, int y, int wz, u16 state) {
    int x = wx - cx * CHUNK_SIZE, z = wz - cz * CHUNK_SIZE, ly = y - BAND_LO * CHUNK_SIZE;
    if (x < 0 || x >= CHUNK_SIZE || z < 0 || z >= CHUNK_SIZE || ly < 0 || ly >= (BAND_HI - BAND_LO + 1) * CHUNK_SIZE) return;
    size_t at = ((size_t)ly << 10) | (size_t)((z << 5) | x);
    if (states[at] == STATE_AIR) states[at] = state;
}

static int tree_percent_base(int biome_index);
static int tree_percent_scaled(int biome_index, float scale) {
    int pct = tree_percent_base(biome_index);
    return scale == 1.0f ? pct : (int)(pct * scale + 0.5f);
}

static int tree_percent_base(int biome_index) {
    if (custom_biomes()) return 0;
    Biome b = (Biome)biome_index;
    switch (b) {
    case BIOME_FOREST: return 100;
    case BIOME_PLAINS: return 8;
    case BIOME_SWAMP: return 35;
    case BIOME_MOUNTAIN: return 30;
    default: return 0;
    }
}

/* Ring cache: a cell inside the chunk reuses the heightmap pass; one outside is computed on first use and kept, so
 * every column is sampled at most once per generated chunk however many trees look at it. */
static int ring_slot(int cx, int cz, int wx, int wz) { return (wz - (cz * CHUNK_SIZE - TREE_RING)) * TREE_RING_SPAN + (wx - (cx * CHUNK_SIZE - TREE_RING)); }

static void ring_fetch(GenScratch *s, int cx, int cz, int wx, int wz, const GenHydrologySample **hydro, int *biome) {
    int lx = wx - cx * CHUNK_SIZE, lz = wz - cz * CHUNK_SIZE;
    if (lx >= 0 && lx < CHUNK_SIZE && lz >= 0 && lz < CHUNK_SIZE) {
        *hydro = &s->hydro[(lz << 5) | lx];
        *biome = s->biome[(lz << 5) | lx];
        return;
    }
    int i = ring_slot(cx, cz, wx, wz);
    if (!s->ring_valid[i]) {
        gen_hydrology_at((float)wx, (float)wz, &s->ring_hydro[i]);
        if (s->plan) { /* same blended ground and biome the neighbouring column computes for its own cell */
            BlendCell c;
            float h;
            int b;
            blend_surface(s->plan, wx, wz, &s->ring_hydro[i], &h, &b, &c);
            s->ring_hydro[i].ground_y = h;
            s->ring_biome[i] = (u8)b;
            /* A root inside an older column follows that epoch's own rule; a new one follows the current rule. */
            s->ring_tree[i] = c.id[0] != s->plan->current ? param_of(ep_of(c.id[0]), PK_TREE) : param_of(&g_cur_ep, PK_TREE);
        } else {
            s->ring_biome[i] = (u8)biome_index_at((float)wx, (float)wz, s->ring_hydro[i].ground_y, &s->ring_hydro[i]);
            s->ring_tree[i] = param_of(EP(), PK_TREE);
        }
        s->ring_valid[i] = 1;
    }
    *hydro = &s->ring_hydro[i];
    *biome = s->ring_biome[i];
}

static bool tree_ground_ok(GenScratch *s, int cx, int cz, int wx, int wz, const GenHydrologySample *hydro, int biome_index) {
    if (custom_biomes()) return false;
    Biome b = (Biome)biome_index;
    float h = hydro->ground_y;
    if (h <= (float)C.sea_level + 1.0f) return false;
    if (hydro->wet || hydro->channel > 0.1f || hydro->water_dist < 1.5f) return false;
    float tl = treeline_at((float)wx, (float)wz);
    if (s->plan) { BlendCell c; blend_weights(s->plan, wx, wz, &c); tl = TREELINE + fnlGetNoise2D(&N.hills, (float)wx, (float)wz) * LINE_WOBBLE + cell_param(&c, PK_TREELINE); }
    if (b == BIOME_MOUNTAIN && h > tl - TREE_TREELINE_MARGIN) return false;
    int fh = (int)floorf(h);
    static const int off[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (int k = 0; k < 4; k++) {
        const GenHydrologySample *nh; int nb;
        ring_fetch(s, cx, cz, wx + off[k][0], wz + off[k][1], &nh, &nb);
        if (abs((int)floorf(nh->ground_y) - fh) >= STEEP_SLOPE) return false;
    }
    return true;
}

static void build_tree(u16 *states, int cx, int cz, int wx, int wz, int base_y, u64 roll) {
    int trunk = TREE_MIN_TRUNK + (int)((roll >> 8) % TREE_TRUNK_RANGE), top = base_y + trunk;
    for (int y = base_y; y < top; y++) put_if_air(states, cx, cz, wx, y, wz, C.log);
    for (int dy = -2; dy <= 1; dy++) {
        int r = dy <= -1 ? TREE_CANOPY_RADIUS : (dy == 0 ? 1 : 1);
        if (dy == 1) r = 1;
        for (int dz = -r; dz <= r; dz++)
            for (int dx = -r; dx <= r; dx++) {
                bool corner = abs(dx) == r && abs(dz) == r;
                if (corner && r == TREE_CANOPY_RADIUS && ((roll >> (12 + (dx > 0) + 2 * (dz > 0) + 4 * (dy + 2))) & 1)) continue;
                if (dy == 1 && corner) continue;
                put_if_air(states, cx, cz, wx + dx, top + dy, wz + dz, C.leaves);
            }
    }
}

/* ------------------------------------------------ data-driven trees (see trees.c and docs/TREES.md) */

typedef struct TreeCtx { const GenScratch *s; } TreeCtx;

static void tree_climate(float x, float z, float h, const GenHydrologySample *hydro, float *t_out, float *m_out) {
    float t = fnlGetNoise2D(&N.temp, x, z) - (h - 90.0f) * 0.004f;
    float m = fnlGetNoise2D(&N.humid, x, z) + 0.55f * water_wetness(hydro);
    if (!EP()->identity) { t += EP()->temp_bias; m += EP()->humid_bias; }
    *t_out = CLAMP(t * 0.5f + 0.5f, 0.0f, 1.0f);
    *m_out = CLAMP(m * 0.5f + 0.5f, 0.0f, 1.0f);
}

static void tree_ground(const GenScratch *s, int wx, int wz, GenHydrologySample *hy, float *h, int *biome, BlendCell *cell) {
    gen_hydrology_at((float)wx, (float)wz, hy);
    *h = hy->ground_y;
    if (s->plan) {
        blend_surface(s->plan, wx, wz, hy, h, biome, cell);
        hy->ground_y = *h;
    } else *biome = biome_index_at((float)wx, (float)wz, *h, hy);
}

static void tree_sample_column(void *ctx, int wx, int wz, TreeColumn *o) {
    const GenScratch *s = ((TreeCtx *)ctx)->s;
    GenHydrologySample hy;
    BlendCell cell;
    float h;
    int b;
    tree_ground(s, wx, wz, &hy, &h, &b, &cell);
    memset(o, 0, sizeof *o);
    bool ocean = (hy.flags & GEN_HYD_OCEAN) != 0;
    o->biome = b;
    o->ground_y = h;
    o->ground = (int)floorf(h);
    o->wet = hy.wet || ocean;
    o->ocean = ocean;
    o->water_depth = o->wet ? MAX(hy.water_y - h, 0.0f) : 0.0f;
    o->water_dist = hy.water_dist;
    tree_climate((float)wx, (float)wz, h, &hy, &o->temperature, &o->moisture);
    o->scale = s->plan ? (cell.id[0] != s->plan->current ? param_of(ep_of(cell.id[0]), PK_TREE) : param_of(&g_cur_ep, PK_TREE)) : param_of(EP(), PK_TREE);
    o->channel = hy.channel;
    o->blocked = hy.channel > 0.1f && !o->wet;
    if (s->plan) {
        float d = blend_old_distance(s->plan, wx, wz);
        if (d > 0.0f && d < TREE_CLEARANCE) o->blocked = true;
    }
    o->ok = o->wet || h > (float)C.sea_level + 1.0f;
    if (k1()) {
        const BiomeDef *bd = biome_get(b);
        if (bd) {
            o->scale *= bd->tree_density_scale;
            if (biome_has_tag(bd, "treeline")) {
                float tl = TREELINE + fnlGetNoise2D(&N.hills, (float)wx, (float)wz) * LINE_WOBBLE + (s->plan ? cell_param(&cell, PK_TREELINE) : EP()->treeline_offset);
                if (h > tl - TREE_TREELINE_MARGIN) o->alpine = true;
            }
        }
    } else if (!custom_biomes() && (Biome)b == BIOME_MOUNTAIN) {
        float tl = TREELINE + fnlGetNoise2D(&N.hills, (float)wx, (float)wz) * LINE_WOBBLE + EP()->treeline_offset;
        if (s->plan) tl = TREELINE + fnlGetNoise2D(&N.hills, (float)wx, (float)wz) * LINE_WOBBLE + cell_param(&cell, PK_TREELINE);
        if (h > tl - TREE_TREELINE_MARGIN) o->alpine = true;
    }
}

/* Slope and surface block: the same inputs the terrain pass feeds to surface_block, so substrate lists match what is there. */
static void tree_sample_detail(void *ctx, int wx, int wz, TreeColumn *o) {
    const GenScratch *s = ((TreeCtx *)ctx)->s;
    /* Forward differences along x and z: two extra samples instead of four, enough for slope and the steep-rock rule. */
    static const int off[2][2] = {{1, 0}, {0, 1}};
    int fh = o->ground;
    float grad = 0.0f;
    int step = 0;
    for (int k = 0; k < 2; k++) {
        GenHydrologySample nh;
        BlendCell nc;
        float h;
        int nb;
        tree_ground(s, wx + off[k][0], wz + off[k][1], &nh, &h, &nb, &nc);
        step = MAX(step, abs((int)floorf(h) - fh));
        grad = MAX(grad, fabsf(h - o->ground_y));
    }
    o->slope = step >= STEEP_SLOPE ? MAX(grad, 2.0f) : grad;
    BlendCell cell;
    memset(&cell, 0, sizeof cell);
    if (s->plan) { GenHydrologySample hy; float h; int b; tree_ground(s, wx, wz, &hy, &h, &b, &cell); }
    float x = (float)wx, z = (float)wz;
    float detail = fnlGetNoise2D(&N.detail, x * 3.1f, z * 3.1f);
    float treeline = TREELINE + fnlGetNoise2D(&N.hills, x, z) * LINE_WOBBLE + (s->plan ? cell_param(&cell, PK_TREELINE) : EP()->treeline_offset);
    float snowline = SNOWLINE + fnlGetNoise2D(&N.hills, x + 4096.0f, z - 4096.0f) * LINE_WOBBLE + (s->plan ? cell_param(&cell, PK_SNOWLINE) : EP()->snowline_offset);
    float patch = fnlGetNoise2D(&N.patch_a, x, z), speck = fnlGetNoise2D(&N.patch_b, x, z);
    o->surface = surface_block(o->biome, fh, detail, (float)step, o->channel, treeline, snowline, patch, speck, o->water_dist);
}

static bool tree_enabled_here(void) { return trees_enabled() && C.log != STATE_AIR; }

static void tree_put(u16 *states, int cx, int cz, int wx, int y, int wz, u16 state) {
    int x = wx - cx * CHUNK_SIZE, z = wz - cz * CHUNK_SIZE, ly = y - BAND_LO * CHUNK_SIZE;
    if (x < 0 || x >= CHUNK_SIZE || z < 0 || z >= CHUNK_SIZE || ly < 0 || ly >= (BAND_HI - BAND_LO + 1) * CHUNK_SIZE) return;
    u16 *at = &states[((size_t)ly << 10) | (size_t)((z << 5) | x)];
    if (*at == STATE_AIR) *at = state; /* water is never displaced: roots and boughs stop at the waterline */
}

static u16 tree_peek(const u16 *states, int cx, int cz, int wx, int y, int wz) {
    int x = wx - cx * CHUNK_SIZE, z = wz - cz * CHUNK_SIZE, ly = y - BAND_LO * CHUNK_SIZE;
    if (x < 0 || x >= CHUNK_SIZE || z < 0 || z >= CHUNK_SIZE || ly < 0 || ly >= (BAND_HI - BAND_LO + 1) * CHUNK_SIZE) return STATE_MISSING;
    return states[((size_t)ly << 10) | (size_t)((z << 5) | x)];
}

/* Writes one tree into this chunk's block array. Only blocks inside the chunk are written; the rest belong to the
 * neighbouring chunk, which builds the identical tree from the same site seed. */
static void tree_write(const GenScratch *s, u16 *states, int cx, int cz, const TreeInst *t, const TreeShape *sh, const TreeSpecies *sp) {
    int base = t->ground + 1;
    for (int i = 0; i < sh->n; i++) {
        const TreeVoxel *v = &sh->v[i];
        int wx = t->x + v->x, wz = t->z + v->z, wy = base + v->y;
        int lx = wx - cx * CHUNK_SIZE, lz = wz - cz * CHUNK_SIZE;
        if (lx < 0 || lx >= CHUNK_SIZE || lz < 0 || lz >= CHUNK_SIZE) continue;
        const GenHydrologySample *hy = &s->hydro[(lz << 5) | lx];
        /* Logs never stand in water columns, nor hang just over them; boughs clear the surface by a block or two. */
        if (v->kind != TV_LEAF && (hy->wet || (hy->flags & GEN_HYD_OCEAN)) && (float)wy <= hy->water_y + 2.0f) continue;
        switch (v->kind) {
        case TV_LEAF: tree_put(states, cx, cz, wx, wy, wz, sp->leaves); break;
        case TV_LOG: tree_put(states, cx, cz, wx, wy, wz, sp->log); break;
        default: {
            int depth = v->kind == TV_ROOT ? 6 : 2;
            tree_put(states, cx, cz, wx, wy, wz, sp->log);
            bool wet_col = hy->wet || (hy->flags & GEN_HYD_OCEAN);
            for (int k = 1; k <= depth; k++) {
                if (wet_col && (float)(wy - k) <= hy->water_y + 2.0f) break;
                u16 below = tree_peek(states, cx, cz, wx, wy - k, wz);
                if (below != STATE_AIR) break;
                tree_put(states, cx, cz, wx, wy - k, wz, sp->log);
            }
        }
        }
    }
}

/* What the tree sampler reads besides the position. Zero (no caching) while a blend plan is active: its answers depend on the chunk. */
static u64 tree_tag(const GenScratch *s) {
    if (s->plan || T_ep) return 0;
    u64 h = hash64(g_gen_serial ^ 0x7A6u);
    const u8 *b = (const u8 *)&g_cur_ep;
    for (size_t i = 0; i < sizeof g_cur_ep; i++) h = hash64(h ^ b[i]);
    return h | 1u;
}

static void place_trees_data(GenScratch *s, u16 *states, int cx, int cz) {
    static _Thread_local TreeScratch *ts;
    static _Thread_local TreePlan plan;
    if (!ts) ts = trees_scratch_create();
    TreeCtx ctx = {s};
    TreeSampler smp = {&ctx, C.sea_level, tree_sample_column, tree_sample_detail, k1() ? 1 : 0, tree_tag(s)};
    int x0 = cx * CHUNK_SIZE, z0 = cz * CHUNK_SIZE;
    trees_plan(ts, (u64)N.seed, &smp, x0, z0, x0 + CHUNK_SIZE - 1, z0 + CHUNK_SIZE - 1, &plan);
    for (int i = 0; i < plan.n; i++) {
        const TreeInst *t = &plan.t[i];
        const TreeSpecies *sp = trees_species(t->species);
        const TreeShape *shp = tree_generate_cached(sp, tree_seed((u64)N.seed, t->gx, t->gz, t->layer));
        tree_write(s, states, cx, cz, t, shp, sp);
    }
}

/* The column facts tree placement used at (wx, wz); detail adds slope and surface block. For tests and diagnostics. */
void gen_tree_column(GenScratch *s, int wx, int wz, bool detail, TreeColumn *out) {
    s->plan = NULL;
    TreeCtx ctx = {s};
    tree_sample_column(&ctx, wx, wz, out);
    if (detail) tree_sample_detail(&ctx, wx, wz, out);
}

/* Every tree that can touch the box, for diagnostics and tests. Uses the same sampler the chunk pass does. */
void gen_tree_plan(GenScratch *s, int x0, int z0, int x1, int z1, TreePlan *out) {
    static _Thread_local TreeScratch *ts;
    if (!ts) ts = trees_scratch_create();
    s->plan = NULL;
    TreeCtx ctx = {s};
    TreeSampler smp = {&ctx, C.sea_level, tree_sample_column, tree_sample_detail, k1() ? 1 : 0, tree_tag(s)};
    trees_plan(ts, (u64)N.seed, &smp, x0, z0, x1, z1, out);
}

static void place_trees(GenScratch *s, u16 *states, int cx, int cz) {
    if (tree_enabled_here()) { place_trees_data(s, states, cx, cz); return; }
    if (C.log == STATE_AIR || C.leaves == STATE_AIR || custom_biomes()) return;
    int x0 = cx * CHUNK_SIZE - TREE_MARGIN, z0 = cz * CHUNK_SIZE - TREE_MARGIN, span = CHUNK_SIZE + 2 * TREE_MARGIN;
    for (int wz = z0; wz < z0 + span; wz++)
        for (int wx = x0; wx < x0 + span; wx++) {
            u64 roll = hash3(N.seed ^ SALT_TREE, wx, 0, wz);
            if (roll % TREE_LATTICE_ODDS) continue;
            const GenHydrologySample *hydro; int b;
            ring_fetch(s, cx, cz, wx, wz, &hydro, &b);
            float h = hydro->ground_y;
            int lx = wx - cx * CHUNK_SIZE, lz = wz - cz * CHUNK_SIZE;
            bool inside = lx >= 0 && lx < CHUNK_SIZE && lz >= 0 && lz < CHUNK_SIZE;
            float scale = inside ? decor_scale(s, (lz << 5) | lx, PK_TREE) : s->ring_tree[ring_slot(cx, cz, wx, wz)];
            if (s->plan && blend_old_distance(s->plan, wx, wz) > 0.0f && blend_old_distance(s->plan, wx, wz) < TREE_CLEARANCE) continue;
            if ((int)((roll >> 32) % 100) >= tree_percent_scaled(b, scale) || !tree_ground_ok(s, cx, cz, wx, wz, hydro, b)) continue;
            build_tree(states, cx, cz, wx, wz, (int)floorf(h) + 1, roll);
        }
}

/* Decides, for each cell, whether it follows an older epoch's decoration rules or the current one.  Cells deep inside
 * either region are forced; the cut between them is a minimum s-t cut, so the border runs where it disturbs the fewest
 * features: through bare ground, not across a tree root, a river or an older structure's footprint. */
static void seam_decor(GenScratch *s, int cx, int cz) {
    enum { N_ = CHUNK_SIZE };
    static _Thread_local int cap_h[(N_ - 1) * N_], cap_v[N_ * (N_ - 1)];
    u8 forced[CHUNK_AREA], label[CHUNK_AREA];
    bool any_old = false, any_new = false;
    int weight[CHUNK_AREA];
    for (int z = 0; z < N_; z++)
        for (int x = 0; x < N_; x++) {
            int i = (z << 5) | x;
            const BlendCell *c = &s->cell[i];
            s->decor_epoch[i] = c->n > 1 ? c->id[1] : c->id[0];
            float cur_w = c->id[0] == s->plan->current ? c->a[0] : 0.0f;
            forced[i] = cur_w >= 0.85f ? 2 : (cur_w <= 0.15f ? 1 : 0);
            int wx = cx * CHUNK_SIZE + x, wz = cz * CHUNK_SIZE + z;
            int box_lo[3] = {wx, INT_MIN, wz}, box_hi[3] = {wx, INT_MAX, wz};
            for (int k = 0; k < s->plan->n_claims; k++) if (box_hit(&s->plan->claims[k], box_lo, box_hi)) forced[i] = 1;
            any_old |= forced[i] == 1;
            any_new |= forced[i] == 2;
            weight[i] = 2 + (hash3(N.seed ^ SALT_TREE, wx, 0, wz) % TREE_LATTICE_ODDS == 0 ? 12 : 0) + (s->hydro[i].wet ? 8 : 0);
        }
    if (!any_old || !any_new) {
        for (int i = 0; i < CHUNK_AREA; i++) {
            const BlendCell *c = &s->cell[i];
            float cur_w = c->id[0] == s->plan->current ? c->a[0] : 0.0f;
            s->decor_old[i] = forced[i] ? forced[i] == 1 : cur_w < 0.5f;
        }
        return;
    }
    for (int z = 0; z < N_; z++)
        for (int x = 0; x < N_; x++) {
            int i = (z << 5) | x;
            if (x + 1 < N_) cap_h[z * (N_ - 1) + x] = weight[i] + weight[i + 1];
            if (z + 1 < N_) cap_v[z * N_ + x] = weight[i] + weight[i + 32];
        }
    /* The grid handed to the solver is dense 32 x 32; CHUNK_AREA cells are laid out with stride 32 already. */
    seam_mincut(N_, N_, cap_h, cap_v, forced, label);
    for (int i = 0; i < CHUNK_AREA; i++) s->decor_old[i] = label[i];
}

/* Structure carving can strand decorations.  Plants without ground and leaves without a neighbour are removed, then
 * the plant pass runs again so any cell that is valid after the carve gets its plant back. */
static u64 g_orphans_removed;
static void clear_orphans(GenScratch *s, u16 *states, int cx, int cz) {
    int layers = BAND_HI - BAND_LO + 1, H = layers * CHUNK_SIZE;
    for (int z = 0; z < CHUNK_SIZE; z++)
        for (int x = 0; x < CHUNK_SIZE; x++) {
            int col = (z << 5) | x;
            for (int ly = 1; ly < H - 1; ly++) {
                size_t at = ((size_t)ly << 10) | col;
                u16 st = states[at];
                if (st == STATE_AIR) continue;
                u16 below = states[((size_t)(ly - 1) << 10) | col];
                bool plant = (st == C.tall_grass || st == C.flower_red || st == C.flower_yellow || st == C.mushroom || st == C.dead_bush) && st != STATE_AIR;
                if (plant && (below == STATE_AIR || below == C.water)) { states[at] = STATE_AIR; g_orphans_removed++; continue; }
                if ((st == C.leaves && C.leaves != STATE_AIR) || trees_is_leaf_state(st)) {
                    bool lonely = below == STATE_AIR && states[((size_t)(ly + 1) << 10) | col] == STATE_AIR;
                    for (int k = 0; k < 4 && lonely; k++) {
                        int nx = x + (k == 0) - (k == 1), nz = z + (k == 2) - (k == 3);
                        if (nx < 0 || nx >= CHUNK_SIZE || nz < 0 || nz >= CHUNK_SIZE) { lonely = false; break; }
                        lonely = states[((size_t)ly << 10) | (size_t)((nz << 5) | nx)] == STATE_AIR;
                    }
                    if (lonely) { states[at] = STATE_AIR; g_orphans_removed++; }
                }
            }
        }
    place_plants(s, states, cx, cz);
}

static void record_cgm(GenScratch *s) {
    Cgm *c = &s->cgm;
    u8 blob[CGM_MAX_TERRAIN_BLOB];
    const float *src = &g_cur_ep.sea_level; /* the 16 floats of EpochParams, in declaration order */
    memcpy(blob, src, sizeof blob);
    cgm_set_terrain(c, blob, sizeof blob);
    int count[CGM_MAX_BIOME_BLOB] = {0};
    for (int i = 0; i < CHUNK_AREA; i++) if (s->biome[i] < CGM_MAX_BIOME_BLOB) count[s->biome[i]]++;
    u8 w[CGM_MAX_BIOME_BLOB];
    for (int k = 0; k < CGM_MAX_BIOME_BLOB; k++) w[k] = (u8)MIN(255, (count[k] * 255 + CHUNK_AREA / 2) / CHUNK_AREA);
    cgm_set_biomes(c, w, sizeof w);
}

bool gen_column_cgm(const GenScratch *s, Cgm *out, bool *overflow) {
    if (!s->cgm_valid) return false;
    *out = s->cgm;
    *overflow = s->cgm_overflow;
    return true;
}

void gen_column(GenScratch *s, int cx, int cz, u16 *states) {
    bool fw = forever_worlds_active() && epoch_count() > 0;
    s->plan = fw && epoch_current() > 0 ? blend_plan_acquire(cx, cz) : NULL;
    s->cgm_valid = false;
    s->cgm_overflow = false;
    if (fw) { cgm_init(&s->cgm, epoch_current(), (u64)N.seed); }
    int layers = BAND_HI - BAND_LO + 1, y0 = BAND_LO * CHUNK_SIZE, H = layers * CHUNK_SIZE;
    int max_h;
    fill_heightmap(s, cx, cz, &max_h);
    memset(s->ring_valid, 0, sizeof s->ring_valid);
    if (s->plan) seam_decor(s, cx, cz);
    fill_cave_grid(s, cx, cz, y0, layers, max_h);
    for (int z = 0; z < CHUNK_SIZE; z++)
        for (int x = 0; x < CHUNK_SIZE; x++) {
            int col = (z << 5) | x;
            int h = s->height[col];
            int bi = s->biome[col];
            float detail = fnlGetNoise2D(&N.detail, (float)(cx * CHUNK_SIZE + x) * 3.1f, (float)(cz * CHUNK_SIZE + z) * 3.1f);
            float step = 0.0f;
            for (int k = 0; k < 4; k++) {
                int nx = CLAMP(x + (k == 0) - (k == 1), 0, CHUNK_SIZE - 1), nz = CLAMP(z + (k == 2) - (k == 3), 0, CHUNK_SIZE - 1);
                step = MAX(step, (float)abs(s->height[(nz << 5) | nx] - h));
            }
            float wx = (float)(cx * CHUNK_SIZE + x), wz = (float)(cz * CHUNK_SIZE + z);
            const GenHydrologySample *hydro = &s->hydro[col];
            float river = hydro->channel;
            bool ocean = (hydro->flags & GEN_HYD_OCEAN) != 0;
            float treeline = treeline_at(wx, wz);
            float snowline = snowline_at(wx, wz);
            if (s->plan) {
                treeline = TREELINE + fnlGetNoise2D(&N.hills, wx, wz) * LINE_WOBBLE + cell_param(&s->cell[col], PK_TREELINE);
                snowline = SNOWLINE + fnlGetNoise2D(&N.hills, wx + 4096.0f, wz - 4096.0f) * LINE_WOBBLE + cell_param(&s->cell[col], PK_SNOWLINE);
            }
            float ore_scale = decor_scale(s, col, PK_ORE);
            float patch = fnlGetNoise2D(&N.patch_a, wx, wz), speck = fnlGetNoise2D(&N.patch_b, wx, wz);
            bool kern = k1();
            BiomeEnv env = surface_env(0, detail, step, river, hydro->water_dist, treeline, snowline, patch, speck);
            int sub_depth = kern ? subsurface_depth_of(bi) : SUBSURFACE_DEPTH;
            for (int ly = 0; ly < H; ly++) {
                int y = y0 + ly;
                u16 st;
                if (y > h) {
                    st = (ocean && y <= C.sea_level) || (hydro->wet && (float)y <= hydro->water_y) ? C.water : STATE_AIR;
                } else {
                    int depth = h - y;
                    if (depth == 0) st = surface_block(bi, y, detail, step, river, treeline, snowline, patch, speck, hydro->water_dist);
                    else if (depth <= sub_depth) st = kern ? subsurface_k1(bi, y, depth, &env) : subsurface_block(bi);
                    else st = y < C.deep_level ? C.deep : C.stone;
                    if (st == C.stone) {
                        u16 ore = ore_at(cx * CHUNK_SIZE + x, y, cz * CHUNK_SIZE + z, depth, bi, hydro, ore_scale);
                        if (ore != STATE_AIR) st = ore;
                        else if (!custom_biomes() && depth <= STONE_BLOB_DEPTH) {
                            float blob = fnlGetNoise3D(&N.blob, wx, (float)y * 1.3f, wz);
                            if (blob > 0.42f) st = pick(C.andesite, C.stone);
                            else if (blob < -0.42f) st = pick(C.granite, C.stone);
                        }
                    }
                    /* No carving near the surface or on the band floor keeps caves sealed from the sky and from the filler below. */
                    if (depth >= CAVE_MIN_DEPTH && ly > 2 && cave_at(s, x, ly, z) > 0.0f) st = STATE_AIR;
                }
                states[((size_t)ly << 10) | col] = st;
            }
        }
    place_plants(s, states, cx, cz);
    place_trees(s, states, cx, cz);
    place_features(s, states, cx, cz);
    place_structures(s, states, cx, cz);
    if (s->plan) clear_orphans(s, states, cx, cz);
    if (fw) { record_cgm(s); s->cgm_valid = true; }
    if (s->plan) { blend_plan_release((BlendPlan *)s->plan); s->plan = NULL; }
}

/* ------------------------------------------------------- distant voxel tiles */

#define LOD_SEA_SKY_LIGHT 15
#define LOD_WATER_OPACITY 2 /* matches opacity in base:water */

/* A tile of the distant-terrain LOD is one 32 x 32 column of voxels that are 2^shift blocks wide. Heights are the
 * minimum over the voxel footprint, rounded down and then lowered by one voxel, so a tile never rises above the
 * real terrain it overlaps: where the near and far layers overlap, the near one always wins and no far block
 * pokes through. Caves, ores and plants are left out; they are invisible at these distances. */
static void lod_sample_column(int shift, int vx, int vz, float *min_h, float *cx_out, float *cz_out) {
    float s = (float)(1 << shift), lo = 1e9f;
    static const float OFF[2] = {0.25f, 0.75f};
    for (int a = 0; a < 2; a++)
        for (int b = 0; b < 2; b++) lo = MIN(lo, gen_height_at(((float)vx + OFF[a]) * s, ((float)vz + OFF[b]) * s));
    *min_h = lo;
    *cx_out = ((float)vx + 0.5f) * s;
    *cz_out = ((float)vz + 0.5f) * s;
}

void gen_lod_grid(int shift, int cx, int cz, GenLodGrid *g) {
    int s = 1 << shift, sea_top = (int)floorf((float)C.sea_level / (float)s);
    float min_h[LOD_PAD * LOD_PAD];
    int biome[LOD_PAD * LOD_PAD];
    float wx[LOD_PAD * LOD_PAD], wz[LOD_PAD * LOD_PAD], river_channel[LOD_PAD * LOD_PAD], water_dist[LOD_PAD * LOD_PAD];
    g->vmin = INT_MAX;
    g->vmax = sea_top;
    for (int zp = 0; zp < LOD_PAD; zp++)
        for (int xp = 0; xp < LOD_PAD; xp++) {
            int i = zp * LOD_PAD + xp;
            lod_sample_column(shift, cx * CHUNK_SIZE + xp - 1, cz * CHUNK_SIZE + zp - 1, &min_h[i], &wx[i], &wz[i]);
            GenHydrologySample hydro;
            gen_hydrology_at(wx[i], wz[i], &hydro);
            biome[i] = biome_index_at(wx[i], wz[i], min_h[i], &hydro);
            g->top[i] = (int)floorf(min_h[i] / (float)s) - 1;
            river_channel[i] = hydro.channel;
            water_dist[i] = hydro.water_dist;
            bool ocean = (hydro.flags & GEN_HYD_OCEAN) != 0;
            g->water_top[i] = ocean ? sea_top : g->top[i];
            if (hydro.wet) g->water_top[i] = MAX(g->top[i], (int)floorf(hydro.water_y / (float)s));
            if (xp >= 1 && xp <= CHUNK_SIZE && zp >= 1 && zp <= CHUNK_SIZE) {
                g->vmin = MIN(g->vmin, g->top[i]);
                g->vmax = MAX(g->vmax, MAX(g->top[i], g->water_top[i]));
            }
        }
    for (int zp = 0; zp < LOD_PAD; zp++)
        for (int xp = 0; xp < LOD_PAD; xp++) {
            int i = zp * LOD_PAD + xp;
            float step = 0.0f;
            for (int k = 0; k < 4; k++) {
                int nx = CLAMP(xp + (k == 0) - (k == 1), 0, LOD_PAD - 1), nz = CLAMP(zp + (k == 2) - (k == 3), 0, LOD_PAD - 1);
                step = MAX(step, (float)abs(g->top[nz * LOD_PAD + nx] - g->top[i]) * (float)s);
            }
            float detail = fnlGetNoise2D(&N.detail, wx[i] * 3.1f, wz[i] * 3.1f);
            float y = (float)((g->top[i] + 1) * s);
            float warped_x, warped_z;
            terrain_coordinates(wx[i], wz[i], &warped_x, &warped_z);
            g->surf[i] = surface_block(biome[i], (int)y, detail, step, river_channel[i], treeline_at(wx[i], wz[i]), snowline_at(wx[i], wz[i]),
                                   fnlGetNoise2D(&N.patch_a, wx[i], wz[i]), fnlGetNoise2D(&N.patch_b, wx[i], wz[i]), water_dist[i]);
            g->sub[i] = subsurface_block(biome[i]);
        }
}

void gen_lod_fill(int shift, int cy, const GenLodGrid *g, u16 *states) {
    int s = 1 << shift;
    for (int py = 0; py < LOD_PAD; py++) {
        int vy = cy * CHUNK_SIZE + py - 1;
        for (int zp = 0; zp < LOD_PAD; zp++)
            for (int xp = 0; xp < LOD_PAD; xp++) {
                int i = zp * LOD_PAD + xp, top = g->top[i];
                u16 st;
                if (vy > top) st = vy <= g->water_top[i] ? C.water : STATE_AIR;
                else if (vy == top) st = g->surf[i];
                else if (vy >= top - 1) st = g->sub[i];
                else st = vy * s < C.deep_level ? C.deep : C.stone;
                states[(py * LOD_PAD + zp) * LOD_PAD + xp] = st;
            }
    }
}

/* Sky light for a LOD cube: open everywhere except under the sea, where it falls by the water's light opacity per
 * block exactly as the real flood fill does. Without this the far sea bed was fully lit and showed through the
 * translucent water, so the far ocean came out lighter than the near one with a visible seam between them. */
void gen_lod_light(int shift, int cy, const GenLodGrid *g, u16 *light) {
    int s = 1 << shift;
    for (int py = 0; py < LOD_PAD; py++) {
        int vy = cy * CHUNK_SIZE + py - 1;
        for (int zp = 0; zp < LOD_PAD; zp++)
            for (int xp = 0; xp < LOD_PAD; xp++) {
                int i = zp * LOD_PAD + xp;
                bool wet = vy > g->top[i] && vy <= g->water_top[i];
                int depth_blocks = (g->water_top[i] - vy) * s + s / 2;
                int sky = wet ? CLAMP(LOD_SEA_SKY_LIGHT - depth_blocks * LOD_WATER_OPACITY, 0, 15) : 15;
                light[(py * LOD_PAD + zp) * LOD_PAD + xp] = LIGHT_PACK(sky, 0, 0, 0);
            }
    }
}

/* ------------------------------------------------------------ test hooks */

void gen_epoch_sample(const BlendPlan *plan, int wx, int wz, float *h, int *biome, BlendCell *cell) {
    GenHydrologySample hy;
    gen_hydrology_at((float)wx, (float)wz, &hy);
    if (!plan) { *h = hy.ground_y; *biome = biome_index_at((float)wx, (float)wz, *h, &hy); cell->n = 1; cell->a[0] = 1.0f; cell->id[0] = epoch_current(); return; }
    blend_surface(plan, wx, wz, &hy, h, biome, cell);
}

/* Diagnostics and tests: the biome a column gets under a given kernel, without touching the world's epoch state. */
int gen_sample_biome(int kernel, int wx, int wz, float *h_out) {
    GenHydrologySample hy;
    gen_hydrology_at((float)wx, (float)wz, &hy);
    int saved = g_cur_ep.kernel;
    g_cur_ep.kernel = kernel;
    int b = biome_index_at((float)wx, (float)wz, hy.ground_y, &hy);
    g_cur_ep.kernel = saved;
    if (h_out) *h_out = hy.ground_y;
    return b;
}
const char *gen_biome_id(int index) { return biome_id_of(index); }
int gen_biome_table_size(int kernel) { return kernel >= 1 && biome_count() > 0 ? biome_count() : g_biome_count; }

float gen_epoch_ground(int wx, int wz) { GenHydrologySample hy; gen_hydrology_at((float)wx, (float)wz, &hy); return hy.ground_y; }
float gen_epoch_raw(u32 epoch, float x, float z) { return raw_height_with(ep_of(epoch), x, z); }
int gen_epoch_biome(u32 epoch, float x, float z, float h) { GenHydrologySample hy; gen_hydrology_at(x, z, &hy); return biome_with(ep_of(epoch), x, z, h, &hy); }
float gen_epoch_cave(u32 epoch, float x, float y, float z) { const EpochParams *saved = T_ep; T_ep = ep_of(epoch); float v = cave_sample(x, y, z); T_ep = saved; return v; }
float gen_blend_cave(const BlendPlan *plan, float x, float y, float z) { return blend_cave(plan, x, y, z); }
u64 gen_orphans_removed(void) { return g_orphans_removed; }
void gen_test_clear_structures(void) { g_structure_count = 0; }

int gen_test_add_structure(const char *id, int chance, int on_water, int on_solid, u16 foundation, const int (*xyz)[3], const u16 *states, int n) {
    if (g_structure_count >= MAX_WORLDGEN_STRUCTURES || n > 32) return -1;
    GenStructureDef d = {0};
    snprintf(d.id, sizeof d.id, "%s", id);
    d.chance = chance; d.min_y = -1000; d.max_y = 1000; d.habitat = HAB_ANY;
    d.on_water = on_water; d.on_solid = on_solid; d.foundation = foundation;
    for (int i = 0; i < n; i++) d.blocks[i] = (GenStructureBlock){xyz[i][0], xyz[i][1], xyz[i][2], states[i]};
    d.block_count = n;
    g_structures[g_structure_count] = d;
    return g_structure_count++;
}

/* Runs only the structure pass on a hand-made column: every cell has the given ground height, biome and water. */
void gen_test_place_structures(GenScratch *s, const BlendPlan *plan, u16 *states, int cx, int cz, int ground, bool wet, bool ocean) {
    memset(s->decor_old, 0, sizeof s->decor_old);
    s->plan = plan;
    s->cgm_overflow = false;
    cgm_init(&s->cgm, epoch_current(), (u64)N.seed);
    for (int i = 0; i < CHUNK_AREA; i++) {
        s->height[i] = (i16)ground;
        s->biome[i] = 0;
        memset(&s->hydro[i], 0, sizeof s->hydro[i]);
        s->hydro[i].ground_y = (float)ground;
        s->hydro[i].wet = wet;
        if (wet) s->hydro[i].water_y = (float)(ground + 3);
        if (ocean) { s->hydro[i].flags = GEN_HYD_OCEAN; s->hydro[i].wet = false; }
    }
    place_structures(s, states, cx, cz);
    s->plan = NULL;
}

int gen_test_claims(const GenScratch *s, StructureClaim *out, int cap) {
    int n = s->cgm.claim_count < cap ? s->cgm.claim_count : cap;
    for (int i = 0; i < n; i++) out[i] = s->cgm.claims[i];
    return n;
}

void gen_test_clear_orphans(GenScratch *s, u16 *states, int cx, int cz) { clear_orphans(s, states, cx, cz); }
