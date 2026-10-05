/* Region-scale drainage. The world is cut into square regions; each region is solved once, from a coarse elevation
 * field that reaches one region past every side, and the result is rasterised so a column query is a few array reads.
 *
 *   1. Elevation: terrain height on a HY_CELL grid (cached in tiles, shared by neighbouring regions).
 *   2. Priority-flood (Barnes): fills every depression to its spill height and gives each cell a flow target, so
 *      water always reaches the ocean or the edge of the window and never stops in a pit.
 *   3. Flow accumulation: upstream catchment per cell, summed from the leaves of that drainage tree down.
 *   4. Rivers: cells above the catchment threshold. Width grows with catchment, so it grows strictly downstream.
 *   5. Lakes: filled depressions of bounded area and depth, each one a single flat water plane.
 *   6. Water surface: derived from the filled height, smoothed along the flow path, forced monotone downstream and
 *      clamped to sea level at the mouth.
 *   7. Centrelines: cell positions are relaxed and joined with Catmull-Rom splines, then stamped into a raster.
 *
 * Everything a region holds depends only on (seed, parameters, region coordinates). Where two regions meet, both
 * rasters overlap by HY_MARGIN blocks and are blended with a smoothstep, so the field is continuous across a seam
 * even where the two windows saw slightly different catchments. */
#include "gen_internal.h"

#include <limits.h>

#define HY_CORE_CELLS 128
#define HY_CORE (HY_CELL * HY_CORE_CELLS)   /* 512 blocks */
#define HY_WIN (HY_CORE_CELLS * 3)          /* the window: this region plus one region on every side */
#define HY_PIX 2                            /* raster lattice spacing, blocks */
#define HY_MARGIN 40
#define HY_RAST ((HY_CORE + 2 * HY_MARGIN) / HY_PIX + 1)
#define HY_SEAM 32                          /* half width of the blend strip at a region boundary */
#define HY_TILE 64
#define HY_TILE_SLOTS 256                   /* 16 x 16 tiles: any window of up to 16 tiles per side is collision free */
#define HY_REGION_SLOTS 12
#define HY_SUBSTEPS 4
#define HY_LAKE_DIST_CELLS 16
#define HY_LAKE_MIN_CELLS 5                 /* smallest pond, in cells (16 square blocks each) */
#define HY_GAP_PX 4                         /* rivers closer than 2 * this many raster pixels are merged */

typedef struct HPix {
    float edge, level, half_width, flow;
    float lake_level;
    i8 dir_x, dir_z;
    u8 grade, flags, lakeness, lake_dist;
} HPix;

typedef struct ElevTile {
    int tx, tz;
    bool used;
    float e[HY_TILE * HY_TILE];
    u8 ocean[HY_TILE * HY_TILE];
} ElevTile;

typedef struct HRegion {
    bool used;
    int rx, rz;
    u64 stamp;
    HPix *pix;
    HydroCellInfo *cells;
    int cell_count;
} HRegion;

typedef struct HeapItem { float key; u32 seq; int idx; } HeapItem;

enum { F_OCEAN = 1, F_SEEN = 2, F_LAKE = 4, F_RIVER = 8 };

static struct {
    bool ready;
    int sea;
    HydroParams p;
    u64 seed;
    Mutex *lock;
    u64 clock;
    int built;
    ElevTile *tiles;
    HRegion regions[HY_REGION_SLOTS];
    /* Window scratch, allocated once and reused under the lock. */
    float *E, *F, *G, *px, *pz, *taper;
    int *down, *order, *mainup, *queue, *pos;
    u32 *acc;
    u8 *flag;
    u8 *lake_d;
    HeapItem *heap;
    int heap_n;
} H;

static float sm01(float t) { t = CLAMP(t, 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }
static int floordiv(int a, int b) { int q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }

void hydro_params_default(HydroParams *p) {
    *p = (HydroParams){
        .river_min_area = 520.0f, .width_base = 3.0f, .width_scale = 0.35f, .width_exp = 0.4f,
        .depth_base = 1.6f, .depth_scale = 0.55f, .bank_grad_min = 0.40f, .bank_grad_max = 1.1f,
        .valley_reach = 24.0f, .lake_min_depth = 1.0f, .lake_max_depth = 22.0f, .lake_max_cells = 9000,
        .fall_drop = 2.5f, .rapids_drop = 1.4f,
    };
}

static int clamp_field(float *v, float lo, float hi, const char *name, char *first, size_t cap, int changed) {
    if (isfinite(*v) && *v >= lo && *v <= hi) return changed;
    if (!changed && first && cap) snprintf(first, cap, "%s", name);
    *v = isfinite(*v) ? CLAMP(*v, lo, hi) : lo;
    return changed + 1;
}

int hydro_params_sanitize(HydroParams *p, char *first, size_t first_cap) {
    int n = 0;
    if (first && first_cap) first[0] = 0;
#define FIELD(f, lo, hi) n = clamp_field(&p->f, lo, hi, #f, first, first_cap, n)
    FIELD(river_min_area, 60.0f, 20000.0f);
    FIELD(width_base, 1.0f, 12.0f);
    FIELD(width_scale, 0.0f, 3.0f);
    FIELD(width_exp, 0.2f, 0.9f);
    FIELD(depth_base, 0.8f, 6.0f);
    FIELD(depth_scale, 0.0f, 2.0f);
    FIELD(bank_grad_min, 0.15f, 2.0f);
    FIELD(bank_grad_max, 0.15f, 3.0f);
    FIELD(valley_reach, 6.0f, 36.0f);
    FIELD(lake_min_depth, 1.0f, 8.0f);
    FIELD(lake_max_depth, 2.0f, 40.0f);
    FIELD(fall_drop, 1.0f, 16.0f);
    FIELD(rapids_drop, 0.3f, 8.0f);
#undef FIELD
    float cells = (float)p->lake_max_cells;
    n = clamp_field(&cells, 16.0f, 12000.0f, "lake_max_cells", first, first_cap, n);
    p->lake_max_cells = (int)cells;
    if (p->bank_grad_max < p->bank_grad_min) { p->bank_grad_max = p->bank_grad_min; if (!n && first) snprintf(first, first_cap, "bank_grad_max"); n++; }
    if (p->rapids_drop > p->fall_drop) { p->rapids_drop = p->fall_drop; if (!n && first) snprintf(first, first_cap, "rapids_drop"); n++; }
    return n;
}

/* ------------------------------------------------------------ elevation */

static ElevTile *tile_get(int tx, int tz) {
    ElevTile *t = &H.tiles[(tx & 15) | ((tz & 15) << 4)];
    if (t->used && t->tx == tx && t->tz == tz) return t;
    t->used = true; t->tx = tx; t->tz = tz;
    for (int z = 0; z < HY_TILE; z++)
        for (int x = 0; x < HY_TILE; x++) {
            float wx = (float)((tx * HY_TILE + x) * HY_CELL + HY_CELL / 2), wz = (float)((tz * HY_TILE + z) * HY_CELL + HY_CELL / 2);
            float e = gen_terrain_height_raw(wx, wz);
            t->e[z * HY_TILE + x] = e;
            t->ocean[z * HY_TILE + x] = (u8)(e < (float)H.sea && gen_ocean_cell(wx, wz));
        }
    return t;
}

/* ---------------------------------------------------------------- heap */

static bool heap_less(const HeapItem *a, const HeapItem *b) { return a->key < b->key || (a->key == b->key && a->seq < b->seq); }

static void heap_push(HeapItem it) {
    int i = H.heap_n++;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (!heap_less(&it, &H.heap[p])) break;
        H.heap[i] = H.heap[p];
        i = p;
    }
    H.heap[i] = it;
}

static HeapItem heap_pop(void) {
    HeapItem top = H.heap[0], last = H.heap[--H.heap_n];
    int i = 0;
    for (;;) {
        int c = i * 2 + 1;
        if (c >= H.heap_n) break;
        if (c + 1 < H.heap_n && heap_less(&H.heap[c + 1], &H.heap[c])) c++;
        if (!heap_less(&H.heap[c], &last)) break;
        H.heap[i] = H.heap[c];
        i = c;
    }
    if (H.heap_n > 0) H.heap[i] = last;
    return top;
}

/* ------------------------------------------------------------- solving */

static const int DX8[8] = {1, 1, 0, -1, -1, -1, 0, 1};
static const int DZ8[8] = {0, 1, 1, 1, 0, -1, -1, -1};

static void alloc_scratch(void) {
    if (H.E) return;
    size_t n = (size_t)HY_WIN * HY_WIN;
    H.E = xmalloc(n * sizeof(float)); H.F = xmalloc(n * sizeof(float)); H.G = xmalloc(n * sizeof(float));
    H.px = xmalloc(n * sizeof(float)); H.pz = xmalloc(n * sizeof(float));
    H.down = xmalloc(n * sizeof(int)); H.order = xmalloc(n * sizeof(int));
    H.mainup = xmalloc(n * sizeof(int)); H.queue = xmalloc(n * sizeof(int));
    H.pos = xmalloc(n * sizeof(int)); H.taper = xmalloc(n * sizeof(float));
    H.acc = xmalloc(n * sizeof(u32));
    H.flag = xmalloc(n); H.lake_d = xmalloc(n);
    H.heap = xmalloc(n * sizeof(HeapItem));
}

static void fill_window(int cx0, int cz0) {
    for (int z = 0; z < HY_WIN; z++)
        for (int x = 0; x < HY_WIN; x++) {
            int gx = cx0 + x, gz = cz0 + z;
            int tx = floordiv(gx, HY_TILE), tz = floordiv(gz, HY_TILE);
            const ElevTile *t = tile_get(tx, tz);
            int li = (gz - tz * HY_TILE) * HY_TILE + (gx - tx * HY_TILE);
            int i = z * HY_WIN + x;
            H.E[i] = t->e[li];
            H.flag[i] = t->ocean[li] ? F_OCEAN : 0;
        }
}

static void priority_flood(void) {
    int n = HY_WIN * HY_WIN;
    u32 seq = 0;
    H.heap_n = 0;
    for (int z = 0; z < HY_WIN; z++)
        for (int x = 0; x < HY_WIN; x++) {
            int i = z * HY_WIN + x;
            H.down[i] = -1;
            bool border = x == 0 || z == 0 || x == HY_WIN - 1 || z == HY_WIN - 1;
            if ((H.flag[i] & F_OCEAN) || border) {
                H.F[i] = H.E[i];
                H.flag[i] |= F_SEEN;
                heap_push((HeapItem){H.E[i], seq++, i});
            }
        }
    int placed = 0;
    while (H.heap_n > 0) {
        HeapItem it = heap_pop();
        int c = it.idx, cx = c % HY_WIN, cz = c / HY_WIN;
        H.order[placed++] = c;
        for (int d = 0; d < 8; d++) {
            int nx = cx + DX8[d], nz = cz + DZ8[d];
            if (nx < 0 || nz < 0 || nx >= HY_WIN || nz >= HY_WIN) continue;
            int ni = nz * HY_WIN + nx;
            if (H.flag[ni] & F_SEEN) continue;
            H.flag[ni] |= F_SEEN;
            H.down[ni] = c;
            H.F[ni] = MAX(H.E[ni], H.F[c]);
            heap_push((HeapItem){H.F[ni], seq++, ni});
        }
    }
    (void)n;
    for (int i = 0; i < HY_WIN * HY_WIN; i++) H.acc[i] = 1;
    for (int k = placed - 1; k >= 0; k--) {
        int c = H.order[k];
        if (H.down[c] >= 0) H.acc[H.down[c]] += H.acc[c];
    }
}

/* Marks lake cells: connected filled depressions that are neither open ocean nor too large or deep. A basin whose
 * flood touches the window border is dropped, because its true spill point lies outside what this region can see. */
static void find_lakes(void) {
    const HydroParams *p = &H.p;
    int n = HY_WIN * HY_WIN;
    for (int i = 0; i < n; i++)
        if (!(H.flag[i] & F_OCEAN) && H.F[i] - H.E[i] >= p->lake_min_depth) H.flag[i] |= F_LAKE;
    static const int D4[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (int s = 0; s < n; s++) {
        if (!(H.flag[s] & F_LAKE) || (H.flag[s] & F_RIVER)) continue; /* F_RIVER doubles as "visited" here */
        int head = 0, tail = 0, area = 0;
        float deepest = 0.0f;
        bool border = false;
        H.queue[tail++] = s;
        H.flag[s] |= F_RIVER;
        while (head < tail) {
            int c = H.queue[head++], cx = c % HY_WIN, cz = c / HY_WIN;
            area++;
            deepest = MAX(deepest, H.F[c] - H.E[c]);
            if (cx < 2 || cz < 2 || cx >= HY_WIN - 2 || cz >= HY_WIN - 2) border = true;
            for (int d = 0; d < 4; d++) {
                int nx = cx + D4[d][0], nz = cz + D4[d][1];
                if (nx < 0 || nz < 0 || nx >= HY_WIN || nz >= HY_WIN) { border = true; continue; }
                int ni = nz * HY_WIN + nx;
                if ((H.flag[ni] & F_LAKE) && !(H.flag[ni] & F_RIVER)) { H.flag[ni] |= F_RIVER; H.queue[tail++] = ni; }
            }
        }
        if (area < HY_LAKE_MIN_CELLS || area > p->lake_max_cells || deepest > p->lake_max_depth || border)
            for (int k = 0; k < tail; k++) H.flag[H.queue[k]] &= (u8)~F_LAKE;
    }
    for (int i = 0; i < n; i++) H.flag[i] &= (u8)~F_RIVER;
    /* Chamfer distance (in cells) to the nearest lake cell, used for shoreline moisture. */
    const u8 CAP = HY_LAKE_DIST_CELLS * 3;
    for (int i = 0; i < n; i++) H.lake_d[i] = (H.flag[i] & F_LAKE) ? 0 : CAP;
    for (int z = 0; z < HY_WIN; z++)
        for (int x = 0; x < HY_WIN; x++) {
            int i = z * HY_WIN + x; int v = H.lake_d[i];
            if (x > 0) v = MIN(v, H.lake_d[i - 1] + 3);
            if (z > 0) { v = MIN(v, H.lake_d[i - HY_WIN] + 3); if (x > 0) v = MIN(v, H.lake_d[i - HY_WIN - 1] + 4); if (x < HY_WIN - 1) v = MIN(v, H.lake_d[i - HY_WIN + 1] + 4); }
            H.lake_d[i] = (u8)MIN(v, CAP);
        }
    for (int z = HY_WIN - 1; z >= 0; z--)
        for (int x = HY_WIN - 1; x >= 0; x--) {
            int i = z * HY_WIN + x; int v = H.lake_d[i];
            if (x < HY_WIN - 1) v = MIN(v, H.lake_d[i + 1] + 3);
            if (z < HY_WIN - 1) { v = MIN(v, H.lake_d[i + HY_WIN] + 3); if (x < HY_WIN - 1) v = MIN(v, H.lake_d[i + HY_WIN + 1] + 4); if (x > 0) v = MIN(v, H.lake_d[i + HY_WIN - 1] + 4); }
            H.lake_d[i] = (u8)MIN(v, CAP);
        }
}

static float river_half_width(int c) {
    const HydroParams *p = &H.p;
    float full = p->width_base + p->width_scale * powf((float)H.acc[c], p->width_exp);
    /* Estuary: the channel flares as it meets the sea. The factor grows monotonically downstream because F falls. */
    float flare = 1.0f + 0.9f * (1.0f - sm01((H.F[c] - (float)H.sea) / 5.0f));
    return 0.5f * full * flare * H.taper[c];
}

static bool is_river(int c) { return (H.flag[c] & F_RIVER) != 0; }

static float cell_hash(int gx, int gz, u32 salt) {
    u64 h = H.seed ^ ((u64)(u32)gx * 0x9E3779B97F4A7C15ull) ^ ((u64)(u32)gz * 0xC2B2AE3D27D4EB4Full) ^ ((u64)salt * 0x165667B19E3779F9ull);
    h ^= h >> 31; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 29; h *= 0x94D049BB133111EBull; h ^= h >> 32;
    return (float)(h & 0xFFFFFF) / (float)0xFFFFFF;
}

static void mark_rivers(void) {
    int n = HY_WIN * HY_WIN;
    for (int i = 0; i < n; i++) {
        bool ok = !(H.flag[i] & (F_OCEAN | F_LAKE)) && (float)H.acc[i] >= H.p.river_min_area;
        if (ok) H.flag[i] |= F_RIVER; else H.flag[i] &= (u8)~F_RIVER;
    }
}

/* Streams that run within MERGE_R cells of a larger one are redirected into it, so no two channels run in parallel.
 * Only targets earlier in flood order are taken, which keeps the drainage tree acyclic. */
#define MERGE_R 2
static void merge_parallel(void) {
    int n = HY_WIN * HY_WIN;
    for (int k = 0; k < n; k++) H.pos[H.order[k]] = k;
    for (int k = n - 1; k >= 0; k--) {
        int c = H.order[k];
        if (!is_river(c) || H.down[c] < 0) continue;
        int cx = c % HY_WIN, cz = c / HY_WIN, best = -1, bd = 1 << 30;
        for (int dz = -MERGE_R; dz <= MERGE_R; dz++)
            for (int dx = -MERGE_R; dx <= MERGE_R; dx++) {
                int nx = cx + dx, nz = cz + dz;
                if ((!dx && !dz) || nx < 0 || nz < 0 || nx >= HY_WIN || nz >= HY_WIN) continue;
                int m = nz * HY_WIN + nx;
                if (!is_river(m) || m == H.down[c] || H.pos[m] >= H.pos[c] || H.F[m] > H.F[c] || H.F[m] < H.F[c] - 0.6f || H.acc[m] < H.acc[c]) continue;
                int d = dx * dx + dz * dz;
                if (d < bd || (d == bd && H.acc[m] > H.acc[best])) { bd = d; best = m; }
            }
        if (best >= 0) H.down[c] = best;
    }
    for (int i = 0; i < n; i++) H.acc[i] = 1;
    for (int k = n - 1; k >= 0; k--) { int c = H.order[k]; if (H.down[c] >= 0) H.acc[H.down[c]] += H.acc[c]; }
}

static void select_rivers(int cx0, int cz0) {
    int n = HY_WIN * HY_WIN;
    for (int i = 0; i < n; i++) H.taper[i] = 1.0f;
    mark_rivers();
    merge_parallel();
    mark_rivers();
    for (int i = 0; i < n; i++) {
        H.mainup[i] = -1;
        int cx = i % HY_WIN, cz = i / HY_WIN;
        /* Jitter breaks the grid's straight and 45-degree runs; the relaxation in shape_rivers turns it into meander. */
        H.px[i] = (float)(cx * HY_CELL + HY_CELL / 2) + (cell_hash(cx0 + cx, cz0 + cz, 1) - 0.5f) * 2.2f * HY_CELL;
        H.pz[i] = (float)(cz * HY_CELL + HY_CELL / 2) + (cell_hash(cx0 + cx, cz0 + cz, 2) - 0.5f) * 2.2f * HY_CELL;
    }
    for (int k = 0; k < n; k++) {
        int c = H.order[k];
        if (!is_river(c)) continue;
        int t = H.down[c];
        if (t >= 0 && is_river(t) && (H.mainup[t] < 0 || H.acc[c] > H.acc[H.mainup[t]])) H.mainup[t] = c;
    }
    /* Headwaters taper in over TAPER_CELLS cells. A head cut by the window edge is not a real head, so it stays full. */
    enum { TAPER_CELLS = 10 };
    int *len = H.queue; /* free scratch once the lakes are found */
    for (int k = 0; k < n; k++) len[k] = -1;
    for (int k = n - 1; k >= 0; k--) {
        int c = H.order[k];
        if (!is_river(c)) continue;
        int cx = c % HY_WIN, cz = c / HY_WIN;
        if (len[c] < 0) len[c] = (cx < 3 || cz < 3 || cx >= HY_WIN - 3 || cz >= HY_WIN - 3) ? 1000 : 0;
        H.taper[c] = 0.12f + 0.88f * sm01((float)len[c] / (float)TAPER_CELLS);
        int t = H.down[c];
        if (t >= 0 && is_river(t)) len[t] = MAX(len[t], len[c] + 1); /* the longest branch sets the width, so it never narrows downstream */
    }
}

/* Water surface and centreline relaxation along the drainage tree. */
static void shape_rivers(void) {
    const HydroParams *p = &H.p;
    int n = HY_WIN * HY_WIN;
    float sea = (float)H.sea;
    for (int i = 0; i < n; i++) H.G[i] = H.F[i] - 0.6f;
    float *tmpG = H.queue ? (float *)xmalloc((size_t)n * sizeof(float)) : NULL;
    float *tmpX = (float *)xmalloc((size_t)n * sizeof(float)), *tmpZ = (float *)xmalloc((size_t)n * sizeof(float));
    for (int pass = 0; pass < 12; pass++) {
        memcpy(tmpG, H.G, (size_t)n * sizeof(float));
        memcpy(tmpX, H.px, (size_t)n * sizeof(float));
        memcpy(tmpZ, H.pz, (size_t)n * sizeof(float));
        for (int c = 0; c < n; c++) {
            if (!is_river(c)) continue;
            int pr = H.mainup[c], nx = H.down[c];
            bool has_next = nx >= 0 && (is_river(nx) || (H.flag[nx] & F_OCEAN));
            float wp = 0.0f, wn = 0.0f;
            if (pr >= 0 && H.G[pr] - H.G[c] < p->fall_drop) wp = 0.3f;
            if (has_next && is_river(nx) && H.G[c] - H.G[nx] < p->fall_drop) wn = 0.3f;
            tmpG[c] = H.G[c] * (1.0f - wp - wn) + (wp > 0 ? H.G[pr] * wp : 0.0f) + (wn > 0 ? H.G[nx] * wn : 0.0f);
            float pw = pr >= 0 ? 0.3f : 0.0f, nw = has_next ? 0.3f : 0.0f;
            tmpX[c] = H.px[c] * (1.0f - pw - nw) + (pr >= 0 ? H.px[pr] * pw : 0.0f) + (has_next ? H.px[nx] * nw : 0.0f);
            tmpZ[c] = H.pz[c] * (1.0f - pw - nw) + (pr >= 0 ? H.pz[pr] * pw : 0.0f) + (has_next ? H.pz[nx] * nw : 0.0f);
        }
        memcpy(H.G, tmpG, (size_t)n * sizeof(float));
        memcpy(H.px, tmpX, (size_t)n * sizeof(float));
        memcpy(H.pz, tmpZ, (size_t)n * sizeof(float));
    }
    free(tmpG); free(tmpX); free(tmpZ);
    for (int c = 0; c < n; c++) if (is_river(c)) H.G[c] = MIN(H.G[c], H.E[c] - 0.5f);
    /* Children are visited before parents (reverse flood order), so the lowering reaches the mouth in one sweep. */
    for (int k = n - 1; k >= 0; k--) {
        int c = H.order[k], t = H.down[c];
        if (is_river(c) && t >= 0 && is_river(t)) H.G[t] = MIN(H.G[t], H.G[c]);
    }
    for (int c = 0; c < n; c++) if (is_river(c)) H.G[c] = MAX(H.G[c], sea);
}

/* ------------------------------------------------------------ raster */

static void catmull(const float *p0, const float *p1, const float *p2, const float *p3, float s, float *out) {
    float s2 = s * s, s3 = s2 * s;
    for (int k = 0; k < 2; k++)
        out[k] = 0.5f * ((2.0f * p1[k]) + (-p0[k] + p2[k]) * s + (2.0f * p0[k] - 5.0f * p1[k] + 4.0f * p2[k] - p3[k]) * s2
                         + (-p0[k] + 3.0f * p1[k] - 3.0f * p2[k] + p3[k]) * s3);
}

typedef struct Stamp {
    float ox, oz;
    float ax, az, bx, bz;
    float hwa, hwb, lva, lvb, flow;
    float grade;
    float dx, dz;
    u8 flags;
} Stamp;

static void stamp_segment(HPix *pix, const Stamp *s) {
    const float reach = H.p.valley_reach;
    float hwm = MAX(s->hwa, s->hwb), r = hwm + reach;
    int x0 = (int)floorf((MIN(s->ax, s->bx) - r - s->ox) / HY_PIX), x1 = (int)ceilf((MAX(s->ax, s->bx) + r - s->ox) / HY_PIX);
    int z0 = (int)floorf((MIN(s->az, s->bz) - r - s->oz) / HY_PIX), z1 = (int)ceilf((MAX(s->az, s->bz) + r - s->oz) / HY_PIX);
    x0 = MAX(x0, 0); z0 = MAX(z0, 0); x1 = MIN(x1, HY_RAST - 1); z1 = MIN(z1, HY_RAST - 1);
    float vx = s->bx - s->ax, vz = s->bz - s->az, len2 = vx * vx + vz * vz;
    for (int iz = z0; iz <= z1; iz++)
        for (int ix = x0; ix <= x1; ix++) {
            float qx = s->ox + (float)(ix * HY_PIX) - s->ax, qz = s->oz + (float)(iz * HY_PIX) - s->az;
            float t = len2 > 1e-6f ? CLAMP((qx * vx + qz * vz) / len2, 0.0f, 1.0f) : 0.0f;
            float ex = qx - vx * t, ez = qz - vz * t;
            float dist = sqrtf(ex * ex + ez * ez);
            float hw = s->hwa + (s->hwb - s->hwa) * t;
            float edge = dist - hw;
            HPix *px = &pix[iz * HY_RAST + ix];
            if (edge >= reach || edge >= px->edge) continue;
            px->edge = edge;
            px->level = s->lva + (s->lvb - s->lva) * t;
            px->half_width = hw;
            px->flow = s->flow;
            px->dir_x = (i8)lrintf(s->dx * 127.0f);
            px->dir_z = (i8)lrintf(s->dz * 127.0f);
            px->grade = (u8)lrintf(CLAMP(s->grade, 0.0f, 1.0f) * 255.0f);
            px->flags = s->flags;
        }
}

/* Water level profile along a segment: linear, except a fall holds its lip and plunges over the last quarter. */
static float lvl_shape(float s, u8 flags) {
    if (!(flags & HYF_FALL)) return s;
    return sm01((s - 0.72f) / 0.2f);
}

static void stamp_rivers(HRegion *R, int cx0, int cz0, float ox, float oz) {
    const HydroParams *p = &H.p;
    int n = HY_WIN * HY_WIN;
    float sea = (float)H.sea, rx0 = ox - (float)(cx0 * HY_CELL), rz0 = oz - (float)(cz0 * HY_CELL); /* raster origin in window blocks */
    float span = (float)(HY_RAST * HY_PIX);
    for (int c = 0; c < n; c++) {
        if (!is_river(c)) continue;
        int t = H.down[c];
        if (t < 0) continue;
        float hw = river_half_width(c);
        float reach = hw + p->valley_reach + 12.0f;
        if (H.px[c] < rx0 - reach || H.px[c] > rx0 + span + reach || H.pz[c] < rz0 - reach || H.pz[c] > rz0 + span + reach) continue;
        bool to_river = is_river(t);
        float p1[2] = {H.px[c], H.pz[c]}, p2[2] = {H.px[t], H.pz[t]}, p0[2], p3[2];
        int up = H.mainup[c];
        if (up >= 0) { p0[0] = H.px[up]; p0[1] = H.pz[up]; } else { p0[0] = 2 * p1[0] - p2[0]; p0[1] = 2 * p1[1] - p2[1]; }
        int tt = to_river ? H.down[t] : -1;
        if (tt >= 0) { p3[0] = H.px[tt]; p3[1] = H.pz[tt]; } else { p3[0] = 2 * p2[0] - p1[0]; p3[1] = 2 * p2[1] - p1[1]; }
        float hwt = to_river ? river_half_width(t) : hw * (H.flag[t] & F_OCEAN ? 1.3f : 1.0f);
        float lvt = to_river ? H.G[t] : (H.flag[t] & F_OCEAN ? sea : H.G[c]);
        float drop = H.G[c] - lvt;
        u8 flags = 0;
        if (drop >= p->fall_drop) flags |= HYF_FALL; else if (drop >= p->rapids_drop) flags |= HYF_RAPIDS;
        if (H.F[c] < sea + 4.0f) flags |= HYF_ESTUARY;
        float smax = 1.0f;
        if (up < 0 && (float)H.acc[c] < p->river_min_area * 1.5f) hw *= 0.45f; /* a source starts as a thread instead of a round blob */
        /* Local ground slope from the elevation field, as a 0..1 grade (about 1.5 blocks per block is the maximum). */
        int cx = c % HY_WIN, cz = c / HY_WIN;
        float gx = 0.0f, gz = 0.0f;
        if (cx > 0 && cx < HY_WIN - 1 && cz > 0 && cz < HY_WIN - 1) {
            gx = (H.E[c + 1] - H.E[c - 1]) / (2.0f * HY_CELL);
            gz = (H.E[c + HY_WIN] - H.E[c - HY_WIN]) / (2.0f * HY_CELL);
        }
        float grade = sqrtf(gx * gx + gz * gz) / 1.5f;
        float prev[2];
        catmull(p0, p1, p2, p3, 0.0f, prev);
        for (int k = 1; k <= HY_SUBSTEPS; k++) {
            float s0 = smax * (float)(k - 1) / HY_SUBSTEPS, s1 = smax * (float)k / HY_SUBSTEPS, cur[2];
            catmull(p0, p1, p2, p3, s1, cur);
            Stamp st = {.ox = ox - 0.0f, .oz = oz,
                .ax = prev[0] + (float)(cx0 * HY_CELL), .az = prev[1] + (float)(cz0 * HY_CELL),
                .bx = cur[0] + (float)(cx0 * HY_CELL), .bz = cur[1] + (float)(cz0 * HY_CELL),
                .hwa = hw + (hwt - hw) * s0, .hwb = hw + (hwt - hw) * s1,
                .lva = H.G[c] + (lvt - H.G[c]) * lvl_shape(s0, flags), .lvb = H.G[c] + (lvt - H.G[c]) * lvl_shape(s1, flags),
                .flow = (float)H.acc[c], .grade = grade, .flags = flags};
            float dx = st.bx - st.ax, dz = st.bz - st.az, l = sqrtf(dx * dx + dz * dz);
            st.dx = l > 1e-4f ? dx / l : 0.0f; st.dz = l > 1e-4f ? dz / l : 0.0f;
            stamp_segment(R->pix, &st);
            prev[0] = cur[0]; prev[1] = cur[1];
        }
    }
}

/* Closes the thin strips of land left between two channels that run side by side (or a hairpin bend): any land pixel
 * with channel on both sides within HY_GAP_PX is taken into the channel, at the nearer side's level. */
static void close_gaps(HRegion *R) {
    static const int D[4][2] = {{1, 0}, {0, 1}, {1, 1}, {1, -1}};
    size_t n = (size_t)HY_RAST * HY_RAST;
    HPix *src = xmalloc(n * sizeof(HPix));
    memcpy(src, R->pix, n * sizeof(HPix));
    for (int iz = 0; iz < HY_RAST; iz++)
        for (int ix = 0; ix < HY_RAST; ix++) {
            HPix *o = &R->pix[iz * HY_RAST + ix];
            if (src[iz * HY_RAST + ix].edge <= 0.0f) continue;
            for (int d = 0; d < 4; d++) {
                const HPix *a = NULL, *b = NULL;
                for (int k = 1; k <= HY_GAP_PX && !a; k++) {
                    int x = ix + D[d][0] * k, z = iz + D[d][1] * k;
                    if (x < 0 || z < 0 || x >= HY_RAST || z >= HY_RAST) break;
                    if (src[z * HY_RAST + x].edge <= 0.0f) a = &src[z * HY_RAST + x];
                }
                for (int k = 1; k <= HY_GAP_PX && a && !b; k++) {
                    int x = ix - D[d][0] * k, z = iz - D[d][1] * k;
                    if (x < 0 || z < 0 || x >= HY_RAST || z >= HY_RAST) break;
                    if (src[z * HY_RAST + x].edge <= 0.0f) b = &src[z * HY_RAST + x];
                }
                if (!a || !b || fabsf(a->level - b->level) > 0.2f) continue;
                const HPix *w = a->level <= b->level ? a : b;
                *o = *w;
                o->edge = -0.5f * w->half_width;
                break;
            }
        }
    free(src);
}

static void raster_lakes(HRegion *R, int cx0, int cz0, float ox, float oz) {
    for (int iz = 0; iz < HY_RAST; iz++)
        for (int ix = 0; ix < HY_RAST; ix++) {
            float fx = (ox + (float)(ix * HY_PIX)) / HY_CELL - 0.5f - (float)cx0, fz = (oz + (float)(iz * HY_PIX)) / HY_CELL - 0.5f - (float)cz0;
            int x0 = (int)floorf(fx), z0 = (int)floorf(fz);
            float tx = fx - (float)x0, tz = fz - (float)z0;
            float wsum = 0.0f, lsum = 0.0f, lvl = 0.0f, dist = 0.0f;
            for (int k = 0; k < 4; k++) {
                int x = CLAMP(x0 + (k & 1), 0, HY_WIN - 1), z = CLAMP(z0 + (k >> 1), 0, HY_WIN - 1);
                float w = ((k & 1) ? tx : 1.0f - tx) * ((k >> 1) ? tz : 1.0f - tz);
                int i = z * HY_WIN + x;
                dist += w * (float)H.lake_d[i];
                if (H.flag[i] & F_LAKE) { lsum += w; lvl += w * (H.F[i] - 0.6f); }
                wsum += w;
            }
            HPix *px = &R->pix[iz * HY_RAST + ix];
            px->lakeness = (u8)lrintf(CLAMP(lsum / wsum, 0.0f, 1.0f) * 255.0f);
            px->lake_level = lsum > 1e-4f ? lvl / lsum : 0.0f;
            /* The chamfer weights are 3 per cell step, so one weight unit is HY_CELL / 3 blocks. */
            px->lake_dist = (u8)MIN(255.0f, dist * ((float)HY_CELL / 3.0f));
        }
}

static void collect_cells(HRegion *R, int rx, int rz, int cx0, int cz0) {
    int count = 0;
    for (int z = HY_CORE_CELLS; z < 2 * HY_CORE_CELLS; z++)
        for (int x = HY_CORE_CELLS; x < 2 * HY_CORE_CELLS; x++) {
            int c = z * HY_WIN + x;
            if (is_river(c) || (H.flag[c] & F_LAKE)) count++;
        }
    R->cells = count ? xmalloc((size_t)count * sizeof(HydroCellInfo)) : NULL;
    R->cell_count = 0;
    for (int z = HY_CORE_CELLS; z < 2 * HY_CORE_CELLS; z++)
        for (int x = HY_CORE_CELLS; x < 2 * HY_CORE_CELLS; x++) {
            int c = z * HY_WIN + x;
            bool river = is_river(c), lake = (H.flag[c] & F_LAKE) != 0;
            if (!river && !lake) continue;
            int t = H.down[c];
            HydroCellInfo *o = &R->cells[R->cell_count++];
            *o = (HydroCellInfo){.cx = cx0 + x, .cz = cz0 + z, .level = river ? H.G[c] : H.F[c] - 0.6f, .flow = (float)H.acc[c],
                                 .half_width = river ? river_half_width(c) : 0.0f, .down_cx = t >= 0 ? cx0 + t % HY_WIN : cx0 + x,
                                 .down_cz = t >= 0 ? cz0 + t / HY_WIN : cz0 + z, .lake_mouth = !lake && t >= 0 && (H.flag[t] & F_LAKE)};
            if (lake) o->flags = 0x80;
            else if (t >= 0 && is_river(t)) {
                float drop = H.G[c] - H.G[t];
                if (drop >= H.p.fall_drop) o->flags |= HYF_FALL; else if (drop >= H.p.rapids_drop) o->flags |= HYF_RAPIDS;
            }
        }
    (void)rx; (void)rz;
}

static void build_region(HRegion *R, int rx, int rz) {
    alloc_scratch();
    int cx0 = (rx - 1) * HY_CORE_CELLS, cz0 = (rz - 1) * HY_CORE_CELLS;
    fill_window(cx0, cz0);
    priority_flood();
    find_lakes();
    select_rivers(cx0, cz0);
    shape_rivers();
    free(R->pix); free(R->cells);
    R->pix = xmalloc((size_t)HY_RAST * HY_RAST * sizeof(HPix));
    for (int i = 0; i < HY_RAST * HY_RAST; i++) R->pix[i] = (HPix){.edge = HY_FAR};
    float ox = (float)(rx * HY_CORE - HY_MARGIN), oz = (float)(rz * HY_CORE - HY_MARGIN);
    stamp_rivers(R, cx0, cz0, ox, oz);
    close_gaps(R);
    raster_lakes(R, cx0, cz0, ox, oz);
    collect_cells(R, rx, rz, cx0, cz0);
    H.built++;
}

/* ----------------------------------------------------------- the cache */

static HRegion *region_get(int rx, int rz) {
    HRegion *victim = &H.regions[0];
    for (int i = 0; i < HY_REGION_SLOTS; i++) {
        HRegion *r = &H.regions[i];
        if (r->used && r->rx == rx && r->rz == rz) { r->stamp = ++H.clock; return r; }
        if (!r->used) victim = r;
        else if (victim->used && r->stamp < victim->stamp) victim = r;
    }
    victim->used = true; victim->rx = rx; victim->rz = rz;
    build_region(victim, rx, rz);
    victim->stamp = ++H.clock;
    return victim;
}

void hydro_init(u64 seed, int sea_level, const HydroParams *p) {
    hydro_shutdown();
    H.seed = seed; H.sea = sea_level; H.p = *p;
    H.lock = mutex_create();
    H.tiles = xcalloc(HY_TILE_SLOTS, sizeof(ElevTile));
    H.ready = true;
}

void hydro_shutdown(void) {
    if (!H.ready && !H.lock) return;
    for (int i = 0; i < HY_REGION_SLOTS; i++) { free(H.regions[i].pix); free(H.regions[i].cells); H.regions[i] = (HRegion){0}; }
    free(H.tiles); H.tiles = NULL;
    free(H.E); free(H.F); free(H.G); free(H.px); free(H.pz); free(H.down); free(H.order); free(H.mainup); free(H.queue); free(H.pos); free(H.taper);
    free(H.acc); free(H.flag); free(H.lake_d); free(H.heap);
    H.E = H.F = H.G = H.px = H.pz = H.taper = NULL; H.pos = NULL; H.down = H.order = H.mainup = H.queue = NULL; H.acc = NULL; H.flag = H.lake_d = NULL; H.heap = NULL;
    if (H.lock) mutex_destroy(H.lock);
    H.lock = NULL; H.ready = false; H.built = 0; H.clock = 0;
}

int hydro_regions_built(void) { return H.built; }

int hydro_region_cells(int rx, int rz, HydroCellInfo *out, int cap) {
    if (!H.ready) return 0;
    mutex_lock(H.lock);
    HRegion *r = region_get(rx, rz);
    int n = MIN(r->cell_count, cap);
    if (out && n > 0) memcpy(out, r->cells, (size_t)n * sizeof(HydroCellInfo));
    int total = r->cell_count;
    mutex_unlock(H.lock);
    return out ? n : total;
}

/* --------------------------------------------------------------- query */

static void sample_region(const HRegion *R, float x, float z, HydroRaw *o, float *valid_w) {
    float ox = (float)(R->rx * HY_CORE - HY_MARGIN), oz = (float)(R->rz * HY_CORE - HY_MARGIN);
    float fx = (x - ox) / HY_PIX, fz = (z - oz) / HY_PIX;
    int x0 = CLAMP((int)floorf(fx), 0, HY_RAST - 2), z0 = CLAMP((int)floorf(fz), 0, HY_RAST - 2);
    float tx = CLAMP(fx - (float)x0, 0.0f, 1.0f), tz = CLAMP(fz - (float)z0, 0.0f, 1.0f);
    float vw = 0.0f, lvl = 0.0f, hw = 0.0f, grade = 0.0f, flow = 0.0f, edge = 0.0f, lk = 0.0f, lklvl = 0.0f, lkw = 0.0f, lkd = 0.0f;
    const HPix *best = NULL; float best_w = -1.0f;
    for (int k = 0; k < 4; k++) {
        const HPix *p = &R->pix[(z0 + (k >> 1)) * HY_RAST + x0 + (k & 1)];
        float w = ((k & 1) ? tx : 1.0f - tx) * ((k >> 1) ? tz : 1.0f - tz);
        edge += w * p->edge;
        bool valid = p->edge < HY_FAR - 0.5f;
        if (valid) {
            vw += w; lvl += w * p->level; hw += w * p->half_width; grade += w * (float)p->grade; flow += w * p->flow;
            if (w > best_w) { best_w = w; best = p; }
        }
        float l = (float)p->lakeness * (1.0f / 255.0f);
        lk += w * l; lklvl += w * l * p->lake_level; lkw += w * l; lkd += w * (float)p->lake_dist;
    }
    *o = (HydroRaw){.edge = edge, .lakeness = lk, .lake_dist = lkd};
    if (lkw > 1e-5f) o->lake_level = lklvl / lkw;
    if (vw > 1e-5f) {
        o->river = true;
        o->level = lvl / vw; o->half_width = hw / vw; o->grade = grade / vw * (1.0f / 255.0f); o->flow = flow / vw;
        o->dir_x = (float)best->dir_x / 127.0f; o->dir_z = (float)best->dir_z / 127.0f; o->flags = best->flags;
    }
    *valid_w = vw;
}

void hydro_query(float x, float z, HydroRaw *out) {
    memset(out, 0, sizeof *out);
    out->edge = HY_FAR; out->lake_dist = HY_FAR;
    if (!H.ready) return;
    int rx = floordiv((int)floorf(x), HY_CORE), rz = floordiv((int)floorf(z), HY_CORE);
    float lx = x - (float)(rx * HY_CORE), lz = z - (float)(rz * HY_CORE);
    /* Blend weights per axis: 1 deep inside the region, 0.5 on the seam, 0 at HY_SEAM beyond it. */
    int ax[2] = {rx, rx}, az[2] = {rz, rz};
    float wx[2] = {1.0f, 0.0f}, wz[2] = {1.0f, 0.0f};
    if (lx < HY_SEAM) { ax[1] = rx - 1; wx[0] = sm01((lx + HY_SEAM) / (2.0f * HY_SEAM)); wx[1] = 1.0f - wx[0]; }
    else if (lx > HY_CORE - HY_SEAM) { ax[1] = rx + 1; wx[0] = sm01(((float)HY_CORE - lx + HY_SEAM) / (2.0f * HY_SEAM)); wx[1] = 1.0f - wx[0]; }
    if (lz < HY_SEAM) { az[1] = rz - 1; wz[0] = sm01((lz + HY_SEAM) / (2.0f * HY_SEAM)); wz[1] = 1.0f - wz[0]; }
    else if (lz > HY_CORE - HY_SEAM) { az[1] = rz + 1; wz[0] = sm01(((float)HY_CORE - lz + HY_SEAM) / (2.0f * HY_SEAM)); wz[1] = 1.0f - wz[0]; }
    float edge = 0.0f, vsum = 0.0f, lvl = 0.0f, hw = 0.0f, grade = 0.0f, flow = 0.0f;
    float lk = 0.0f, lklvl = 0.0f, lkd = 0.0f;
    float top_w = -1.0f; HydroRaw top = {0};
    mutex_lock(H.lock);
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) {
            float w = wx[i] * wz[j];
            if (w <= 0.0f) continue;
            HydroRaw r; float vw;
            sample_region(region_get(ax[i], az[j]), x, z, &r, &vw);
            edge += w * r.edge;
            lk += w * r.lakeness; lklvl += w * r.lakeness * r.lake_level; lkd += w * r.lake_dist;
            if (r.river) {
                float vwt = w * vw;
                vsum += vwt; lvl += vwt * r.level; hw += vwt * r.half_width; grade += vwt * r.grade; flow += vwt * r.flow;
                if (w > top_w) { top_w = w; top = r; }
            }
        }
    mutex_unlock(H.lock);
    out->edge = edge;
    out->lakeness = lk;
    out->lake_level = lk > 1e-5f ? lklvl / lk : 0.0f;
    out->lake_dist = lkd;
    if (vsum > 1e-6f) {
        out->river = true;
        out->level = lvl / vsum; out->half_width = hw / vsum; out->grade = grade / vsum; out->flow = flow / vsum;
        out->dir_x = top.dir_x; out->dir_z = top.dir_z; out->flags = top.flags;
    }
}
