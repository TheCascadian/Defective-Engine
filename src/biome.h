/* Data-driven biomes (generation kernel 1): one JSON file per biome at data/<namespace>/biomes/<name>.json.
 * See docs/MODDING.md and sdk/schemas/biome.schema.json.  This module parses and evaluates the rules; gen.c owns the
 * noise fields and passes in what a column looks like through BiomeEnv. */
#ifndef DFE_BIOME_H
#define DFE_BIOME_H

#include "dfe.h"

#define BIOME_MAX 32 /* matches MAX_WORLDGEN_BIOMES: a column stores its biome in one byte */
#define BIOME_MAX_LAYERS 24
#define BIOME_MAX_PLANTS 8
#define BIOME_MAX_ON 8
#define BIOME_MAX_TAGS 8
#define BIOME_LEGACY 8 /* the first eight table slots are the original biomes, in the order of the Biome enum in gen.c */

typedef enum { BIOME_ROLE_LAND, BIOME_ROLE_OCEAN, BIOME_ROLE_SHORE } BiomeRole;
/* Same order as Habitat in gen.c. */
typedef enum { BIOME_HAB_DRY, BIOME_HAB_ANY, BIOME_HAB_RIVER, BIOME_HAB_LAKE, BIOME_HAB_SHORE, BIOME_HAB_WETLAND } BiomeHabitat;

/* What a column looks like to a surface rule. */
typedef struct BiomeEnv {
    int y;
    float slope, river, water_dist, treeline, snowline;
    float patch[3]; /* noise "a" (broad patches), "b" (fine speckle), "detail" (ground roughness) */
    float speck_height; /* speck_height: dithered altitude used for snow and tree lines */
} BiomeEnv;

typedef struct BiomeCond {
    float slope_min, slope_max;       /* slope >= min and slope < max */
    float river_min;                  /* river channel strength above this; near_river is river_min 0.4 */
    float water_dist_max;
    float height_min, height_max;     /* block height of the layer being chosen */
    bool above_snowline, above_treeline;
    int patch_count;
    struct { u8 noise; float lo, hi; } patch[3]; /* noise 0 = "a", 1 = "b", 2 = "detail"; lo <= value < hi */
    bool stripe;
    int stripe_period, stripe_lo, stripe_hi;     /* y mod period in [lo, hi) */
} BiomeCond;

typedef struct BiomeLayer {
    u16 state;
    int depth; /* 0 = top layer; n > 0 = down to n blocks below the surface */
    BiomeCond when;
} BiomeLayer;

typedef struct BiomePlant {
    u16 state;
    float chance; /* percent of columns, 0..100, in steps of 0.1 */
    u16 on[BIOME_MAX_ON];
    int on_count;
    int habitat;
    float water_min, water_max;
    bool has_noise;
    u8 noise;
    float noise_lo, noise_hi;
} BiomePlant;

typedef struct BiomeDef {
    char id[64];
    int role, priority;
    float temp_center, temp_extent, humid_center, humid_extent;
    float height_min, height_max, weird_min, weird_max;
    BiomeLayer layers[BIOME_MAX_LAYERS];
    int layer_count, subsurface_depth;
    BiomePlant plants[BIOME_MAX_PLANTS];
    int plant_count;
    float tree_density_scale;
    char tags[BIOME_MAX_TAGS][24];
    int tag_count;
} BiomeDef;

/* Loads every file in data/<ns>/biomes.  The table order is the eight legacy ids first (base:ocean ... base:mountain),
 * then the rest sorted by id, so the same index means the same biome in both kernels.  Returns the number of data
 * errors raised.  `on_arrays` is called once per biome file with its parsed JSON so gen.c can read the feature, ore
 * and structure arrays it owns the formats of. */
typedef void (*BiomeExtraFn)(const Json *root, const char *id, const char *rel, const char *owner);
int biome_load_all(BiomeExtraFn extra);
void biome_reset(void);
int biome_count(void);
const BiomeDef *biome_get(int index);
int biome_find(const char *id);
bool biome_has_tag(const BiomeDef *b, const char *tag);

/* Block for a depth-0 or depth-n layer; STATE_MISSING when no layer matches (the caller falls back). */
u16 biome_surface_state(const BiomeDef *b, const BiomeEnv *env);
u16 biome_sub_state(const BiomeDef *b, int depth, const BiomeEnv *env);

#endif
