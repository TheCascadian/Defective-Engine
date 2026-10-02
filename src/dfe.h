#pragma once
/* Internal engine header. Sections are grouped by translation unit. The stable
 * mod-facing surface lives in include/dfe/dfe_api.h, never here. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))
#define PI_F 3.14159265358979f
#define TAU_F 6.28318530717959f
#define DEG2RAD (PI_F / 180.0f)

/* ---------------------------------------------------------------- base.c */

typedef enum { LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR } LogLevel;
void log_msg(LogLevel level, const char *fmt, ...);
void log_set_level(LogLevel level);
/* Keeps the last lines for the in-game console. */
int log_history_count(void);
const char *log_history_line(int index, LogLevel *level);
#define LOGD(...) log_msg(LOG_DEBUG, __VA_ARGS__)
#define LOGI(...) log_msg(LOG_INFO, __VA_ARGS__)
#define LOGW(...) log_msg(LOG_WARN, __VA_ARGS__)
#define LOGE(...) log_msg(LOG_ERROR, __VA_ARGS__)
void die(const char *fmt, ...);

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrfmt(const char *fmt, ...);
size_t mem_peak_rss_bytes(void);
size_t mem_current_rss_bytes(void);

double time_now_s(void);
void sleep_ms(int ms);
int cpu_count(void);

/* Growable array. Zero-initialised struct is a valid empty vector. */
#define VEC(T) struct { T *d; int n, cap; }
#define vec_reserve(v, want) do { \
    int want_ = (want); \
    if ((v).cap < want_) { \
        int cap_ = (v).cap ? (v).cap : 8; \
        while (cap_ < want_) cap_ *= 2; \
        (v).d = xrealloc((v).d, (size_t)cap_ * sizeof(*(v).d)); \
        (v).cap = cap_; \
    } } while (0)
#define vec_push(v, x) do { vec_reserve(v, (v).n + 1); (v).d[(v).n++] = (x); } while (0)
#define vec_free(v) do { free((v).d); (v).d = NULL; (v).n = (v).cap = 0; } while (0)
#define vec_clear(v) ((v).n = 0)

/* String keyed open-addressing map to a u32 value. Keys are copied. */
typedef struct StrMap {
    char **keys;
    u32 *vals;
    u32 cap, count;
} StrMap;
void strmap_init(StrMap *m);
void strmap_free(StrMap *m);
bool strmap_get(const StrMap *m, const char *key, u32 *out);
void strmap_set(StrMap *m, const char *key, u32 val);

u64 hash64(u64 x);
u32 hash_str(const char *s);
u64 hash_str64(const char *s);
/* Deterministic per-coordinate hash, used by world generation so results never depend on thread order. */
u64 hash3(i64 seed, i64 x, i64 y, i64 z);
static inline float hash_to_unit(u64 h) { return (float)(h >> 40) * (1.0f / 16777216.0f); }

typedef struct Rng { u64 s; } Rng;
static inline u64 rng_next(Rng *r) { r->s += 0x9E3779B97F4A7C15ull; return hash64(r->s); }
static inline float rng_float(Rng *r) { return hash_to_unit(rng_next(r)); }
static inline int rng_range(Rng *r, int lo, int hi) { return lo + (int)(rng_next(r) % (u64)(hi - lo + 1)); }

/* Math. Column-major matrices to match GLSL uniforms. */
typedef struct V3 { float x, y, z; } V3;
typedef struct M4 { float m[16]; } M4;
static inline V3 v3(float x, float y, float z) { V3 r = {x, y, z}; return r; }
static inline V3 v3_add(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline V3 v3_sub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline V3 v3_scale(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline float v3_dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V3 v3_cross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static inline float v3_len(V3 a) { return sqrtf(v3_dot(a, a)); }
static inline V3 v3_norm(V3 a) { float l = v3_len(a); return l > 1e-9f ? v3_scale(a, 1.0f / l) : v3(0, 0, 0); }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float smoothstepf(float a, float b, float x) {
    float t = CLAMP((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
static inline int floor_div(int a, int b) { int q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }
static inline int floor_mod(int a, int b) { int r = a % b; return r < 0 ? r + b : r; }
static inline int ifloor(float f) { int i = (int)f; return (float)i > f ? i - 1 : i; }
M4 m4_identity(void);
M4 m4_mul(M4 a, M4 b);
M4 m4_perspective(float fov_y_rad, float aspect, float znear, float zfar);
M4 m4_ortho(float l, float r, float b, float t, float n, float f);
M4 m4_look_dir(V3 eye, V3 dir, V3 up);
M4 m4_translate(V3 t);
M4 m4_inverse(M4 a);
V3 m4_transform_point(M4 m, V3 p);

typedef struct Frustum { float planes[6][4]; } Frustum;
void frustum_from_matrix(Frustum *f, M4 viewproj);
/* Returns false if the box is entirely outside any plane. */
bool frustum_box_visible(const Frustum *f, V3 lo, V3 hi);

/* Threads and sync, thin wrappers so the rest of the code is portable. */
typedef struct Mutex Mutex;
typedef struct Cond Cond;
typedef struct Thread Thread;
Mutex *mutex_create(void);
void mutex_destroy(Mutex *m);
void mutex_lock(Mutex *m);
void mutex_unlock(Mutex *m);
Cond *cond_create(void);
void cond_destroy(Cond *c);
void cond_wait(Cond *c, Mutex *m);
void cond_signal(Cond *c);
void cond_broadcast(Cond *c);
Thread *thread_start(void (*fn)(void *), void *arg);
void thread_join(Thread *t);

/* Job system. Workers run `run`; the main thread later runs `complete` from
 * jobs_pump under a time budget. Lower priority value runs first. */
typedef void (*JobRun)(void *data, int worker);
typedef void (*JobComplete)(void *data);
enum { JOB_KIND_GEN, JOB_KIND_LIGHT, JOB_KIND_MESH, JOB_KIND_SAVE, JOB_KIND_FAR, JOB_KIND_OTHER, JOB_KIND_COUNT };
typedef struct JobKindStats {
    u64 count;
    double total_s;
    double max_s;
} JobKindStats;
void jobs_init(int worker_count);
void jobs_shutdown(void);
int jobs_worker_count(void);
void jobs_submit(int kind, float priority, JobRun run, JobComplete complete, void *data);
/* Runs completion callbacks until the budget (seconds) is spent. Returns how many ran. */
int jobs_pump(double budget_s);
int jobs_queued(void);
int jobs_in_flight(void);
void jobs_wait_idle(void);
JobKindStats jobs_stats(int kind);
void jobs_stats_reset(void);

/* Filesystem helpers and the layered virtual filesystem used for mod overrides. */
u8 *file_read(const char *path, size_t *size);
bool file_write_atomic(const char *path, const void *data, size_t size);
bool dir_make_all(const char *path);
bool path_exists(const char *path);
bool path_is_dir(const char *path);
typedef VEC(char *) StrList;
void strlist_free(StrList *l);
void dir_list(const char *path, StrList *out);
void path_exe_dir(char *out, size_t cap);

void vfs_reset(void);
void vfs_add_root(const char *dir, const char *mod_id);
int vfs_root_count(void);
const char *vfs_root_dir(int index);
const char *vfs_root_mod(int index);
/* Reads the highest-priority file. owner_out receives the owning mod id (may be NULL). */
u8 *vfs_read(const char *rel, size_t *size, const char **owner_out);
bool vfs_exists(const char *rel);
bool vfs_resolve(const char *rel, char *out, size_t cap, const char **owner_out);
/* Lists unique entry names of a directory across all roots (files and sub-directories). */
void vfs_list(const char *rel_dir, StrList *out);
u64 vfs_stamp(void); /* changes whenever any watched file changes, used by hot reload */

/* Percentile helper for benchmark reporting. Sorts a copy. */
double percentile_of(const double *vals, int n, double pct);

/* --------------------------------------------------------------- render.c */

#include <glad/gl.h>

typedef struct Input {
    double mouse_x, mouse_y, mouse_dx, mouse_dy;
    double scroll;
    bool keys[512];
    bool keys_pressed[512];
    bool mouse_buttons[8];
    bool mouse_pressed[8];
    bool mouse_released[8];
    bool cursor_captured;
    char text[64];
    int text_len;
} Input;

typedef struct Window {
    void *handle;
    int width, height;
    int fb_width, fb_height;
    bool vsync;
    bool should_close;
    bool focused;
} Window;

extern Window g_win;
extern Input g_in;
bool window_create(const char *title, int width, int height, bool vsync, bool visible);
void window_destroy(void);
void window_poll(void);
void window_swap(void);
void window_set_cursor_captured(bool captured);
void window_set_vsync(bool vsync);
void window_set_fullscreen(bool fullscreen);
void window_set_title(const char *title);
bool key_down(int key);
bool key_pressed(int key);
const char *gl_info_string(void);

typedef struct Shader {
    GLuint program;
    char name[48];
    GLint loc_cache[24];
    const char *loc_names[24];
    int loc_count;
} Shader;

/* Builds a program from GLSL fragments found through the VFS (shader packs override by path).
 * `defines` is injected after the #version line. Returns false and logs the full compile log on failure. */
bool shader_load(Shader *s, const char *name, const char *vs_path, const char *fs_path, const char *defines);
void shader_destroy(Shader *s);
GLint shader_uniform(Shader *s, const char *name);
void shader_use(Shader *s);

typedef struct Camera {
    V3 pos;
    float yaw, pitch; /* radians, yaw 0 looks down -Z */
    float fov_y;
    float znear, zfar;
    M4 view, proj, viewproj;
    Frustum frustum;
    V3 forward, right, up;
} Camera;
void camera_update(Camera *c, float aspect);

/* 2D batch used by the HUD, menus, console and overlay. Coordinates are pixels, origin top-left. */
bool ui_init(void);
void ui_shutdown(void);
bool ui_load_font(void);
void ui_begin(int width, int height);
void ui_end(void);
void ui_rect(float x, float y, float w, float h, u32 rgba);
void ui_rect_gradient(float x, float y, float w, float h, u32 top_rgba, u32 bottom_rgba);
void ui_line(float x0, float y0, float x1, float y1, float thickness, u32 rgba);
void ui_text(float x, float y, float size, u32 rgba, const char *text);
float ui_text_width(float size, const char *text);
void ui_image(GLuint tex, float x, float y, float w, float h, float u0, float v0, float u1, float v1, u32 rgba);
void ui_flush(void);
void ui_clip(int x, int y, int w, int h); /* w<=0 disables */
static inline u32 rgba(int r, int g, int b, int a) { return (u32)r | ((u32)g << 8) | ((u32)b << 16) | ((u32)a << 24); }

/* Debug overlay pages are registered by whichever module owns the data. */
typedef void (*OverlayPageFn)(float x, float y);
void overlay_add_page(const char *name, OverlayPageFn fn);
void overlay_frame(double frame_dt, double cpu_ms);
void overlay_draw(void);
void overlay_cycle(void);
int overlay_page(void);
bool overlay_visible(void);
void overlay_text_line(float *x, float *y, const char *fmt, ...);

/* Immediate line drawing for debugging and the M1 test scene. */
void debug_lines_init(void);
void debug_line(V3 a, V3 b, u32 rgba);
void debug_lines_flush(const Camera *cam);

/* ----------------------------------------------------------------- stats */

typedef struct FrameStats {
    double frame_ms[8192];
    double cpu_ms[8192];
    int count;
    int draw_calls_last;
    u64 triangles_last;
} FrameStats;
extern FrameStats g_stats;
void stats_record_frame(double dt_ms, double cpu_ms);
bool screenshot_save_ppm(const char *path);

/* ----------------------------------------------------------------- json.c */

typedef enum { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT } JsonType;
typedef struct Json Json;
struct Json {
    JsonType type;
    int line;
    double num;
    bool boolean;
    char *str;
    int count;
    Json **items;
    char **keys; /* objects only, parallel to items */
};
Json *json_parse(const char *text, size_t len, char *err, size_t errcap, int *err_line);
void json_free(Json *j);
const Json *json_get(const Json *obj, const char *key);
int json_len(const Json *j);
const Json *json_at(const Json *j, int i);
double json_num(const Json *obj, const char *key, double def);
int json_int(const Json *obj, const char *key, int def);
bool json_bool(const Json *obj, const char *key, bool def);
const char *json_str(const Json *obj, const char *key, const char *def);
double json_as_num(const Json *v, double def);
const char *json_as_str(const Json *v, const char *def);

typedef struct JsonWriter {
    char *buf;
    size_t len, cap;
    bool needs_comma[32];
    int depth;
    bool after_key;
} JsonWriter;
void jw_begin_obj(JsonWriter *w);
void jw_end_obj(JsonWriter *w);
void jw_begin_arr(JsonWriter *w);
void jw_end_arr(JsonWriter *w);
void jw_key(JsonWriter *w, const char *key);
void jw_str(JsonWriter *w, const char *s);
void jw_num(JsonWriter *w, double v);
void jw_bool(JsonWriter *w, bool v);
void jw_free(JsonWriter *w);

/* ------------------------------------------------------------ registry.c */

#define CHUNK_SIZE 32
#define CHUNK_SHIFT 5
#define CHUNK_AREA 1024
#define CHUNK_VOL 32768
#define MAX_BLOCK_PROPS 4
#define MAX_PROP_VALUES 16
#define MAX_STATES 65535
#define STATE_AIR 0
#define STATE_MISSING 1
#define STATE_UNLOADED 0xFFFF
#define MAX_TEXTURE_LAYERS 1024

enum { DIR_PX, DIR_NX, DIR_PY, DIR_NY, DIR_PZ, DIR_NZ };
extern const int DIR_VEC[6][3];

typedef enum { SHAPE_NONE, SHAPE_CUBE, SHAPE_CROSS, SHAPE_FLUID, SHAPE_MODEL } BlockShape;
typedef enum { LAYER_OPAQUE, LAYER_CUTOUT, LAYER_TRANSLUCENT, LAYER_COUNT } RenderLayer;
typedef enum { TINT_NONE, TINT_GRASS, TINT_FOLIAGE, TINT_WATER } TintKind;

enum {
    BF_SOLID = 1,       /* collides with entities */
    BF_REPLACEABLE = 2, /* placing a block over it replaces it (air, plants, fluids) */
    BF_WIND = 4,        /* vertex-shader sway */
    BF_FLUID = 8,
    BF_RANDOM_TICK = 16,
    BF_NO_ITEM = 32,    /* do not auto-register a block item */
    BF_OPAQUE = 64,     /* derived: full cube that hides neighbouring faces and blocks light */
    BF_CLIMBABLE = 128
};

typedef struct PropDef {
    char name[24];
    int count;
    char values[MAX_PROP_VALUES][16];
} PropDef;

typedef struct BlockDef {
    char name[64];
    char mod[32];
    char file[160]; /* data file for error messages */
    u16 id;
    u16 first_state, state_count, default_state;
    u8 shape, layer, tint;
    u8 tint_mask;   /* faces (bit per DIR_*) that receive the tint */
    u8 opacity;     /* light attenuation 0..15 */
    u8 emit[3];     /* block light colour 0..15 per channel */
    u8 flags;
    float hardness; /* seconds to break by hand, negative means unbreakable */
    char tool[16];
    char drop[64];
    char sound[24];
    int fluid_viscosity; /* ticks per spread step, 0 for non-fluids */
    int fluid_group;
    char tex_name[6][64];
    u16 tex[6]; /* array-texture layers after stitching, order DIR_* */
    int nprops;
    PropDef props[MAX_BLOCK_PROPS];
    char model[64];
    char item_name[64];
    float friction;
    int light_filter; /* reserved for per-state opacity variants */
} BlockDef;

extern BlockDef *g_blocks[];
extern int g_block_count;
extern u16 g_state_block[];
extern u8 g_state_flags[];
extern u8 g_state_opacity[];
extern u16 g_state_emit[];

void registry_reset(void);
/* Registers a block. Fails (returns NULL) with a logged message when the name is invalid or the id space is full. */
BlockDef *block_register(const BlockDef *def);
BlockDef *block_find(const char *name);
BlockDef *block_of_state(u16 state);
u16 block_state_with(const BlockDef *b, u16 state, const char *prop, const char *value);
int block_state_prop_index(const BlockDef *b, u16 state, int prop);
void registry_freeze_blocks(void);
/* Parses data/NS/blocks/ JSON files from every mounted mod. Returns the number of errors reported. */
int registry_load_blocks(void);
int registry_load_worldgen_config(void);

static inline bool state_opaque(u16 s) { return (g_state_flags[s] & BF_OPAQUE) != 0; }
static inline bool state_solid(u16 s) { return (g_state_flags[s] & BF_SOLID) != 0; }

/* Texture array assembled from every mod's block textures. */
typedef struct TextureSet {
    int tile_size;
    int layer_count;
    GLuint gl_array;
    GLuint gl_anim; /* RG8 per layer: frame count, frames per 4 seconds */
    StrMap name_to_layer;
} TextureSet;
extern TextureSet g_tex;
/* Loads and stitches every texture the registry references. Needs a GL context for the upload. */
bool textures_build(void);
u16 texture_layer_lookup(const char *res_id);
void textures_destroy(void);

/* Data errors name the mod, file and line, and say how to fix the problem. They are also kept for the load screen. */
void data_error(const char *mod, const char *file, int line, const char *fmt, ...);
int data_error_count(void);
const char *data_error_text(int i);
void data_error_reset(void);

/* World save identity of block names, written with each world so ids never leak into save data. */
typedef struct BlockNameTable {
    VEC(char *) names;
} BlockNameTable;
void block_table_save_names(BlockNameTable *t);
u16 block_table_remap_state(const BlockNameTable *saved, u32 saved_block_index, u32 state_index);
void block_table_free(BlockNameTable *t);

/* --------------------------------------------------------------- world.c */

#define CF_GENERATED 1u
#define CF_MESH_DIRTY 2u
#define CF_SAVE_DIRTY 4u
#define CF_MESH_PENDING 8u
#define CF_HAS_MESH 16u
#define CF_VIRTUAL 32u   /* no stored data, behaves as uniform air or filler */
#define CF_MESHED_ONCE 64u

typedef struct MeshSlot {
    i32 page;       /* arena page, -1 when empty */
    u32 first;      /* first vertex, multiple of the granule size */
    u32 count;      /* vertex count */
    u32 granules;
} MeshSlot;

typedef struct Chunk {
    i32 cx, cy, cz;
    u16 uniform;   /* the state when palette is NULL */
    u16 pal_n;
    u8 bits;       /* bits per palette index: 1, 2, 4, 8 or 16 (direct) */
    u16 *pal;
    u32 *data;
    u16 light_uniform;
    u16 *light;    /* NULL when every block has light_uniform */
    u32 flags;
    u16 conn;      /* face connectivity, bit per face pair, see mesher.c */
    u32 mesh_version;
    u32 vis_frame;
    u8 vis_entry;  /* faces the visibility walk entered through */
    MeshSlot mesh[LAYER_COUNT];
} Chunk;

typedef struct Column {
    i32 cx, cz;
    i32 lo_cy, hi_cy;   /* generated band, inclusive */
    u16 deep_state;     /* what lies below the band */
    u32 flags;          /* generation serial while pending */
    u8 state;           /* COLUMN_* */
} Column;
enum { COLUMN_PENDING = 1, COLUMN_READY = 2 };

typedef struct WorldStats {
    int columns_loaded, columns_pending, chunks_loaded, chunks_meshed;
    int mesh_pending, light_queue;
    int columns_missing, chunks_unmeshed;
    u64 columns_generated, chunks_meshed_total;
    u64 vertices_resident;
} WorldStats;

void world_init(u64 seed);
void world_shutdown(void);
u64 world_seed(void);
Chunk *world_chunk(int cx, int cy, int cz);
Column *world_column(int cx, int cz);
/* Returns STATE_UNLOADED when the column is not loaded. Bands above and below read as air and filler. */
u16 world_get_state(int x, int y, int z);
u16 world_get_light(int x, int y, int z);
/* Low-level edit: updates storage, light and dirty flags. Gameplay events are the server's job. */
bool world_set_state(int x, int y, int z, u16 state);
void world_set_light_raw(int x, int y, int z, u16 light);
/* Streaming: schedules generation, meshing and unloading around the focus point. */
void world_stream(V3 focus, V3 forward, int render_distance, bool first_load);
void world_mark_mesh_dirty(Chunk *c);
void world_mark_neighbours_dirty(int cx, int cy, int cz);
void world_stats(WorldStats *out);
/* True once every column and mesh inside the render distance is built and lighting has settled. */
bool world_ready(void);
/* Materialises a chunk that currently only exists virtually so it can be edited. */
Chunk *world_chunk_materialize(int cx, int cy, int cz);
void world_each_chunk(void (*fn)(Chunk *c, void *user), void *user);
void world_flush_generation(int cx, int cz, int radius);
/* Called by light.c after it rewrites a voxel so affected meshes are rebuilt. */
void world_light_touched(int x, int y, int z);

/* Palette storage. Index layout is (y << 10) | (z << 5) | x. */
Chunk *chunk_create(int cx, int cy, int cz);
void chunk_destroy(Chunk *c);
u16 chunk_get(const Chunk *c, int idx);
void chunk_set(Chunk *c, int idx, u16 state);
void chunk_pack_from(Chunk *c, const u16 *flat);
void chunk_unpack(const Chunk *c, u16 *flat);
void chunk_compact(Chunk *c);
size_t chunk_memory_bytes(const Chunk *c);
u16 chunk_get_light(const Chunk *c, int idx);
void chunk_set_light_from(Chunk *c, const u16 *flat);
void chunk_set_light(Chunk *c, int idx, u16 light);

/* ---------------------------------------------------------------- light.c */

#define LIGHT_SKY(l) (((l) >> 12) & 15)
#define LIGHT_R(l) (((l) >> 8) & 15)
#define LIGHT_G(l) (((l) >> 4) & 15)
#define LIGHT_B(l) ((l) & 15)
#define LIGHT_PACK(s, r, g, b) ((u16)(((s) << 12) | ((r) << 8) | ((g) << 4) | (b)))
#define LIGHT_FULL_SKY LIGHT_PACK(15, 0, 0, 0)

/* Column-local lighting used by generation workers. `states` is the whole column, H = chunks * 32 layers. */
void light_init_column(const u16 *states, int chunk_layers, u16 *light_out);
/* Main-thread incremental propagation across chunks. */
void light_on_block_changed(int x, int y, int z, u16 old_state, u16 new_state);
void light_seed_column_borders(int cx, int cz);
/* Processes queued propagation; returns remaining queue length. */
int light_process(int max_steps);
int light_queue_size(void);

/* ----------------------------------------------------------------- gen.c */

typedef struct GenScratch GenScratch;
void gen_init(u64 seed);
void gen_shutdown(void);
GenScratch *gen_scratch_create(void);
void gen_scratch_destroy(GenScratch *s);
void gen_band(int *lo_cy, int *hi_cy);
u16 gen_deep_state(void);
/* Fills `states` (H*1024 entries, index (ylayer<<10)|(z<<5)|x) for the column band. */
void gen_column(GenScratch *s, int cx, int cz, u16 *states);
int gen_sea_level(void);
/* Cheap analytic queries used by the far terrain layer. */
float gen_height_at(float x, float z);
u32 gen_far_color_at(float x, float z, float height);

/* -------------------------------------------------------------- mesher.c */

#define MESH_PAD 34
#define MESH_PAD_VOL (MESH_PAD * MESH_PAD * MESH_PAD)
#define MESH_GRANULE 256
#define MESH_MAX_QUADS_PER_DRAW 16384

typedef struct MeshVertex { u32 a, b; } MeshVertex;

typedef struct MeshInput {
    i32 cx, cy, cz;
    u32 version;
    u16 *states; /* MESH_PAD_VOL entries, index ((y+1)*34 + (z+1))*34 + (x+1) */
    u16 *light;
} MeshInput;

typedef struct MeshOutput {
    i32 cx, cy, cz;
    u32 version;
    MeshVertex *verts[LAYER_COUNT];
    u32 count[LAYER_COUNT];
    u16 conn;
    double build_ms;
} MeshOutput;

void mesh_build(const MeshInput *in, MeshOutput *out);
void mesh_output_free(MeshOutput *out);
void mesh_snapshot(MeshInput *in, int cx, int cy, int cz);
void mesh_input_free(MeshInput *in);
#define CONN_BIT(a, b) (1u << conn_pair_index(a, b))
int conn_pair_index(int a, int b);
static inline bool chunk_faces_connected(u16 conn, int a, int b) { return a == b || (conn & CONN_BIT(a, b)) != 0; }

/* ---------------------------------------------------------------- scene.c */

typedef struct SceneConfig {
    int render_distance;     /* chunks */
    float fov_deg;
    bool occlusion_culling;
    bool wireframe;          /* debug view */
} SceneConfig;
extern SceneConfig g_scene_cfg;

typedef struct SceneStats {
    int chunks_visible, chunks_drawn[LAYER_COUNT], draw_calls;
    u64 vertices_drawn;
    int uploads_this_frame;
    u64 upload_bytes_total;
    int arena_pages;
    double arena_used_mb;
    int chunks_in_range, chunks_culled_frustum, chunks_culled_occlusion;
} SceneStats;
extern SceneStats g_scene_stats;

bool scene_init(void);
void scene_shutdown(void);
void scene_resize(int w, int h);
/* Uploads a finished mesh into the arena and swaps it with the previous one. */
void scene_upload_mesh(Chunk *c, MeshOutput *out);
void scene_free_chunk(Chunk *c);
void scene_render(const Camera *cam, double time_s);
bool scene_reload_shaders(void);
void scene_mesh_job_complete_hook(void);

/* ------------------------------------------------------------ selftest.c */

void selftest_check(bool ok, const char *expr, const char *file, int line);
#define CHECK(x) selftest_check((x), #x, __FILE__, __LINE__)
/* Runs every registered self-test group. Returns the number of failed checks. */
int selftest_run(void);

/* ---------------------------------------------------------------- main.c */

typedef struct Options {
    bool benchmark;
    bool selftest;
    bool no_vsync;
    bool hidden_window;
    bool no_render; /* benchmark without GL, measures CPU side only */
    int width, height;
    int bench_seconds;
    char mods_dir[512];
    char assets_dir[512];
    char world_name[64];
    u64 seed;
    bool seed_set;
    int render_distance;
    char preset[16];
    char screenshot_path[256];
    int screenshot_frame;
    int overlay_page;
    int workers;             /* 0 = automatic */
    bool wireframe;
} Options;
extern Options g_opt;
