/* Forever Worlds: generation epochs, chunk generation metadata (CGM), blend plans and seam carving.
 * See docs/FOREVER_WORLDS.md.  Nothing in this header depends on OpenGL or the world; gen.c, world.c and save.c
 * consume it, and the self-tests drive it directly. */
#ifndef DFE_EPOCH_H
#define DFE_EPOCH_H

#include "dfe.h"

/* ------------------------------------------------------------------ GER */

#define EP_MAX_EPOCHS 64
#define EP_BLEND_MAX 4 /* epochs mixed at one block, the current one included */
#define EP_RADIUS_MIN 32.0f
#define EP_RADIUS_MAX 256.0f
#define EP_RADIUS_DEFAULT 96.0f

/* Everything a frozen epoch says about generation.  Kernel "terrain_v1" reads these on top of the engine's noise
 * fields; a default-valued epoch reproduces the engine's original output bit for bit. */
typedef struct EpochParams {
    /* noise.json */
    float sea_level, height_scale, height_offset, cont_bias, erosion_bias, weirdness_bias;
    /* biomes.json */
    float temp_bias, humid_bias;
    /* caves.json */
    float tunnel_width_scale, cheese_threshold_delta;
    /* surface.json */
    float snowline_offset, treeline_offset;
    /* features.json */
    float tree_scale, plant_scale, ore_scale;
    /* structures.json */
    float structure_scale;
    /* biomes.json: 0 = the original if-chain biomes, 1 = data-driven biome files (data/<ns>/biomes).  Kept after the 16
     * floats above, which CGM copies verbatim as the terrain blob. */
    int kernel;
    bool identity; /* every value is at its default */
} EpochParams;

typedef struct Epoch {
    u32 id;
    u8 hash[32];
    EpochParams p;
} Epoch;

void epoch_params_default(EpochParams *p);

/* Loads data/dfe/epochs/<id>/ through the VFS, or from `fs_root/<id>/` on disk when fs_root is not NULL (tests and
 * tools).  Epoch ids must be 0..N-1 with no gaps.  A missing file, an unknown file, a bad value or a hash that does
 * not match hash.txt is a hard error: the call fails, the registry is left empty and `err` names the cause. */
bool epoch_registry_load(const char *fs_root, char *err, size_t err_cap);
void epoch_registry_clear(void);
int epoch_count(void);
const Epoch *epoch_get(u32 id);
u32 epoch_current(void); /* the highest registered id; 0 when the registry is empty */
/* Bumped by every load, clear and CGM edit.  Caches compare it and rebuild when it moves. */
u64 epoch_generation(void);
void epoch_invalidate(void);

/* SHA-256 of an epoch directory's six data files (names in byte order, each as name NUL length-LE64 bytes). */
bool epoch_hash_dir(const char *dir, u8 out[32], char *err, size_t err_cap);
void epoch_hex(const u8 *h, size_t n, char *out); /* out holds 2n+1 chars */
void sha256_bytes(const void *data, size_t len, u8 out[32]);

float epoch_blend_radius(void);
void epoch_set_blend_radius(float r); /* clamped to [EP_RADIUS_MIN, EP_RADIUS_MAX] */

/* ------------------------------------------------------------------ CGM */

#define CGM_MAX_CLAIMS 8
#define CGM_MAX_TERRAIN_BLOB 64
#define CGM_MAX_BIOME_BLOB 16
#define CGM_MAX_BYTES 1024

typedef struct StructureClaim {
    char id[32];
    i32 min[3], max[3]; /* world bounding box, inclusive */
    u64 seed;
} StructureClaim;

typedef struct Cgm {
    u32 epoch_id;
    u8 epoch_hash[32];
    u64 seed_snapshot;
    u8 terrain_len, biome_len, claim_count;
    u8 terrain_params[CGM_MAX_TERRAIN_BLOB]; /* per-column noise deviations from the epoch defaults, normally empty */
    u8 biome_weights[CGM_MAX_BIOME_BLOB];    /* probability of each biome at the column centre, 0..255 */
    StructureClaim claims[CGM_MAX_CLAIMS];
} Cgm;

typedef enum { CGM_OK, CGM_ERR_OVERFLOW, CGM_ERR_FORMAT, CGM_ERR_EPOCH, CGM_ERR_HASH } CgmStatus;
const char *cgm_status_text(CgmStatus s);

void cgm_init(Cgm *c, u32 epoch_id, u64 seed); /* stamps the epoch's hash from the registry */
CgmStatus cgm_set_terrain(Cgm *c, const void *data, size_t len);
CgmStatus cgm_set_biomes(Cgm *c, const void *data, size_t len);
CgmStatus cgm_add_claim(Cgm *c, const StructureClaim *claim);
/* Wire format: "CGM1", u32 epoch, hash[32], u64 seed, blobs, claims; little endian.  Returns the byte count, 0 when
 * `cap` is too small. */
size_t cgm_serialize(const Cgm *c, u8 *out, size_t cap);
/* Parses and validates against the registry: an unknown epoch or a hash that differs is an error, never ignored. */
CgmStatus cgm_deserialize(const u8 *in, size_t len, Cgm *out);

/* World-level index of every column's CGM.  Writers are on the main thread; workers read. */
void cgm_store_clear(void);
bool cgm_store_get(int cx, int cz, Cgm *out);
bool cgm_store_epoch(int cx, int cz, u32 *epoch);
int cgm_store_count(void);
/* Adds or replaces an entry.  Stored entries are append-only on disk (cgm.dat); the newest record for a column wins. */
bool cgm_store_put(int cx, int cz, const Cgm *c);
/* Opens `dir/cgm.dat`, loading every record.  A record that fails its checksum or validation fails the open. */
bool cgm_store_open(const char *dir, char *err, size_t err_cap);
bool cgm_store_has_file(const char *dir);
void cgm_store_close(void);
/* Columns that were saved before the feature was on count as epoch 0.  The probe says whether a column exists on disk. */
void cgm_store_set_legacy_probe(bool (*probe)(int cx, int cz));

/* ---------------------------------------------------------------- blend */

typedef struct BlendCol { i32 cx, cz; u32 epoch; } BlendCol;

/* The older-epoch columns around one column, frozen at build time.  Weights are a pure function of a block position
 * and this set, so the order columns were generated in cannot change the result. */
typedef struct BlendPlan {
    u32 current;
    float radius;
    int n_cols;
    BlendCol *cols;       /* boundary columns of older epochs inside the window */
    int n_claims;
    StructureClaim *claims; /* claims of older epochs whose box may reach the column */
    int cx, cz;
} BlendPlan;

typedef struct BlendCell {
    int n;                  /* 1 when the block is pure current epoch */
    u32 id[EP_BLEND_MAX];
    float a[EP_BLEND_MAX];  /* sums to 1 */
} BlendCell;

/* Returns NULL when no older column lies within the radius (the common case: nothing to blend). */
BlendPlan *blend_plan_build(int cx, int cz, float radius);
void blend_plan_free(BlendPlan *p);
/* Cached form: shared, reference counted, rebuilt when epoch_generation moves.  Release with blend_plan_release. */
BlendPlan *blend_plan_acquire(int cx, int cz);
void blend_plan_release(BlendPlan *p);
/* wx, wz are block coordinates; the block centre is used. */
void blend_weights(const BlendPlan *p, int wx, int wz, BlendCell *out);
/* Weighted scalar and vector blends of per-epoch samples (the caller evaluates each epoch's field at the block). */
float blend_scalar(const BlendCell *c, const float *per_epoch);
void blend_vector(const BlendCell *c, const float *per_epoch, int dim, float *out); /* per_epoch[i * dim + k] */
/* Distance from a block to the nearest older column, blocks; large when there is none. */
float blend_old_distance(const BlendPlan *p, int wx, int wz);
extern u64 g_blend_plan_builds, g_blend_cache_hits;

/* ----------------------------------------------------------------- seam */

/* Minimum s-t cut on a 4-connected grid by push-relabel.  cap_h holds (w-1)*h capacities between x and x+1,
 * cap_v holds w*(h-1) between y and y+1; forced[] is 0 free, 1 source, 2 sink.  label[] receives 1 for cells on
 * the source side (the maximal source side).  Returns the cut value (flow). */
i64 seam_mincut(int w, int h, const int *cap_h, const int *cap_v, const u8 *forced, u8 *label);

/* ----------------------------------------------------------------- gen */

/* After gen_column: the column's CGM (false when Forever Worlds is off).  *overflow is set when a structure claim did
 * not fit; the caller reports it as a data error. */
bool gen_column_cgm(const GenScratch *s, Cgm *out, bool *overflow);

/* Test hooks (gen.c): sample the blend field and the structure pass directly. */
void gen_epoch_sample(const BlendPlan *plan, int wx, int wz, float *h, int *biome, BlendCell *cell);
float gen_epoch_ground(int wx, int wz);
float gen_epoch_raw(u32 epoch, float x, float z);
int gen_epoch_biome(u32 epoch, float x, float z, float h);
float gen_epoch_cave(u32 epoch, float x, float y, float z);
float gen_blend_cave(const BlendPlan *plan, float x, float y, float z);
u64 gen_orphans_removed(void);
void gen_test_clear_structures(void);
int gen_test_add_structure(const char *id, int chance, int on_water, int on_solid, u16 foundation, const int (*xyz)[3], const u16 *states, int n);
void gen_test_place_structures(GenScratch *s, const BlendPlan *plan, u16 *states, int cx, int cz, int ground, bool wet, bool ocean);
int gen_test_claims(const GenScratch *s, StructureClaim *out, int cap);
void gen_test_clear_orphans(GenScratch *s, u16 *states, int cx, int cz);

#endif
