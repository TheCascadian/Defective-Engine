/* Data-driven trees: species loaded from data/NS/trees/ *.json, a pure per-region placement plan, and per-tree shape
 * generators. Nothing here knows about chunks: gen.c supplies column samples through TreeSampler and writes the voxels. */
#ifndef DFE_TREES_H
#define DFE_TREES_H

#include "dfe.h"

#define TREE_MAX_SPECIES 64
#define TREE_REACH 15       /* a tree never extends further than this many blocks sideways from its trunk */
#define TREE_MAX_HEIGHT 40  /* trunk height limit, blocks */
#define TREE_BELOW 4        /* grid rows kept below the root cell (stilt roots, hollows) */
#define TREE_ABOVE 12       /* canopy rows allowed above the tallest trunk */
#define TREE_LAYERS 2       /* 0 = canopy trees, 1 = understory and deadwood */
#define TREE_BIOME_SLOTS 32

typedef enum {
    TS_DECIDUOUS_BROADLEAF, TS_DECIDUOUS_SLENDER, TS_CONIFER_PINE, TS_CONIFER_FIR, TS_PALM, TS_WILLOW, TS_ACACIA,
    TS_MANGROVE, TS_BUSH, TS_DEAD_SNAG, TS_FALLEN_LOG, TS_CUSTOM, TS_TYPE_COUNT
} TreeShapeType;

typedef enum { CS_IRREGULAR_SPHERE, CS_CONICAL, CS_UMBRELLA, CS_DROOPING, CS_LAYERED, CS_CLUSTERED, CS_FROND, CS_NONE, CS_COUNT } CanopyShape;

typedef struct TRange { float lo, hi; } TRange;

typedef struct TreeSpecies {
    char id[64], owner[64], file[160];
    char log_name[64], leaves_name[64], sapling_name[64];
    u16 log, leaves;
    int biome_count;
    char biomes[8][64];
    bool biome_ok[TREE_BIOME_SLOTS]; /* filled by trees_bind_biomes; all true when no biome list is given */
    TRange temperature, moisture, light;
    int substrate_count;
    u16 substrate[12];
    int min_y, max_y;
    float max_slope;
    float water_min, water_max;
    bool prefer_near, allow_shallow;
    int min_radius, cluster_size;
    float clustering, density;
    int succession; /* 0 pioneer, 1 mid, 2 climax */
    int layer;
    TreeShapeType type;
    CanopyShape canopy;
    TRange height, trunk_radius, trunk_lean, trunk_taper, trunk_curve, branch_count, branch_angle, branch_length_ratio;
    TRange canopy_radius, canopy_density, canopy_flatness, root_flare, buttress;
    float leaf_clumping, fork_chance;
    int reach; /* conservative horizontal reach of any instance, blocks */
    int kernel; /* "kernel" in the species file: first terrain kernel that may use it; later kernels never disturb earlier ones */
    int salt;   /* grove-noise salt: rank among species of the same kernel class, so a species added for kernel 1 leaves kernel 0 untouched */
} TreeSpecies;

/* ------------------------------------------------------------------ species data */

void trees_reset(void);
/* Loads every data/NS/trees/ *.json. Returns the number of data errors added; an invalid species is skipped. */
int trees_load(void);
/* Parses one species file (used by the loader and by tests). Returns true when the species was accepted. */
bool trees_load_text(const char *text, size_t len, const char *rel, const char *owner);
/* ids[i] is the biome with index i. explicit_only: species without a biome list do not grow (custom biome sets). */
void trees_bind_biomes(const char *const *ids, int count, bool explicit_only);
int trees_species_count(void);
const TreeSpecies *trees_species(int i);
const TreeSpecies *trees_find(const char *id);
bool trees_is_leaf_state(u16 state);
/* Off: gen.c falls back to the original single-shape trees (kept for the benchmark and for worlds with no tree data). */
void trees_set_enabled(bool on);
bool trees_enabled(void);
int trees_max_reach(void);

/* ---------------------------------------------------------------------- shapes */

enum { TV_LEAF = 1, TV_LOG = 2, TV_ROOT = 3, TV_FALLEN = 4 };
/* TV_ROOT logs continue downward through air and water to the ground (up to 6 blocks); TV_FALLEN logs up to 2. */

typedef struct TreeVoxel { i8 x, y, z; u8 kind; } TreeVoxel;

typedef struct TreeShape {
    const TreeVoxel *v; /* relative to the root cell: y = 0 is the first block above the ground */
    int n;
    int min_x, max_x, min_y, max_y, min_z, max_z;
    int height;         /* trunk height asked for, blocks */
    int measured_height; /* highest log + 1 */
    int branch_count;
    int logs, leaves, clipped;
    float trunk_radius, lean, canopy_radius, canopy_density, root_flare;
} TreeShape;

/* Seed of one tree: world seed, the tree's site cell and the layer. It never mentions the chunk being generated, which
 * is what lets two chunks build the same tree. */
u64 tree_seed(u64 world_seed, int gx, int gz, int layer);
/* Builds one tree. The result lives in a thread-local buffer valid until the next call on the same thread. */
const TreeShape *tree_generate(const TreeSpecies *sp, u64 seed);
/* Same result as tree_generate, from a small per-thread cache of recent trees (sp must come from trees_species). The result
 * stays valid until the next cached call on the same thread that hits the same slot, so write it out immediately. */
const TreeShape *tree_generate_cached(const TreeSpecies *sp, u64 seed);

/* --------------------------------------------------------------------- placement */

typedef struct TreeColumn {
    bool ok;        /* false: nothing can stand here */
    bool wet, ocean, alpine, blocked;
    int biome, ground;     /* ground = floor(ground_y); the first tree block is at ground + 1 */
    float ground_y, water_depth, water_dist;
    float temperature, moisture; /* 0..1 */
    float scale;           /* epoch tree-density scale */
    float channel;         /* river channel strength at the column (0 on dry land) */
    float slope;           /* gradient, blocks per block (detail stage) */
    u16 surface;           /* block the column's top layer is made of (detail stage) */
} TreeColumn;

typedef struct TreeSampler {
    void *ctx;
    int sea_level;
    void (*column)(void *ctx, int wx, int wz, TreeColumn *out); /* cheap: ground, biome, climate, water */
    void (*detail)(void *ctx, int wx, int wz, TreeColumn *out); /* dearer: slope and surface block */
    int kernel; /* terrain kernel of the world: species whose "kernel" is higher are never placed */
    u64 tag; /* names everything the callbacks read besides (x, z): equal tags may share cached sites. 0 = never cache */
} TreeSampler;

typedef struct TreeInst { int x, z, ground, gx, gz; u16 species; u8 layer; } TreeInst;
typedef struct TreePlan { TreeInst *t; int n, cap; } TreePlan;
typedef struct TreeScratch TreeScratch;

TreeScratch *trees_scratch_create(void);
void trees_scratch_destroy(TreeScratch *ts);
/* Every tree whose blocks can touch the box [x0,x1] x [z0,z1] (inclusive), in a fixed global order. Pure in
 * (seed, sampler): the answer never depends on the box, on other calls, or on the order they were made in. */
void trees_plan(TreeScratch *ts, u64 seed, const TreeSampler *smp, int x0, int z0, int x1, int z1, TreePlan *out);
void trees_plan_free(TreePlan *p);
/* Placement figures for diagnostics. */
u64 trees_sites_evaluated(void);

#endif
