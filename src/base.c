/* Utilities shared by every other unit: logging, memory, math, containers,
 * threads, the priority job system and the layered virtual filesystem. */
#include "dfe.h"

#include <time.h>
#include <errno.h>
#include <sys/stat.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <direct.h>
#else
#include <pthread.h>
#include <unistd.h>
#include <dirent.h>
#endif

/* ------------------------------------------------------------------- log */

#define LOG_HISTORY_LINES 256
#define LOG_LINE_LEN 256

static LogLevel g_log_level = LOG_INFO;
static struct {
    char text[LOG_HISTORY_LINES][LOG_LINE_LEN];
    LogLevel level[LOG_HISTORY_LINES];
    int head, count;
} g_log;
static Mutex *g_log_mutex;

void log_set_level(LogLevel level) { g_log_level = level; }

void log_msg(LogLevel level, const char *fmt, ...) {
    static const char *tags[] = {"debug", "info", "warn", "error"};
    char line[LOG_LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (!g_log_mutex) g_log_mutex = mutex_create();
    mutex_lock(g_log_mutex);
    int slot = (g_log.head + g_log.count) % LOG_HISTORY_LINES;
    if (g_log.count == LOG_HISTORY_LINES) g_log.head = (g_log.head + 1) % LOG_HISTORY_LINES;
    else g_log.count++;
    memcpy(g_log.text[slot], line, LOG_LINE_LEN);
    g_log.level[slot] = level;
    if (level >= g_log_level) fprintf(level >= LOG_WARN ? stderr : stdout, "[%s] %s\n", tags[level], line);
    mutex_unlock(g_log_mutex);
}

int log_history_count(void) { return g_log.count; }

const char *log_history_line(int index, LogLevel *level) {
    int slot = (g_log.head + index) % LOG_HISTORY_LINES;
    if (level) *level = g_log.level[slot];
    return g_log.text[slot];
}

void die(const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[fatal] %s\n", msg);
    exit(1);
}

/* ---------------------------------------------------------------- memory */

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory allocating %zu bytes", n);
    return p;
}
void *xcalloc(size_t n, size_t sz) {
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) die("out of memory allocating %zu bytes", n * sz);
    return p;
}
void *xrealloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) die("out of memory reallocating %zu bytes", n);
    return p;
}
char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = xmalloc(n);
    memcpy(d, s, n);
    return d;
}
char *xstrfmt(const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *s = xmalloc((size_t)n + 1);
    vsnprintf(s, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return s;
}

#ifdef _WIN32
size_t mem_peak_rss_bytes(void) {
    PROCESS_MEMORY_COUNTERS pmc;
    return GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc) ? pmc.PeakWorkingSetSize : 0;
}
size_t mem_current_rss_bytes(void) {
    PROCESS_MEMORY_COUNTERS pmc;
    return GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc) ? pmc.WorkingSetSize : 0;
}
#else
static size_t read_proc_status_kb(const char *key) {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256];
    size_t kb = 0, keylen = strlen(key);
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, keylen) == 0) { kb = (size_t)strtoull(line + keylen, NULL, 10); break; }
    }
    fclose(f);
    return kb;
}
size_t mem_peak_rss_bytes(void) { return read_proc_status_kb("VmHWM:") * 1024; }
size_t mem_current_rss_bytes(void) { return read_proc_status_kb("VmRSS:") * 1024; }
#endif

/* ------------------------------------------------------------------ time */

double time_now_s(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
#endif
}

int cpu_count(void) {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#endif
}

/* ---------------------------------------------------------------- hashes */

u64 hash64(u64 x) {
    x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27; x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    return x;
}
u32 hash_str(const char *s) {
    u32 h = 2166136261u;
    for (; *s; s++) { h ^= (u8)*s; h *= 16777619u; }
    return h;
}
u64 hash_str64(const char *s) {
    u64 h = 1469598103934665603ull;
    for (; *s; s++) { h ^= (u8)*s; h *= 1099511628211ull; }
    return hash64(h);
}
u64 hash3(i64 seed, i64 x, i64 y, i64 z) {
    u64 h = hash64((u64)seed ^ 0xA0761D6478BD642Full);
    h = hash64(h ^ (u64)x * 0x9E3779B97F4A7C15ull);
    h = hash64(h ^ (u64)y * 0xC2B2AE3D27D4EB4Full);
    h = hash64(h ^ (u64)z * 0x165667B19E3779F9ull);
    return h;
}

/* ---------------------------------------------------------------- strmap */

void strmap_init(StrMap *m) { memset(m, 0, sizeof *m); }

void strmap_free(StrMap *m) {
    for (u32 i = 0; i < m->cap; i++) free(m->keys[i]);
    free(m->keys);
    free(m->vals);
    memset(m, 0, sizeof *m);
}

static u32 strmap_find_slot(const StrMap *m, const char *key) {
    u32 mask = m->cap - 1, i = hash_str(key) & mask;
    while (m->keys[i] && strcmp(m->keys[i], key) != 0) i = (i + 1) & mask;
    return i;
}

bool strmap_get(const StrMap *m, const char *key, u32 *out) {
    if (!m->cap) return false;
    u32 i = strmap_find_slot(m, key);
    if (!m->keys[i]) return false;
    if (out) *out = m->vals[i];
    return true;
}

static void strmap_grow(StrMap *m) {
    StrMap n;
    n.cap = m->cap ? m->cap * 2 : 64;
    n.count = 0;
    n.keys = xcalloc(n.cap, sizeof(char *));
    n.vals = xcalloc(n.cap, sizeof(u32));
    for (u32 i = 0; i < m->cap; i++) {
        if (!m->keys[i]) continue;
        u32 s = strmap_find_slot(&n, m->keys[i]);
        n.keys[s] = m->keys[i];
        n.vals[s] = m->vals[i];
        n.count++;
    }
    free(m->keys);
    free(m->vals);
    *m = n;
}

void strmap_set(StrMap *m, const char *key, u32 val) {
    if (!m->cap || (m->count + 1) * 10 > m->cap * 7) strmap_grow(m);
    u32 i = strmap_find_slot(m, key);
    if (!m->keys[i]) { m->keys[i] = xstrdup(key); m->count++; }
    m->vals[i] = val;
}

/* ------------------------------------------------------------------ math */

M4 m4_identity(void) {
    M4 r;
    memset(&r, 0, sizeof r);
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

M4 m4_mul(M4 a, M4 b) {
    M4 r;
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

M4 m4_perspective(float fov_y_rad, float aspect, float znear, float zfar) {
    float f = 1.0f / tanf(fov_y_rad * 0.5f);
    M4 r;
    memset(&r, 0, sizeof r);
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = (zfar + znear) / (znear - zfar);
    r.m[11] = -1.0f;
    r.m[14] = 2.0f * zfar * znear / (znear - zfar);
    return r;
}

M4 m4_ortho(float l, float r_, float b, float t, float n, float f) {
    M4 r = m4_identity();
    r.m[0] = 2.0f / (r_ - l);
    r.m[5] = 2.0f / (t - b);
    r.m[10] = -2.0f / (f - n);
    r.m[12] = -(r_ + l) / (r_ - l);
    r.m[13] = -(t + b) / (t - b);
    r.m[14] = -(f + n) / (f - n);
    return r;
}

M4 m4_look_dir(V3 eye, V3 dir, V3 up) {
    V3 f = v3_norm(dir);
    V3 s = v3_norm(v3_cross(f, up));
    V3 u = v3_cross(s, f);
    M4 r = m4_identity();
    r.m[0] = s.x; r.m[4] = s.y; r.m[8] = s.z;
    r.m[1] = u.x; r.m[5] = u.y; r.m[9] = u.z;
    r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
    r.m[12] = -v3_dot(s, eye);
    r.m[13] = -v3_dot(u, eye);
    r.m[14] = v3_dot(f, eye);
    return r;
}

M4 m4_translate(V3 t) {
    M4 r = m4_identity();
    r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
    return r;
}

M4 m4_inverse(M4 a) {
    const float *m = a.m;
    float inv[16], det;
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    M4 r = m4_identity();
    if (fabsf(det) < 1e-12f) return r;
    det = 1.0f / det;
    for (int i = 0; i < 16; i++) r.m[i] = inv[i] * det;
    return r;
}

V3 m4_transform_point(M4 m, V3 p) {
    float x = m.m[0] * p.x + m.m[4] * p.y + m.m[8] * p.z + m.m[12];
    float y = m.m[1] * p.x + m.m[5] * p.y + m.m[9] * p.z + m.m[13];
    float z = m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z + m.m[14];
    float w = m.m[3] * p.x + m.m[7] * p.y + m.m[11] * p.z + m.m[15];
    if (fabsf(w) > 1e-9f) { x /= w; y /= w; z /= w; }
    return v3(x, y, z);
}

void frustum_from_matrix(Frustum *f, M4 vp) {
    const float *m = vp.m;
    /* Gribb and Hartmann extraction from a column-major clip matrix. */
    for (int i = 0; i < 3; i++) {
        for (int s = 0; s < 2; s++) {
            float sign = s == 0 ? 1.0f : -1.0f;
            float *p = f->planes[i * 2 + s];
            p[0] = m[3] + sign * m[i];
            p[1] = m[7] + sign * m[4 + i];
            p[2] = m[11] + sign * m[8 + i];
            p[3] = m[15] + sign * m[12 + i];
            float len = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
            if (len > 1e-9f) { p[0] /= len; p[1] /= len; p[2] /= len; p[3] /= len; }
        }
    }
}

bool frustum_box_visible(const Frustum *f, V3 lo, V3 hi) {
    for (int i = 0; i < 6; i++) {
        const float *p = f->planes[i];
        /* Positive vertex test: the corner furthest along the plane normal. */
        float x = p[0] >= 0 ? hi.x : lo.x;
        float y = p[1] >= 0 ? hi.y : lo.y;
        float z = p[2] >= 0 ? hi.z : lo.z;
        if (p[0] * x + p[1] * y + p[2] * z + p[3] < 0) return false;
    }
    return true;
}

static int double_cmp(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

double percentile_of(const double *vals, int n, double pct) {
    if (n <= 0) return 0;
    double *tmp = xmalloc((size_t)n * sizeof(double));
    memcpy(tmp, vals, (size_t)n * sizeof(double));
    qsort(tmp, (size_t)n, sizeof(double), double_cmp);
    double idx = pct / 100.0 * (n - 1);
    int lo = (int)idx, hi = MIN(lo + 1, n - 1);
    double r = tmp[lo] + (tmp[hi] - tmp[lo]) * (idx - lo);
    free(tmp);
    return r;
}

/* --------------------------------------------------------------- threads */

#ifdef _WIN32
struct Mutex { CRITICAL_SECTION cs; };
struct Cond { CONDITION_VARIABLE cv; };
struct Thread { HANDLE h; void (*fn)(void *); void *arg; };
Mutex *mutex_create(void) { Mutex *m = xmalloc(sizeof *m); InitializeCriticalSection(&m->cs); return m; }
void mutex_destroy(Mutex *m) { DeleteCriticalSection(&m->cs); free(m); }
void mutex_lock(Mutex *m) { EnterCriticalSection(&m->cs); }
void mutex_unlock(Mutex *m) { LeaveCriticalSection(&m->cs); }
Cond *cond_create(void) { Cond *c = xmalloc(sizeof *c); InitializeConditionVariable(&c->cv); return c; }
void cond_destroy(Cond *c) { free(c); }
void cond_wait(Cond *c, Mutex *m) { SleepConditionVariableCS(&c->cv, &m->cs, INFINITE); }
void cond_signal(Cond *c) { WakeConditionVariable(&c->cv); }
void cond_broadcast(Cond *c) { WakeAllConditionVariable(&c->cv); }
static DWORD WINAPI thread_tramp(LPVOID p) { Thread *t = p; t->fn(t->arg); return 0; }
Thread *thread_start(void (*fn)(void *), void *arg) {
    Thread *t = xmalloc(sizeof *t);
    t->fn = fn; t->arg = arg;
    t->h = CreateThread(NULL, 0, thread_tramp, t, 0, NULL);
    return t;
}
void thread_join(Thread *t) { WaitForSingleObject(t->h, INFINITE); CloseHandle(t->h); free(t); }
#else
struct Mutex { pthread_mutex_t m; };
struct Cond { pthread_cond_t c; };
struct Thread { pthread_t t; void (*fn)(void *); void *arg; };
Mutex *mutex_create(void) { Mutex *m = xmalloc(sizeof *m); pthread_mutex_init(&m->m, NULL); return m; }
void mutex_destroy(Mutex *m) { pthread_mutex_destroy(&m->m); free(m); }
void mutex_lock(Mutex *m) { pthread_mutex_lock(&m->m); }
void mutex_unlock(Mutex *m) { pthread_mutex_unlock(&m->m); }
Cond *cond_create(void) { Cond *c = xmalloc(sizeof *c); pthread_cond_init(&c->c, NULL); return c; }
void cond_destroy(Cond *c) { pthread_cond_destroy(&c->c); free(c); }
void cond_wait(Cond *c, Mutex *m) { pthread_cond_wait(&c->c, &m->m); }
void cond_signal(Cond *c) { pthread_cond_signal(&c->c); }
void cond_broadcast(Cond *c) { pthread_cond_broadcast(&c->c); }
static void *thread_tramp(void *p) { Thread *t = p; t->fn(t->arg); return NULL; }
Thread *thread_start(void (*fn)(void *), void *arg) {
    Thread *t = xmalloc(sizeof *t);
    t->fn = fn; t->arg = arg;
    if (pthread_create(&t->t, NULL, thread_tramp, t) != 0) die("could not create worker thread");
    return t;
}
void thread_join(Thread *t) { pthread_join(t->t, NULL); free(t); }
#endif

/* ------------------------------------------------------------------ jobs */

typedef struct Job {
    int kind;
    float prio;
    u64 seq;
    JobRun run;
    JobComplete complete;
    void *data;
} Job;

static struct {
    Mutex *mu;
    Cond *work_cv, *idle_cv;
    Job *heap;
    int heap_n, heap_cap;
    Job *done;
    int done_head, done_n, done_cap;
    Thread **threads;
    int worker_count;
    int running;
    bool quit;
    u64 seq;
    JobKindStats stats[JOB_KIND_COUNT];
} g_jobs;

static bool job_less(const Job *a, const Job *b) {
    return a->prio < b->prio || (a->prio == b->prio && a->seq < b->seq);
}

static void heap_push(Job j) {
    if (g_jobs.heap_n == g_jobs.heap_cap) {
        g_jobs.heap_cap = g_jobs.heap_cap ? g_jobs.heap_cap * 2 : 256;
        g_jobs.heap = xrealloc(g_jobs.heap, (size_t)g_jobs.heap_cap * sizeof(Job));
    }
    int i = g_jobs.heap_n++;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (!job_less(&j, &g_jobs.heap[p])) break;
        g_jobs.heap[i] = g_jobs.heap[p];
        i = p;
    }
    g_jobs.heap[i] = j;
}

static Job heap_pop(void) {
    Job top = g_jobs.heap[0];
    Job last = g_jobs.heap[--g_jobs.heap_n];
    int i = 0, n = g_jobs.heap_n;
    while (true) {
        int c = i * 2 + 1;
        if (c >= n) break;
        if (c + 1 < n && job_less(&g_jobs.heap[c + 1], &g_jobs.heap[c])) c++;
        if (!job_less(&g_jobs.heap[c], &last)) break;
        g_jobs.heap[i] = g_jobs.heap[c];
        i = c;
    }
    if (n > 0) g_jobs.heap[i] = last;
    return top;
}

static void done_push(Job j) {
    if (g_jobs.done_head + g_jobs.done_n == g_jobs.done_cap) {
        if (g_jobs.done_head > 0) {
            memmove(g_jobs.done, g_jobs.done + g_jobs.done_head, (size_t)g_jobs.done_n * sizeof(Job));
            g_jobs.done_head = 0;
        } else {
            g_jobs.done_cap = g_jobs.done_cap ? g_jobs.done_cap * 2 : 256;
            g_jobs.done = xrealloc(g_jobs.done, (size_t)g_jobs.done_cap * sizeof(Job));
        }
    }
    g_jobs.done[g_jobs.done_head + g_jobs.done_n++] = j;
}

static void worker_main(void *arg) {
    int worker = (int)(intptr_t)arg;
    mutex_lock(g_jobs.mu);
    while (true) {
        while (g_jobs.heap_n == 0 && !g_jobs.quit) cond_wait(g_jobs.work_cv, g_jobs.mu);
        if (g_jobs.quit && g_jobs.heap_n == 0) break;
        Job j = heap_pop();
        g_jobs.running++;
        mutex_unlock(g_jobs.mu);
        double t0 = time_now_s();
        j.run(j.data, worker);
        double dt = time_now_s() - t0;
        mutex_lock(g_jobs.mu);
        JobKindStats *s = &g_jobs.stats[j.kind];
        s->count++;
        s->total_s += dt;
        if (dt > s->max_s) s->max_s = dt;
        g_jobs.running--;
        if (j.complete) done_push(j);
        if (g_jobs.heap_n == 0 && g_jobs.running == 0) cond_broadcast(g_jobs.idle_cv);
    }
    mutex_unlock(g_jobs.mu);
}

void jobs_init(int worker_count) {
    memset(&g_jobs, 0, sizeof g_jobs);
    g_jobs.mu = mutex_create();
    g_jobs.work_cv = cond_create();
    g_jobs.idle_cv = cond_create();
    g_jobs.worker_count = MAX(worker_count, 1);
    g_jobs.threads = xcalloc((size_t)g_jobs.worker_count, sizeof(Thread *));
    for (int i = 0; i < g_jobs.worker_count; i++) g_jobs.threads[i] = thread_start(worker_main, (void *)(intptr_t)i);
}

void jobs_shutdown(void) {
    if (!g_jobs.mu) return;
    mutex_lock(g_jobs.mu);
    g_jobs.quit = true;
    cond_broadcast(g_jobs.work_cv);
    mutex_unlock(g_jobs.mu);
    for (int i = 0; i < g_jobs.worker_count; i++) thread_join(g_jobs.threads[i]);
    free(g_jobs.threads);
    free(g_jobs.heap);
    free(g_jobs.done);
    mutex_destroy(g_jobs.mu);
    cond_destroy(g_jobs.work_cv);
    cond_destroy(g_jobs.idle_cv);
    memset(&g_jobs, 0, sizeof g_jobs);
}

int jobs_worker_count(void) { return g_jobs.worker_count; }

void jobs_submit(int kind, float priority, JobRun run, JobComplete complete, void *data) {
    Job j = {kind, priority, 0, run, complete, data};
    mutex_lock(g_jobs.mu);
    j.seq = g_jobs.seq++;
    heap_push(j);
    cond_signal(g_jobs.work_cv);
    mutex_unlock(g_jobs.mu);
}

int jobs_pump(double budget_s) {
    double start = time_now_s();
    int ran = 0;
    while (true) {
        Job j;
        mutex_lock(g_jobs.mu);
        if (g_jobs.done_n == 0) { mutex_unlock(g_jobs.mu); break; }
        j = g_jobs.done[g_jobs.done_head++];
        g_jobs.done_n--;
        mutex_unlock(g_jobs.mu);
        j.complete(j.data);
        ran++;
        if (time_now_s() - start >= budget_s) break;
    }
    return ran;
}

int jobs_queued(void) {
    mutex_lock(g_jobs.mu);
    int n = g_jobs.heap_n;
    mutex_unlock(g_jobs.mu);
    return n;
}

int jobs_in_flight(void) {
    mutex_lock(g_jobs.mu);
    int n = g_jobs.heap_n + g_jobs.running + g_jobs.done_n;
    mutex_unlock(g_jobs.mu);
    return n;
}

void jobs_wait_idle(void) {
    mutex_lock(g_jobs.mu);
    while (g_jobs.heap_n > 0 || g_jobs.running > 0) cond_wait(g_jobs.idle_cv, g_jobs.mu);
    mutex_unlock(g_jobs.mu);
}

JobKindStats jobs_stats(int kind) {
    mutex_lock(g_jobs.mu);
    JobKindStats s = g_jobs.stats[kind];
    mutex_unlock(g_jobs.mu);
    return s;
}

void jobs_stats_reset(void) {
    mutex_lock(g_jobs.mu);
    memset(g_jobs.stats, 0, sizeof g_jobs.stats);
    mutex_unlock(g_jobs.mu);
}

/* ------------------------------------------------------------ filesystem */

u8 *file_read(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || path_is_dir(path)) { fclose(f); return NULL; }
    u8 *buf = xmalloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0; /* text consumers rely on the terminator */
    if (size) *size = got;
    return buf;
}

bool dir_make_all(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/' && *p != '\\') continue;
        char c = *p;
        *p = 0;
#ifdef _WIN32
        _mkdir(tmp);
#else
        mkdir(tmp, 0755);
#endif
        *p = c;
    }
#ifdef _WIN32
    return _mkdir(tmp) == 0 || errno == EEXIST;
#else
    return mkdir(tmp, 0755) == 0 || errno == EEXIST;
#endif
}

bool file_write_atomic(const char *path, const void *data, size_t size) {
    /* Write then rename so a crash never leaves a truncated save file. */
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, size, f) == size;
    ok = (fclose(f) == 0) && ok;
    if (!ok) { remove(tmp); return false; }
#ifdef _WIN32
    remove(path);
#endif
    return rename(tmp, path) == 0;
}

bool path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

bool path_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IFDIR);
}

/* Symlinks are never followed, so deleting a world cannot reach outside its own folder. */
static bool is_real_dir(const char *path) {
#ifdef _WIN32
    return path_is_dir(path);
#else
    struct stat st;
    return lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool dir_remove_all(const char *path) {
    if (!is_real_dir(path)) return remove(path) == 0;
    StrList l = {0};
    dir_list(path, &l);
    bool ok = true;
    for (int i = 0; i < l.n; i++) {
        char child[1100];
        snprintf(child, sizeof child, "%s/%s", path, l.d[i]);
        if (!dir_remove_all(child)) ok = false;
    }
    strlist_free(&l);
#ifdef _WIN32
    return ok && _rmdir(path) == 0;
#else
    return ok && rmdir(path) == 0;
#endif
}

bool path_rename(const char *from, const char *to) { return rename(from, to) == 0; }

i64 path_mtime(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (i64)st.st_mtime : -1;
}

void strlist_free(StrList *l) {
    for (int i = 0; i < l->n; i++) free(l->d[i]);
    vec_free(*l);
}

static int strptr_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

void dir_list(const char *path, StrList *out) {
#ifdef _WIN32
    char pat[1100];
    snprintf(pat, sizeof pat, "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) vec_push(*out, xstrdup(fd.cFileName));
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) vec_push(*out, xstrdup(e->d_name));
    }
    closedir(d);
#endif
    if (out->n > 1) qsort(out->d, (size_t)out->n, sizeof(char *), strptr_cmp);
}

void path_exe_dir(char *out, size_t cap) {
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
    while (n > 0 && out[n - 1] != '\\' && out[n - 1] != '/') n--;
    out[n > 0 ? n - 1 : 0] = 0;
#else
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n <= 0) { snprintf(out, cap, "."); return; }
    out[n] = 0;
    char *slash = strrchr(out, '/');
    if (slash) *slash = 0;
#endif
}

/* ------------------------------------------------------------------- vfs */

typedef struct VfsRoot { char *dir; char *mod; } VfsRoot;
typedef struct WatchEntry { char *path; i64 mtime; } WatchEntry;
static VEC(VfsRoot) g_roots;
static VEC(WatchEntry) g_watch;
static Mutex *g_vfs_mutex;

static void vfs_lock(void) { if (!g_vfs_mutex) g_vfs_mutex = mutex_create(); mutex_lock(g_vfs_mutex); }

static i64 file_mtime(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (i64)st.st_mtime * 1000 + (i64)(st.st_size & 1023) : -1;
}

void vfs_reset(void) {
    vfs_lock();
    for (int i = 0; i < g_roots.n; i++) { free(g_roots.d[i].dir); free(g_roots.d[i].mod); }
    vec_clear(g_roots);
    for (int i = 0; i < g_watch.n; i++) free(g_watch.d[i].path);
    vec_clear(g_watch);
    mutex_unlock(g_vfs_mutex);
}

void vfs_add_root(const char *dir, const char *mod_id) {
    vfs_lock();
    VfsRoot r = {xstrdup(dir), xstrdup(mod_id)};
    vec_push(g_roots, r);
    mutex_unlock(g_vfs_mutex);
}

int vfs_root_count(void) { return g_roots.n; }
const char *vfs_root_dir(int i) { return g_roots.d[i].dir; }
const char *vfs_root_mod(int i) { return g_roots.d[i].mod; }

bool vfs_resolve(const char *rel, char *out, size_t cap, const char **owner_out) {
    vfs_lock();
    for (int i = g_roots.n - 1; i >= 0; i--) {
        snprintf(out, cap, "%s/%s", g_roots.d[i].dir, rel);
        if (path_exists(out)) {
            if (owner_out) *owner_out = g_roots.d[i].mod;
            mutex_unlock(g_vfs_mutex);
            return true;
        }
    }
    mutex_unlock(g_vfs_mutex);
    return false;
}

u8 *vfs_read(const char *rel, size_t *size, const char **owner_out) {
    char path[1100];
    if (!vfs_resolve(rel, path, sizeof path, owner_out)) return NULL;
    u8 *data = file_read(path, size);
    if (!data) return NULL;
    vfs_lock();
    bool known = false;
    for (int i = 0; i < g_watch.n; i++) if (strcmp(g_watch.d[i].path, path) == 0) { known = true; break; }
    if (!known) {
        WatchEntry w = {xstrdup(path), file_mtime(path)};
        vec_push(g_watch, w);
    }
    mutex_unlock(g_vfs_mutex);
    return data;
}

bool vfs_exists(const char *rel) {
    char path[1100];
    return vfs_resolve(rel, path, sizeof path, NULL);
}

void vfs_list(const char *rel_dir, StrList *out) {
    vfs_lock();
    for (int i = 0; i < g_roots.n; i++) {
        char path[1100];
        snprintf(path, sizeof path, "%s/%s", g_roots.d[i].dir, rel_dir);
        StrList l = {0};
        dir_list(path, &l);
        for (int k = 0; k < l.n; k++) {
            bool dup = false;
            for (int j = 0; j < out->n; j++) if (strcmp(out->d[j], l.d[k]) == 0) { dup = true; break; }
            if (!dup) vec_push(*out, l.d[k]);
            else free(l.d[k]);
        }
        vec_free(l);
    }
    mutex_unlock(g_vfs_mutex);
    if (out->n > 1) qsort(out->d, (size_t)out->n, sizeof(char *), strptr_cmp);
}

u64 vfs_stamp(void) {
    vfs_lock();
    u64 stamp = 0;
    for (int i = 0; i < g_watch.n; i++) stamp = stamp * 31 + (u64)file_mtime(g_watch.d[i].path);
    mutex_unlock(g_vfs_mutex);
    return stamp;
}
