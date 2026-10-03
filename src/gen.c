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
    u16 log, leaves, tall_grass, flower_red, flower_yellow, mushroom, dead_bush, coal, iron, gold, diamond;
} GenConfig;

#define MAX_WORLDGEN_BIOMES 32
#define MAX_WORLDGEN_ORES 64
#define MAX_WORLDGEN_FEATURES 32
#define MAX_WORLDGEN_STRUCTURES 32

typedef struct GenFeatureDef {
    char id[64];
    char block_name[64];
    u16 block_state;
    char biome_ids[8][64];
    int biome_count;
    int chance;
    int min_y, max_y;
    int radius;
} GenFeatureDef;

typedef struct GenStructureBlock {
    int x, y, z;
    u16 state;
} GenStructureBlock;

typedef struct GenStructureDef {
    char id[64];
    char biome_ids[8][64];
    int biome_count;
    int chance;
    int min_y, max_y;
    GenStructureBlock blocks[32];
    int block_count;
} GenStructureDef;

static GenBiomeDef g_biomes[MAX_WORLDGEN_BIOMES];
static GenOreDef g_ores[MAX_WORLDGEN_ORES];
static GenFeatureDef g_features[MAX_WORLDGEN_FEATURES];
static GenStructureDef g_structures[MAX_WORLDGEN_STRUCTURES];
static int g_biome_count, g_ore_count, g_feature_count, g_structure_count;

static GenConfig C;
static struct {
    fnl_state cont, mount, ridge, hills, detail, temp, humid, cave_a, cave_b, cheese;
    i64 seed;
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
        const Json *biomes = json_get(o, "biomes");
        g_ores[g_ore_count].biome_count = 0;
        if (biomes && biomes->type == JSON_ARRAY) for (int k = 0; k < MIN(biomes->count, 8); k++) {
            const char *name = json_as_str(biomes->items[k], "");
            if (name[0]) snprintf(g_ores[g_ore_count].biome_ids[g_ores[g_ore_count].biome_count++], sizeof g_ores[g_ore_count].biome_ids[0], "%s", name);
        }
        g_ore_count++;
    }
}

static void parse_feature_array(const Json *items, const char *rel, const char *owner);
static void parse_structure_array(const Json *items, const char *rel, const char *owner);
static void put_if_air(u16 *states, int cx, int cz, int wx, int y, int wz, u16 state);

int gen_biome_count(void) { return g_biome_count; }
int gen_ore_count(void) { return g_ore_count; }
int gen_feature_count(void) { return g_feature_count; }
int gen_structure_count(void) { return g_structure_count; }

int registry_load_worldgen_config(void) {
    int errors_before = data_error_count();
    g_biome_count = g_ore_count = g_feature_count = g_structure_count = 0;
    memset(g_biomes, 0, sizeof g_biomes);
    memset(g_ores, 0, sizeof g_ores);
    memset(g_features, 0, sizeof g_features);
    memset(g_structures, 0, sizeof g_structures);
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
    parse_biome_array(json_get(root, "biomes"), rel, owner);
    parse_ore_array(json_get(root, "ores"), rel, owner);
    parse_feature_array(json_get(root, "features"), rel, owner);
    parse_structure_array(json_get(root, "structures"), rel, owner);
    if (!g_biome_count) add_default_biomes();
    if (!g_ore_count) add_default_ores();
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
    const char *biome_id = g_biomes[biome_index].id;
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
        memset(&g_features[g_feature_count], 0, sizeof g_features[g_feature_count]);
        snprintf(g_features[g_feature_count].id, sizeof g_features[g_feature_count].id, "%s", id);
        snprintf(g_features[g_feature_count].block_name, sizeof g_features[g_feature_count].block_name, "%s", block);
        g_features[g_feature_count].block_state = def->default_state;
        g_features[g_feature_count].chance = json_int(f, "chance", 100);
        g_features[g_feature_count].min_y = json_int(f, "min_y", 0);
        g_features[g_feature_count].max_y = json_int(f, "max_y", 255);
        g_features[g_feature_count].radius = json_int(f, "radius", 1);
        const Json *biomes = json_get(f, "biomes");
        g_features[g_feature_count].biome_count = 0;
        if (biomes && biomes->type == JSON_ARRAY) for (int k = 0; k < MIN(biomes->count, 8); k++) {
            const char *name = json_as_str(biomes->items[k], "");
            if (name[0]) snprintf(g_features[g_feature_count].biome_ids[g_features[g_feature_count].biome_count++], sizeof g_features[g_feature_count].biome_ids[0], "%s", name);
        }
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
        memset(&g_structures[g_structure_count], 0, sizeof g_structures[g_structure_count]);
        snprintf(g_structures[g_structure_count].id, sizeof g_structures[g_structure_count].id, "%s", id);
        g_structures[g_structure_count].chance = json_int(s, "chance", 100);
        g_structures[g_structure_count].min_y = json_int(s, "min_y", 0);
        g_structures[g_structure_count].max_y = json_int(s, "max_y", 255);
        const Json *biomes = json_get(s, "biomes");
        g_structures[g_structure_count].biome_count = 0;
        if (biomes && biomes->type == JSON_ARRAY) for (int k = 0; k < MIN(biomes->count, 8); k++) {
            const char *name = json_as_str(biomes->items[k], "");
            if (name[0]) snprintf(g_structures[g_structure_count].biome_ids[g_structures[g_structure_count].biome_count++], sizeof g_structures[g_structure_count].biome_ids[0], "%s", name);
        }
        g_structures[g_structure_count].block_count = 0;
        for (int b = 0; b < blocks->count; b++) {
            const Json *entry = blocks->items[b];
            if (!entry || entry->type != JSON_ARRAY || entry->count < 4) { data_error(owner, rel, s->line, "structure \"%s\" block entries must be [x, y, z, \"block\"].", id); continue; }
            if (g_structures[g_structure_count].block_count >= 32) { data_error(owner, rel, s->line, "structure \"%s\" has too many blocks; the limit is 32.", id); break; }
            int x = (int)json_as_num(json_at(entry, 0), 0); int y = (int)json_as_num(json_at(entry, 1), 0); int z = (int)json_as_num(json_at(entry, 2), 0);
            const char *name = json_as_str(json_at(entry, 3), NULL);
            BlockDef *def = name ? block_find(name) : NULL;
            if (!def) { data_error(owner, rel, s->line, "structure \"%s\" references unknown block \"%s\".", id, name ? name : "<null>"); continue; }
            g_structures[g_structure_count].blocks[g_structures[g_structure_count].block_count].x = x;
            g_structures[g_structure_count].blocks[g_structures[g_structure_count].block_count].y = y;
            g_structures[g_structure_count].blocks[g_structures[g_structure_count].block_count].z = z;
            g_structures[g_structure_count].blocks[g_structures[g_structure_count].block_count].state = def->default_state;
            g_structures[g_structure_count].block_count++;
        }
        if (g_structures[g_structure_count].block_count > 0) g_structure_count++;
    }
}

static void parse_worldgen_data(void) {
    g_biome_count = g_ore_count = g_feature_count = g_structure_count = 0;
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

void gen_init(u64 seed) {
    int s = (int)(hash64(seed) & 0x7FFFFFFF);
    N.seed = (i64)(hash64(seed ^ 0xDEC0DEull) & 0x7FFFFFFFFFFFull);
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

static int biome_index_at(float x, float z, float h) {
    if (g_biome_count == 0) {
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
    float t = fnlGetNoise2D(&N.temp, x, z) - (h - 90.0f) * 0.004f;
    float m = fnlGetNoise2D(&N.humid, x, z);
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

static void fill_heightmap(GenScratch *s, int cx, int cz, int *max_h) {
    int mh = INT_MIN;
    for (int z = 0; z < CHUNK_SIZE; z++)
        for (int x = 0; x < CHUNK_SIZE; x++) {
            float wx = (float)(cx * CHUNK_SIZE + x), wz = (float)(cz * CHUNK_SIZE + z);
            float h = gen_height_at(wx, wz);
            int i = (z << 5) | x;
            s->height[i] = (i16)floorf(h);
            s->biome[i] = (u8)biome_index_at(wx, wz, h);
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

/* Mountains are meadow below the tree line, rock above it and snow on the upper slopes; any biome shows bare
 * rock where the ground is steep, which is what makes cliffs and ridges read as mountain rather than as a
 * green heap. Distant voxel tiles use the same rules, so near and far terrain are made of the same blocks. */
static u16 surface_block(int biome_index, int y, float detail, bool steep, float treeline, float snowline) {
    if (g_biome_count > 0) {
        const GenBiomeDef *b = &g_biomes[biome_index];
        if (steep && b->surface_state != C.sand && b->surface_state != C.water && b->surface_state != C.sandstone) return C.stone;
        return b->surface_state != STATE_MISSING ? b->surface_state : C.grass;
    }
    Biome b = (Biome)biome_index;
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

static u16 subsurface_block(int biome_index) {
    if (g_biome_count > 0) {
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
#define TREE_MARGIN 2              /* canopy radius, so trees rooted in a neighbour column still reach this one */
#define TREE_MIN_TRUNK 4
#define TREE_TRUNK_RANGE 3
#define TREE_CANOPY_RADIUS 2
#define TREE_TREELINE_MARGIN 4.0f
#define PLANT_GRASS_PERCENT 14
#define PLANT_FLOWER_PERCENT 1
#define PLANT_MUSHROOM_PERCENT 2
#define PLANT_DEAD_BUSH_PERCENT 2

static u16 ore_at(int wx, int y, int wz, int depth, int biome_index) {
    if (depth < ORE_MIN_DEPTH) return STATE_AIR;
    if (g_ore_count > 0) {
        const char *biome_id = g_biome_count > 0 ? g_biomes[biome_index].id : "";
        for (int i = 0; i < g_ore_count; i++) {
            const GenOreDef *def = &g_ores[i];
            if (y < def->min_y || y > def->max_y) continue;
            bool match = true;
            if (def->biome_count > 0) {
                match = false;
                for (int b = 0; b < def->biome_count; b++) if (!strcmp(def->biome_ids[b], biome_id)) { match = true; break; }
            }
            if (!match) continue;
            u64 hh = hash3(N.seed ^ SALT_ORE, (wx + i) >> ORE_CELL_SHIFT, y >> ORE_CELL_SHIFT, (wz + i) >> ORE_CELL_SHIFT);
            unsigned roll = (unsigned)(hh & (ORE_RANGE - 1));
            if (def->rarity <= 0 || (int)(roll % (unsigned)def->rarity) != 0) continue;
            return def->ore_state;
        }
        return STATE_AIR;
    }
    u64 hh = hash3(N.seed ^ SALT_ORE, wx >> ORE_CELL_SHIFT, y >> ORE_CELL_SHIFT, wz >> ORE_CELL_SHIFT);
    unsigned roll = (unsigned)(hh & (ORE_RANGE - 1));
    if (((hh >> 16) & 3) == 0) return STATE_AIR;
    unsigned edge = 0;
    if (C.diamond && y < DIAMOND_MAX_Y && roll < (edge += DIAMOND_ODDS)) return C.diamond;
    if (C.gold && y < GOLD_MAX_Y && roll < (edge += GOLD_ODDS)) return C.gold;
    if (C.iron && y < IRON_MAX_Y && roll < (edge += IRON_ODDS)) return C.iron;
    if (C.coal && y < COAL_MAX_Y && roll < (edge += COAL_ODDS)) return C.coal;
    return STATE_AIR;
}

static u16 plant_for(int biome_index, u64 roll) {
    if (g_biome_count > 0) return STATE_AIR;
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

static void place_plants(const GenScratch *s, u16 *states, int cx, int cz) {
    int y0 = BAND_LO * CHUNK_SIZE, H = (BAND_HI - BAND_LO + 1) * CHUNK_SIZE;
    for (int z = 0; z < CHUNK_SIZE; z++)
        for (int x = 0; x < CHUNK_SIZE; x++) {
            int col = (z << 5) | x, h = s->height[col], ly = h + 1 - y0;
            if (h <= C.sea_level || ly < 1 || ly >= H) continue;
            size_t at = ((size_t)ly << 10) | col, below = ((size_t)(ly - 1) << 10) | col;
            if (states[at] != STATE_AIR) continue;
            int biome_index = s->biome[col];
            bool sandy = states[below] == C.sand;
            if (states[below] != C.grass && !(g_biome_count == 0 && (Biome)biome_index == BIOME_DESERT && sandy)) continue;
            u16 plant = plant_for(biome_index, hash3(N.seed ^ SALT_PLANT, cx * CHUNK_SIZE + x, 0, cz * CHUNK_SIZE + z));
            if (plant != STATE_AIR) states[at] = plant;
        }
}

static bool feature_matches_biome(int biome_index, const GenFeatureDef *feature) {
    if (feature->biome_count <= 0) return true;
    const char *id = g_biomes[biome_index].id;
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
                int wx = cx * CHUNK_SIZE + x, wz = cz * CHUNK_SIZE + z;
                int y = s->height[col];
                if (y < feature->min_y || y > feature->max_y) continue;
                u64 roll = hash3(N.seed ^ 0xF1E5ULL, wx, y, wz);
                if ((unsigned)(roll % 100) >= (unsigned)feature->chance) continue;
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
    const char *id = g_biomes[biome_index].id;
    for (int i = 0; i < structure->biome_count; i++) if (!strcmp(id, structure->biome_ids[i])) return true;
    return false;
}

static void place_structures(const GenScratch *s, u16 *states, int cx, int cz) {
    for (int st = 0; st < g_structure_count; st++) {
        const GenStructureDef *structure = &g_structures[st];
        for (int z = 0; z < CHUNK_SIZE; z++)
            for (int x = 0; x < CHUNK_SIZE; x++) {
                int col = (z << 5) | x;
                int biome_index = s->biome[col];
                if (!structure_matches_biome(biome_index, structure)) continue;
                int wx = cx * CHUNK_SIZE + x, wz = cz * CHUNK_SIZE + z;
                int y = s->height[col];
                if (y < structure->min_y || y > structure->max_y) continue;
                u64 roll = hash3(N.seed ^ 0xA11CULL, wx, y, wz);
                if ((unsigned)(roll % 100) >= (unsigned)structure->chance) continue;
                int base_y = y + 1;
                for (int b = 0; b < structure->block_count; b++) {
                    const GenStructureBlock *block = &structure->blocks[b];
                    int px = wx + block->x, py = base_y + block->y, pz = wz + block->z;
                    put_if_air(states, cx, cz, px, py, pz, block->state);
                }
            }
    }
}

static void put_if_air(u16 *states, int cx, int cz, int wx, int y, int wz, u16 state) {
    int x = wx - cx * CHUNK_SIZE, z = wz - cz * CHUNK_SIZE, ly = y - BAND_LO * CHUNK_SIZE;
    if (x < 0 || x >= CHUNK_SIZE || z < 0 || z >= CHUNK_SIZE || ly < 0 || ly >= (BAND_HI - BAND_LO + 1) * CHUNK_SIZE) return;
    size_t at = ((size_t)ly << 10) | (size_t)((z << 5) | x);
    if (states[at] == STATE_AIR) states[at] = state;
}

static int tree_percent(int biome_index) {
    if (g_biome_count > 0) return 0;
    Biome b = (Biome)biome_index;
    switch (b) {
    case BIOME_FOREST: return 100;
    case BIOME_PLAINS: return 8;
    case BIOME_SWAMP: return 35;
    case BIOME_MOUNTAIN: return 30;
    default: return 0;
    }
}

static bool tree_ground_ok(int wx, int wz, float h, int biome_index) {
    if (g_biome_count > 0) return false;
    Biome b = (Biome)biome_index;
    if (h <= (float)C.sea_level + 1.0f) return false;
    if (b == BIOME_MOUNTAIN && h > treeline_at((float)wx, (float)wz) - TREE_TREELINE_MARGIN) return false;
    int fh = (int)floorf(h);
    static const int off[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (int k = 0; k < 4; k++)
        if (abs((int)floorf(gen_height_at((float)(wx + off[k][0]), (float)(wz + off[k][1]))) - fh) >= STEEP_SLOPE) return false;
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

static void place_trees(u16 *states, int cx, int cz) {
    if (C.log == STATE_AIR || C.leaves == STATE_AIR || g_biome_count > 0) return;
    int x0 = cx * CHUNK_SIZE - TREE_MARGIN, z0 = cz * CHUNK_SIZE - TREE_MARGIN, span = CHUNK_SIZE + 2 * TREE_MARGIN;
    for (int wz = z0; wz < z0 + span; wz++)
        for (int wx = x0; wx < x0 + span; wx++) {
            u64 roll = hash3(N.seed ^ SALT_TREE, wx, 0, wz);
            if (roll % TREE_LATTICE_ODDS) continue;
            float h = gen_height_at((float)wx, (float)wz);
            int b = biome_index_at((float)wx, (float)wz, h);
            if ((int)((roll >> 32) % 100) >= tree_percent(b) || !tree_ground_ok(wx, wz, h, b)) continue;
            build_tree(states, cx, cz, wx, wz, (int)floorf(h) + 1, roll);
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
            int bi = s->biome[col];
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
                    if (st == C.stone) {
                        u16 ore = ore_at(cx * CHUNK_SIZE + x, y, cz * CHUNK_SIZE + z, depth, bi);
                        if (ore != STATE_AIR) st = ore;
                    }
                    /* No carving near the surface or on the band floor keeps caves sealed from the sky and from the filler below. */
                    if (depth >= CAVE_MIN_DEPTH && ly > 2 && cave_at(s, x, ly, z) > 0.0f) st = STATE_AIR;
                }
                states[((size_t)ly << 10) | col] = st;
            }
        }
    place_plants(s, states, cx, cz);
    place_trees(states, cx, cz);
    place_features(s, states, cx, cz);
    place_structures(s, states, cx, cz);
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
    float wx[LOD_PAD * LOD_PAD], wz[LOD_PAD * LOD_PAD];
    g->vmin = INT_MAX;
    g->vmax = sea_top;
    for (int zp = 0; zp < LOD_PAD; zp++)
        for (int xp = 0; xp < LOD_PAD; xp++) {
            int i = zp * LOD_PAD + xp;
            lod_sample_column(shift, cx * CHUNK_SIZE + xp - 1, cz * CHUNK_SIZE + zp - 1, &min_h[i], &wx[i], &wz[i]);
            biome[i] = biome_index_at(wx[i], wz[i], min_h[i]);
            g->top[i] = (int)floorf(min_h[i] / (float)s) - 1;
            if (xp >= 1 && xp <= CHUNK_SIZE && zp >= 1 && zp <= CHUNK_SIZE) {
                g->vmin = MIN(g->vmin, g->top[i]);
                g->vmax = MAX(g->vmax, g->top[i]);
            }
        }
    for (int zp = 0; zp < LOD_PAD; zp++)
        for (int xp = 0; xp < LOD_PAD; xp++) {
            int i = zp * LOD_PAD + xp, step = 0;
            for (int k = 0; k < 4; k++) {
                int nx = CLAMP(xp + (k == 0) - (k == 1), 0, LOD_PAD - 1), nz = CLAMP(zp + (k == 2) - (k == 3), 0, LOD_PAD - 1);
                step = MAX(step, abs(g->top[nz * LOD_PAD + nx] - g->top[i]));
            }
            float detail = fnlGetNoise2D(&N.detail, wx[i] * 3.1f, wz[i] * 3.1f);
            float y = (float)((g->top[i] + 1) * s);
            g->surf[i] = surface_block(biome[i], (int)y, detail, step >= STEEP_SLOPE, treeline_at(wx[i], wz[i]), snowline_at(wx[i], wz[i]));
            g->sub[i] = subsurface_block(biome[i]);
        }
}

void gen_lod_fill(int shift, int cy, const GenLodGrid *g, u16 *states) {
    int s = 1 << shift, sea_top = (int)floorf((float)C.sea_level / (float)s);
    for (int py = 0; py < LOD_PAD; py++) {
        int vy = cy * CHUNK_SIZE + py - 1;
        for (int zp = 0; zp < LOD_PAD; zp++)
            for (int xp = 0; xp < LOD_PAD; xp++) {
                int i = zp * LOD_PAD + xp, top = g->top[i];
                u16 st;
                if (vy > top) st = vy <= sea_top ? C.water : STATE_AIR;
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
        int depth_blocks = C.sea_level - (vy * s + s / 2);
        for (int zp = 0; zp < LOD_PAD; zp++)
            for (int xp = 0; xp < LOD_PAD; xp++) {
                bool wet = vy > g->top[zp * LOD_PAD + xp] && depth_blocks > 0;
                int sky = wet ? CLAMP(LOD_SEA_SKY_LIGHT - depth_blocks * LOD_WATER_OPACITY, 0, 15) : 15;
                light[(py * LOD_PAD + zp) * LOD_PAD + xp] = LIGHT_PACK(sky, 0, 0, 0);
            }
    }
}
