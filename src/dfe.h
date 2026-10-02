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
} Options;
extern Options g_opt;
