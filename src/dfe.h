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
/* Deletes a file or a whole folder tree without following symlinks. */
bool dir_remove_all(const char *path);
bool path_rename(const char *from, const char *to);
i64 path_mtime(const char *path); /* seconds since the epoch, -1 when missing */
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
void window_request_close(void);
void window_swap(void);
void window_set_cursor_captured(bool captured);
void window_set_vsync(bool vsync);
void window_set_fullscreen(bool fullscreen);
void window_resize(int width, int height);
void window_set_title(const char *title);
bool key_down(int key);
bool key_pressed(int key);
const char *gl_info_string(void);

typedef struct Shader {
    GLuint program;
    char name[48];
    GLint loc_cache[32];
    const char *loc_names[32];
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
void ui_text_plain(float x, float y, float size, u32 rgba, const char *text);
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
    int draw_calls_last;
    u64 triangles_last;
} FrameStats;
extern FrameStats g_stats;

/* perf.c: GPU pass timers, per-frame recording and the benchmark report. */
typedef enum { GPU_OPAQUE, GPU_CUTOUT, GPU_SKY, GPU_WATER, GPU_RAIN, GPU_UI, GPU_POST, GPU_ENTITY, GPU_SECTION_COUNT } GpuSection;
typedef struct FrameSample {
    float frame_ms, cpu_ms, stream_ms, render_ms, swap_ms, gpu_ms, scale;
    float gpu_section_ms[GPU_SECTION_COUNT];
    int draw_calls, uploads;
    u32 vertices;
} FrameSample;
void perf_init(void);
void perf_shutdown(void);
/* Collects the timer results of earlier frames; call once at the top of each rendered frame. */
void perf_gpu_frame_begin(void);
/* Sections must not nest: GL allows one timer query at a time. */
void perf_gpu_begin(GpuSection s);
void perf_gpu_end(void);
void perf_record_frame(const FrameSample *s);
/* Latest finished GPU timings, for the overlay. Valid when perf_gpu_available() is true. */
bool perf_gpu_available(void);
float perf_gpu_latest_ms(GpuSection s);
/* Prints the benchmark summary and writes the optional JSON and CSV files. */
void perf_report(double wall_s, double cold_start_s);

/* In-process benchmark matrix (bench.c): several preset and window size cases run one after another in one launch. */
#define BENCH_PRESET_MAX 32 /* equals PRESET_ID_MAX, which is declared further down */
typedef struct BenchCase { char preset[BENCH_PRESET_MAX]; int width, height; } BenchCase;
/* Parses "preset:WxH,preset:WxH" and repeats the list `runs` times, run after run. Returns false with a message on
 * stderr when the text is malformed or names an unknown preset. */
bool bench_matrix_parse(const char *spec, int runs);
int bench_case_total(void);
const BenchCase *bench_case_at(int index);
/* Switches settings and window size to the case. Leaves the world and camera to the caller. */
void bench_case_apply(const BenchCase *c);
/* perf.c: matrix output. One JSON array holds every case; a text line is printed per case. */
void perf_matrix_begin(void);
void perf_report_case(const BenchCase *c, int index, int total, double wall_s, double cold_start_s);
void perf_matrix_end(void);
void perf_reset_samples(void);
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
Json *json_clone(const Json *src);
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
void json_write(JsonWriter *w, const Json *j);
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
#define FLUID_DEFAULT_REACH 7

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
    int emit_prop;    /* index of the property that gates emission, or -1 when the block always emits */
    char emit_prop_name[24], emit_value[16];
    int fluid_reach;      /* horizontal steps a flow travels from its source */
    bool fluid_infinite;  /* two adjacent sources over solid ground make a third, as in a pond */
    int fluid_level_prop; /* index of the "level" property, -1 for non-fluids */
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
u16 block_parse_state(const char *spec);
bool block_format_state(u16 state, char *out, size_t cap);
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
    u8 *pixels;     /* RGBA of every layer at full size, tile_size squared each */
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
#define CF_PERSISTENT 128u /* column is saved or edited, so light changes must reach the disk too */

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
/* Serialises at most SAVE_COLUMNS_PER_CALL edited columns so the main thread never spends a frame on a large backlog;
 * the rest stay dirty for the next call. Called periodically. */
void world_save_dirty(void);
/* Writes every edited column by repeating world_save_dirty until nothing more can be saved. Called on exit. */
void world_save_all(void);
/* Number of ready columns with unsaved edits. */
int world_dirty_columns(void);
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
/* True when the column is generated and every chunk in it has been meshed at least once. */
bool world_column_meshed(int cx, int cz);
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

/* ----------------------------------------------------------------- save.c */

#define SAVE_INV_SLOTS 36 /* matches INV_SLOTS; inventory.c asserts it */
#define SAVE_BLOCK_NAME_LEN 64
typedef struct SaveMeta {
    bool has_player, flying, dead;
    double x, y, z, day_time;
    float yaw, pitch, health;
    bool has_inventory, creative;
    int selected;
    /* Items are stored by block name so ids and states never leak into save data. */
    char inv_name[SAVE_INV_SLOTS][SAVE_BLOCK_NAME_LEN];
    u8 inv_count[SAVE_INV_SLOTS];
} SaveMeta;
/* Mod-scoped save data. Each mod gets its own JSON object under the active world save. */
bool mod_storage_set(const char *mod_id, const char *key, const Json *value);
const Json *mod_storage_get(const char *mod_id, const char *key);
bool mod_storage_remove(const char *mod_id, const char *key);
typedef struct SavedColumn {
    int lo, hi;
    u16 deep_state;
    Chunk **chunks;
} SavedColumn;
/* Opens or creates a world folder. The seed argument is used only when the world is new. */
bool save_open(const char *dir, u64 default_seed);
bool save_active(void);
u64 save_seed(void);
SaveMeta *save_meta(void);
/* Main thread: serialises the column now and writes it on a worker. */
void save_store_column(const Column *col, Chunk *const *chunks);
/* Worker-safe. Returns true and hands over freshly allocated chunks when the column was saved earlier. */
bool save_load_column(int cx, int cz, SavedColumn *out);
int save_jobs_inflight(void);
void save_close(void);

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
int gen_biome_count(void);
int gen_ore_count(void);
int gen_feature_count(void);
int gen_structure_count(void);
/* Fills `states` (H*1024 entries, index (ylayer<<10)|(z<<5)|x) for the column band. */
void gen_column(GenScratch *s, int cx, int cz, u16 *states);
int gen_sea_level(void);
/* Cheap analytic height, used by the distant-terrain tiles and the benchmark camera. */
float gen_height_at(float x, float z);

/* Distant voxel tiles (lod.c): one 32 x 32 column of voxels, each 2^shift blocks wide, padded by one voxel on
 * every side so the mesher can cull and shade borders without neighbour data. */
#define LOD_PAD 34
typedef struct GenLodGrid {
    int top[LOD_PAD * LOD_PAD];      /* voxel index of the surface voxel */
    u16 surf[LOD_PAD * LOD_PAD], sub[LOD_PAD * LOD_PAD];
    int vmin, vmax;                  /* voxel index range inside the tile that holds any surface or water */
} GenLodGrid;
void gen_lod_grid(int shift, int cx, int cz, GenLodGrid *g);
/* Fills a padded 34^3 state cube for vertical chunk cy of the tile, index ((y+1)*34 + (z+1))*34 + (x+1). */
void gen_lod_fill(int shift, int cy, const GenLodGrid *g, u16 *states);
void gen_lod_light(int shift, int cy, const GenLodGrid *g, u16 *light);

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
    int scale_shift; /* voxels are 2^shift blocks wide: 0 for real chunks, 1..4 for distant LOD tiles */
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
    int far_chunks;          /* voxel LOD terrain beyond the render distance, in chunks, 0 disables */
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
    int lod_tiles_drawn, lod_tiles_total;
} SceneStats;
extern SceneStats g_scene_stats;

bool scene_init(void);
void scene_shutdown(void);
void scene_resize(int w, int h);
/* Uploads a finished mesh into the arena and swaps it with the previous one. */
void scene_upload_mesh(Chunk *c, MeshOutput *out);
void scene_free_chunk(Chunk *c);
/* Arena access for other mesh owners. origin is {x, y, z, scale shift} in blocks. Slots must start with page -1. */
void scene_upload_slots(MeshSlot slots[LAYER_COUNT], MeshOutput *out, const i32 origin[4]);
void scene_release_slots(MeshSlot slots[LAYER_COUNT]);
void scene_render(const Camera *cam, double time_s);
bool scene_reload_shaders(void);
void scene_mesh_job_complete_hook(void);

/* ----------------------------------------------------------------- reload.c */

/* Re-reads shaders, the atmosphere file and presets. A file with errors is reported and the old version keeps
 * running. Returns true when everything reloaded cleanly. Block, texture and script changes need a restart. */
bool hot_reload_now(void);
/* Call once per frame; reloads when a file that was read through the virtual filesystem has changed. */
void hot_reload_poll(double now_s, bool enabled);

/* --------------------------------------------------------------- settings.c */

#define MAX_PRESETS 16
#define PRESET_ID_MAX 32

/* A quality preset, loaded from data/<namespace>/presets/<id>.json. */
typedef struct Preset {
    char id[PRESET_ID_MAX];
    char name[48];
    int render_distance, far_chunks;
    bool clouds, stars, light_shafts, dynamic_resolution;
    float min_scale;         /* lowest render scale the dynamic controller may pick */
    float target_fps;        /* frame rate the controller tries to hold */
} Preset;

/* What the player chose, saved to settings.json. Fields at their "automatic" value follow the preset. */
typedef struct Settings {
    char preset[PRESET_ID_MAX];
    int render_distance;     /* chunks, 0 follows the preset */
    int dynamic_resolution;  /* -1 follows the preset, 0 off, 1 on */
    float render_scale;      /* fixed scale used while dynamic resolution is off, 0.5 to 1 */
    float fov_deg;
    bool vsync;
} Settings;
extern Settings g_settings;

/* The values the renderer actually uses: preset, then settings, then command line. */
typedef struct GraphicsConfig {
    int render_distance, far_chunks;
    bool clouds, stars, light_shafts, dynamic_resolution;
    float min_scale, fixed_scale, target_ms, fov_deg;
    bool vsync;
} GraphicsConfig;
extern GraphicsConfig g_gfx;

int registry_load_presets(void);
int preset_count(void);
const Preset *preset_at(int i);
const Preset *preset_find(const char *id);
void settings_defaults(void);
void settings_load(void);
bool settings_save(void);
/* Recomputes g_gfx from the current preset and settings and pushes it to the scene and atmosphere. */
void gfx_apply(void);

/* menu.c: title screen, pause menu and settings screen. */
bool menu_is_open(void);
bool menu_quit_requested(void);
void menu_set_open(bool open);
/* Escape: settings go back to the pause menu, the pause menu resumes the game. */
void menu_back(void);
void menu_draw(int width, int height);
/* Blocking title screen. Returns false when the player quit. The seed is set only for a new world. */
bool menu_title(char *world_out, size_t cap, u64 *seed_out, bool *seed_set);

/* post.c: offscreen target for dynamic resolution and light shafts. */
bool post_init(void);
void post_shutdown(void);
bool post_reload_shaders(void);
/* Binds the target the scene draws into and sets the viewport. Call before clearing. */
void post_begin_scene(const Camera *cam);
/* Resolves the scene to the window. Call after all world-space drawing and before the 2D layer. */
void post_end_scene(const Camera *cam);
float post_scale(void);
void post_update_controller(double frame_ms, double swap_ms);

/* ------------------------------------------------------------ atmosphere.c */

#define ATMO_MAX_KEYS 16

/* One point of the day cycle. Phase 0 is midnight, 0.25 sunrise, 0.5 noon and 0.75 sunset. */
typedef struct SkyKey {
    float time;
    V3 zenith, horizon, sky_light;
    float ambient;
} SkyKey;

typedef enum Weather { WEATHER_CLEAR, WEATHER_OVERCAST, WEATHER_RAIN, WEATHER_COUNT } Weather;

typedef struct Atmosphere {
    /* From data/<namespace>/atmosphere/default.json. */
    double day_length_s;
    float start_phase;
    float cloud_altitude, cloud_scale, cloud_speed;
    float weather_min_s, weather_max_s, rain_share;
    SkyKey keys[ATMO_MAX_KEYS];
    int key_count;
    bool loaded;
    /* Derived every frame by atmosphere_evaluate. */
    float phase;
    V3 sun_dir, moon_dir;
    V3 zenith, horizon, fog_color, sky_light, sun_color, rain_color;
    V3 shade_dir;
    float shade_strength, ambient, star_alpha, sun_vis, moon_vis;
    /* Smoothed state. cloud_amt and rain_amt chase the weather target, underwater chases the head. */
    Weather weather;
    float cloud_amt, rain_amt, rain_exposure, underwater;
    V3 under_color;          /* fog colour while the eye is inside a fluid */
    bool under_daylit;       /* water dims with the sky, lava does not */
    float weather_timer;
    bool auto_weather;
    bool clouds, stars;      /* quality switches, set by presets */
} Atmosphere;
extern Atmosphere g_atmo;

int registry_load_atmosphere(void);
int atmosphere_reload_data(void);
bool atmosphere_gl_reload_shaders(void);
void atmosphere_init_state(void);
void atmosphere_evaluate(Atmosphere *a, double game_seconds);
void atmosphere_update(double dt, V3 eye);
void atmosphere_set_phase(float phase);
void atmosphere_set_weather(Weather w, bool instant);
const char *atmosphere_phase_name(float phase);
const char *atmosphere_weather_name(Weather w);
bool atmosphere_parse_phase(const char *text, float *phase);
bool atmosphere_parse_weather(const char *text, Weather *w);
bool atmosphere_gl_init(void);
void atmosphere_gl_shutdown(void);
void atmosphere_set_uniforms(Shader *sh);
void atmosphere_adjust_fog(float *start, float *end);
void atmosphere_draw_sky(const Camera *cam, double time_s);
void atmosphere_draw_rain(const Camera *cam, double time_s);


/* ------------------------------------------------------------------ lod.c */

bool lod_init(void);
void lod_shutdown(void);
/* Schedules tile builds, rebuilds the coverage texture and the per-layer draw lists (nearest tile first). */
void lod_update(const Camera *cam, int rd, int far_chunks);
const MeshSlot *const *lod_draw_list(int layer, int *count);
/* Sets the coverage and sea-level uniforms of a chunk shader built with LOD defined. */
void lod_set_uniforms(Shader *sh);

/* ------------------------------------------------------------ selftest.c */

void selftest_check(bool ok, const char *expr, const char *file, int line);
#define CHECK(x) selftest_check((x), #x, __FILE__, __LINE__)
/* Runs every registered self-test group. Returns the number of failed checks. */
int selftest_run(void);

/* ----------------------------------------------------------------- mods.c */

#include "dfe_api.h"

#define MOD_MAX_DEPS 16
#define MOD_MAX_AFTER 8

typedef struct ModDep {
    char id[32];
    char op;      /* '>' minimum version, '=' exact, '^' same major and at least */
    int ver[3];
    bool optional;
} ModDep;

typedef struct ModInfo {
    char id[32], name[64], version[16], dir[1100], manifest[1200];
    int ver[3];
    ModDep deps[MOD_MAX_DEPS];
    int dep_count;
    char load_after[MOD_MAX_AFTER][32];
    int after_count;
    char script[96];     /* Lua entry file relative to the mod folder, empty when the mod has none */
    char plugin[96];     /* shared library relative to the mod folder for this platform, empty when none */
    bool disabled;       /* switched off in mods.json */
    bool failed;         /* did not load, reason is in the error list */
    int order;           /* position in the final load order, -1 when not loaded */
} ModInfo;

/* Finds every folder under dir that holds a mod.json. Problems go to the data error list. */
int mods_discover(const char *dir);
/* Applies mods.json, checks dependencies and versions, then fixes a deterministic load order:
 * base first, then dependencies before dependents, ties broken by id. Returns mods that will load. */
int mods_resolve(void);
/* Mounts loaded mods into the virtual filesystem in load order (later mods override earlier ones). */
void mods_mount(void);
int mods_total(void);
const ModInfo *mods_at(int index);       /* discovery order */
int mods_loaded_count(void);
const ModInfo *mods_loaded_at(int order); /* load order */
const ModInfo *mods_find(const char *id);
void mods_reset(void);
/* Native plugins. They are skipped with a clear message unless allowed. */
void mods_load_plugins(bool allow_native);
void mods_unload_plugins(void);

/* Event bus and command table shared by Lua and native plugins. */
const dfe_api_t *api_get(void);
/* Fires an event. Returns true when a subscriber asked to cancel it. */
bool event_fire(const dfe_event_t *ev);
void events_clear(const char *mod_id); /* drops every subscription and command of a mod, used by reload */
void events_clear_all(void);
/* Runs a console line: a registered command, or the "command" event, or reports unknown. */
void command_run(const char *line);
int command_count(void);
const char *command_name(int i);
const char *command_help(int i);

#define GAME_TICK_DT 0.05
#define GAME_TICK_HZ 20
double game_time_get(void);
void game_time_set(double seconds);
/* Advances simulated time by one fixed step and fires the "tick" event. */
void game_tick(void);
bool game_edit_block(int x, int y, int z, u16 state);

/* ------------------------------------------------------------ console.c */

/* Shows the data error list until the player quits or, when can_continue, presses Enter. Without a window the
 * list goes to stderr. Returns true when the game should go on. */
bool errors_screen(const char *title, bool can_continue);

void console_init(void);
void console_print(const char *fmt, ...);
/* Returns true while the console owns the keyboard. Toggled with the grave key. */
bool console_open(void);
void console_update(void);
void console_draw(int width, int height);

/* ------------------------------------------------------------- script.c */

/* One sandboxed Lua state shared by every mod, with its own instruction budget per call. */
bool script_init(void);
void script_shutdown(void);
/* Runs the entry script of every loaded mod in load order. Returns the number of script errors. */
int script_load_mods(void);
/* Evaluates console input. Returns true if it was handled. */
void script_eval(const char *code);
int script_error_count(void);

/* ------------------------------------------------------------- player.c */

#define PLAYER_WIDTH 0.6f
#define PLAYER_HEIGHT 1.8f
#define PLAYER_EYE 1.62f
#define PLAYER_CROUCH_HEIGHT 1.0f
#define PLAYER_CROUCH_EYE 0.9f
#define PLAYER_MAX_HEALTH 20.0f

typedef struct PlayerInput {
    float forward, strafe; /* -1..1 along the view direction and across it */
    bool jump, jump_pressed, crouch, descend, sprint, toggle_fly;
    float speed_scale;     /* multiplies the walking speed; 0 means 1, entities use it for slow or fast types */
} PlayerInput;

typedef struct Player {
    V3 pos; /* centre of the feet */
    V3 vel;
    float yaw, pitch;
    float render_eye_height, jump_buffer;
    float health, hurt_timer, invulnerability_timer, lava_damage_timer, fall_peak_y;
    bool on_ground, in_water, head_in_water, flying, in_lava, crouched, dead;
    float half_width, height; /* collision box; set by player_init, entities override them */
} Player;

void player_init(Player *p, V3 feet);
V3 player_eye(const Player *p);
V3 player_eye_render(const Player *p);
void player_hurt(Player *p, float damage);
bool player_teleport(Player *p, V3 feet);
void player_respawn(Player *p, V3 feet);
/* Advances the player by dt seconds (at most one physics step of 1/60 s per call is exact; larger dt is split). */
void player_step(Player *p, const PlayerInput *in, float dt);
/* True when a player box with its feet at `feet` overlaps a solid block or an unloaded column. */
bool player_box_blocked(V3 feet);
/* The same test for a box of any size, used by entities. */
bool box_blocked(V3 feet, float half_width, float height);
/* Dry land close to the origin, found from the height function so it works before any chunk exists. */
V3 player_find_spawn(void);
extern Player g_player;

/* ---------------------------------------------------------------- entity.c */

#define MAX_ENTITY_TYPES 64
#define MAX_ENTITIES 256
typedef struct EntityType {
    char id[64];             /* namespace:file, for example base:hopper */
    char name[48];
    float width, height;     /* collision box in blocks */
    float color[3], accent[3]; /* body and head colour, 0..1 */
    float speed;             /* multiplier of the player's walking speed */
    bool wander;
    float lifetime;          /* seconds, 0 for unlimited */
} EntityType;

int registry_load_entities(void);
int entity_type_count(void);
const EntityType *entity_type_at(int i);
/* Returns a handle (>0), or 0 with a warning when the type is unknown or the entity limit is reached. */
int entity_spawn(const char *type_id, V3 pos);
bool entity_remove(int id);
bool entity_position(int id, V3 *out);
int entity_count(void);
void entity_clear(void);
void entity_world_init(u64 seed);
void entity_update(float dt);
bool entity_gl_init(void);
bool entity_reload_shaders(void);
void entity_gl_shutdown(void);
/* Draws every entity the fog leaves visible and returns the number of draw calls issued. */
int entity_draw(const Camera *cam, float fog_start, float fog_end);

/* ----------------------------------------------------------- inventory.c */

#define INV_HOTBAR 9
#define INV_SLOTS 36
#define INV_MAX_STACK 64

typedef struct ItemStack {
    u16 state; /* the default state of the block this item places, STATE_AIR when the slot is empty */
    u8 count;
} ItemStack;

typedef struct Inventory {
    ItemStack slot[INV_SLOTS]; /* the first INV_HOTBAR slots are the hotbar */
    ItemStack cursor;          /* the stack held by the mouse while the inventory screen is open */
    int selected;
} Inventory;
extern Inventory g_inv;
extern bool g_creative;

void inventory_clear(Inventory *inv);
/* Copies the bag to or from the save record. Unknown block names (a removed mod) are dropped on load. */
void inventory_store(const Inventory *inv, bool creative, SaveMeta *m);
void inventory_restore(Inventory *inv, bool *creative, const SaveMeta *m);
/* Fills the hotbar of a new world with a small set of building blocks. */
void inventory_starter(Inventory *inv);
/* Adds as many as fit, stacking first. Returns how many did not fit. */
int inventory_add(Inventory *inv, u16 state, int count);
int inventory_count(const Inventory *inv, u16 state);
/* Takes one item out of a slot. Returns false when it was empty. */
bool inventory_take_one(Inventory *inv, int slot);
/* Mouse semantics of an inventory slot: button 0 picks up, drops or swaps a whole stack, button 1 half or one. */
void inventory_click(Inventory *inv, int slot, int button);
/* Number of block items the registry offers, and the n-th one's state. Skips air and blocks marked "item": false. */
int item_count(void);
u16 item_state_at(int index);
const char *item_name(u16 state);
/* Resolves the block named by a "drops" entry to its default state, STATE_AIR for none. */
u16 item_for_block(const BlockDef *b);

/* ---------------------------------------------------------- interact.c */

typedef struct RayHit {
    bool hit;
    int x, y, z;       /* the block that was hit */
    int face;          /* DIR_* of the face that was hit */
    int px, py, pz;    /* the empty cell in front of that face, where a block would be placed */
    u16 state;
    float dist;
} RayHit;
/* Walks the voxel grid from origin along dir. Fluids are skipped unless hit_fluids. */
bool raycast_blocks(V3 origin, V3 dir, float max_dist, bool hit_fluids, RayHit *out);

#define REACH_DISTANCE 5.5f
#define INTERACT_REPEAT_S 0.20f

typedef struct Interact {
    RayHit target;
    float break_progress; /* 0..1 of the block under the cursor */
    int break_x, break_y, break_z;
    bool breaking;
    float place_cooldown, break_cooldown;
} Interact;
extern Interact g_interact;
/* Reads the mouse and the number keys, posts server messages for edits, and updates the target block. */
void interact_update(const Player *p, float dt, bool active);

/* -------------------------------------------------------------- server.c */

/* The server owns the world rules. The client posts requests; the server validates them against the player's
 * reach and the block rules, runs the cancellable events, and applies them. Requests are applied the same frame
 * they are posted, while simulation (fluids, random ticks) advances in fixed ticks. */
typedef enum { MSG_BREAK, MSG_PLACE } ServerMsgType;
typedef struct ServerMsg {
    ServerMsgType type;
    int x, y, z;
    u16 state; /* MSG_PLACE: the block to place, taken from the player's selected slot */
} ServerMsg;
void server_init(void);
void server_shutdown(void);
void server_post(const ServerMsg *m);
/* Applies every queued message. Returns how many changed the world. */
int server_pump(void);
void server_tick(void);
/* Called by world_set_state after every edit so fluids and other neighbours can react. */
void server_block_changed(int x, int y, int z, u16 old_state, u16 new_state);
void server_schedule(int x, int y, int z, int delay_ticks);
int server_scheduled_count(void);
long server_ticks_run(void);
void server_set_focus(V3 pos); /* random ticks run around this point */

/* ---------------------------------------------------------------- hud.c */

bool hud_init(void);
void hud_shutdown(void);
/* Builds the item icons from the block textures. Needs the texture array and a GL context. */
void hud_build_icons(void);
void hud_draw(int width, int height);
bool hud_inventory_open(void);
void hud_set_inventory_open(bool open);
void hud_update(void);

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
    bool world_set;          /* --world was given: skip the title screen */
    int render_distance;
    char preset[16];         /* --preset: empty keeps the saved choice */
    char screenshot_path[256];
    int screenshot_frame;
    int overlay_page;
    char bench_label[64];     /* --bench-label: free text copied into the JSON so runs can be told apart */
    char bench_json[256];     /* --bench-json: summary of the run */
    char bench_csv[256];      /* --bench-csv: one row per frame */
    char bench_matrix[512];   /* --bench-matrix: cases run in one launch, see bench.c */
    int bench_runs;           /* --bench-runs: repetitions of the matrix, default 1 */
    int workers;             /* 0 = automatic */
    bool wireframe;
    bool allow_native;       /* load native plugins from mods */
    float render_scale;      /* --render-scale, valid when render_scale_set */
    bool render_scale_set;
    bool dynamic_res;        /* --dynamic-res */
    bool dev;                /* --dev: reload shaders and data files as they change */
    bool camera_set;         /* --camera pins the start pose and freezes the benchmark path, for screenshots */
    float camera[5];         /* x y z yaw pitch (degrees) */
    float start_phase;       /* --time: day phase 0..1 forced at start, valid when start_phase_set */
    bool start_phase_set;
    int start_weather;       /* --weather: Weather value forced at start, valid when start_weather_set */
    bool start_weather_set;
} Options;
extern Options g_opt;
