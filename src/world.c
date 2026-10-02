/* World storage and streaming: palette-compressed chunks, the chunk and column maps,
 * generation and meshing job scheduling, snapshots for lock-free meshing, and edits.
 * Everything here runs on the main thread except the job bodies, which only touch
 * their own private copies. */
#include "dfe.h"

/* ---------------------------------------------------------------- hash map */

typedef struct PtrMap {
    u64 *keys;
    void **vals;
    u32 cap, count, used;
} PtrMap;

#define TOMB ((void *)1)

static u64 pack3(int x, int y, int z) {
    return (u64)(x & 0x1FFFFF) | ((u64)(y & 0x1FFFFF) << 21) | ((u64)(z & 0x1FFFFF) << 42);
}

static u32 key_slot(u64 key, u32 mask) { return (u32)(hash64(key) & mask); }

static void *ptrmap_get(const PtrMap *m, u64 key) {
    if (!m->cap) return NULL;
    u32 mask = m->cap - 1, i = key_slot(key, mask);
    while (m->vals[i]) {
        if (m->vals[i] != TOMB && m->keys[i] == key) return m->vals[i];
        i = (i + 1) & mask;
    }
    return NULL;
}

static void ptrmap_rehash(PtrMap *m, u32 new_cap) {
    PtrMap n = {xcalloc(new_cap, sizeof(u64)), xcalloc(new_cap, sizeof(void *)), new_cap, 0, 0};
    for (u32 i = 0; i < m->cap; i++) {
        if (!m->vals[i] || m->vals[i] == TOMB) continue;
        u32 s = key_slot(m->keys[i], new_cap - 1);
        while (n.vals[s]) s = (s + 1) & (new_cap - 1);
        n.keys[s] = m->keys[i];
        n.vals[s] = m->vals[i];
        n.count++;
        n.used++;
    }
    free(m->keys);
    free(m->vals);
    *m = n;
}

static void ptrmap_set(PtrMap *m, u64 key, void *val) {
    if (!m->cap || (m->used + 1) * 10 > m->cap * 7) ptrmap_rehash(m, m->cap ? (m->count * 4 > m->cap ? m->cap * 2 : m->cap) : 1024);
    u32 mask = m->cap - 1, i = key_slot(key, mask);
    while (m->vals[i] && m->vals[i] != TOMB && m->keys[i] != key) i = (i + 1) & mask;
    if (!m->vals[i]) m->used++;
    if (!m->vals[i] || m->vals[i] == TOMB) m->count++;
    m->keys[i] = key;
    m->vals[i] = val;
}

static void ptrmap_remove(PtrMap *m, u64 key) {
    if (!m->cap) return;
    u32 mask = m->cap - 1, i = key_slot(key, mask);
    while (m->vals[i]) {
        if (m->vals[i] != TOMB && m->keys[i] == key) { m->vals[i] = TOMB; m->count--; return; }
        i = (i + 1) & mask;
    }
}

static void ptrmap_free(PtrMap *m) {
    free(m->keys);
    free(m->vals);
    memset(m, 0, sizeof *m);
}

/* ----------------------------------------------------------- palette store */

static inline u16 index_get(const Chunk *c, int idx) {
    u32 bit = (u32)idx * c->bits;
    return (u16)((c->data[bit >> 5] >> (bit & 31)) & ((1u << c->bits) - 1));
}

static inline void index_set(Chunk *c, int idx, u32 v) {
    u32 bit = (u32)idx * c->bits, mask = (1u << c->bits) - 1;
    u32 *w = &c->data[bit >> 5];
    *w = (*w & ~(mask << (bit & 31))) | (v << (bit & 31));
}

Chunk *chunk_create(int cx, int cy, int cz) {
    Chunk *c = xcalloc(1, sizeof *c);
    c->cx = cx; c->cy = cy; c->cz = cz;
    for (int i = 0; i < LAYER_COUNT; i++) c->mesh[i].page = -1;
    return c;
}

void chunk_destroy(Chunk *c) {
    if (!c) return;
    free(c->pal);
    free(c->data);
    free(c->light);
    free(c);
}

u16 chunk_get(const Chunk *c, int idx) {
    if (c->bits == 0) return c->uniform;
    if (c->bits == 16) return ((const u16 *)c->data)[idx];
    return c->pal[index_get(c, idx)];
}

static void chunk_repack(Chunk *c, int new_bits) {
    u16 *flat = xmalloc(CHUNK_VOL * sizeof(u16));
    chunk_unpack(c, flat);
    if (new_bits == 16) {
        free(c->pal);
        free(c->data);
        c->pal = NULL;
        c->pal_n = 0;
        c->data = (u32 *)flat;
        c->bits = 16;
        return;
    }
    u32 *data = xcalloc((size_t)CHUNK_VOL * (size_t)new_bits / 32, sizeof(u32));
    u16 *pal = xmalloc(((size_t)1 << new_bits) * sizeof(u16));
    memcpy(pal, c->pal, c->pal_n * sizeof(u16));
    int old_bits = c->bits;
    u32 *old_data = c->data;
    (void)old_bits;
    for (int i = 0; i < CHUNK_VOL; i++) {
        u32 pi = index_get(c, i);
        u32 bit = (u32)i * (u32)new_bits;
        data[bit >> 5] |= pi << (bit & 31);
    }
    free(old_data);
    free(c->pal);
    free(flat);
    c->pal = pal;
    c->data = data;
    c->bits = (u8)new_bits;
}

void chunk_set(Chunk *c, int idx, u16 state) {
    if (c->bits == 0) {
        if (c->uniform == state) return;
        c->bits = 1;
        c->pal = xmalloc(2 * sizeof(u16));
        c->pal[0] = c->uniform;
        c->pal_n = 1;
        c->data = xcalloc(CHUNK_VOL / 32, sizeof(u32));
    }
    if (c->bits == 16) { ((u16 *)c->data)[idx] = state; return; }
    int pi = -1;
    for (int i = 0; i < c->pal_n; i++) if (c->pal[i] == state) { pi = i; break; }
    if (pi < 0) {
        if (c->pal_n == (1u << c->bits)) {
            int nb = c->bits == 1 ? 2 : c->bits == 2 ? 4 : c->bits == 4 ? 8 : 16;
            chunk_repack(c, nb);
            if (c->bits == 16) { ((u16 *)c->data)[idx] = state; return; }
        }
        pi = c->pal_n++;
        c->pal[pi] = state;
    }
    index_set(c, idx, (u32)pi);
}

void chunk_unpack(const Chunk *c, u16 *flat) {
    if (c->bits == 0) {
        for (int i = 0; i < CHUNK_VOL; i++) flat[i] = c->uniform;
    } else if (c->bits == 16) {
        memcpy(flat, c->data, CHUNK_VOL * sizeof(u16));
    } else {
        for (int i = 0; i < CHUNK_VOL; i++) flat[i] = c->pal[index_get(c, i)];
    }
}

void chunk_pack_from(Chunk *c, const u16 *flat) {
    free(c->pal);
    free(c->data);
    c->pal = NULL; c->data = NULL; c->pal_n = 0; c->bits = 0;
    int i = 1;
    while (i < CHUNK_VOL && flat[i] == flat[0]) i++;
    if (i == CHUNK_VOL) { c->uniform = flat[0]; return; }

    u16 pal[256];
    u16 slot_key[1024];
    u16 slot_idx[1024];
    bool slot_used[1024] = {0};
    int n = 0;
    bool overflow = false;
    for (int k = 0; k < CHUNK_VOL && !overflow; k++) {
        u16 v = flat[k];
        u32 s = (v * 2654435761u >> 22) & 1023;
        while (slot_used[s] && slot_key[s] != v) s = (s + 1) & 1023;
        if (!slot_used[s]) {
            if (n == 256) { overflow = true; break; }
            slot_used[s] = true; slot_key[s] = v; slot_idx[s] = (u16)n; pal[n++] = v;
        }
    }
    if (overflow) {
        c->bits = 16;
        c->data = xmalloc(CHUNK_VOL * sizeof(u16));
        memcpy(c->data, flat, CHUNK_VOL * sizeof(u16));
        return;
    }
    int bits = n <= 2 ? 1 : n <= 4 ? 2 : n <= 16 ? 4 : 8;
    c->bits = (u8)bits;
    c->pal_n = (u16)n;
    c->pal = xmalloc(((size_t)1 << bits) * sizeof(u16));
    memcpy(c->pal, pal, (size_t)n * sizeof(u16));
    c->data = xcalloc((size_t)CHUNK_VOL * (size_t)bits / 32, sizeof(u32));
    for (int k = 0; k < CHUNK_VOL; k++) {
        u16 v = flat[k];
        u32 s = (v * 2654435761u >> 22) & 1023;
        while (slot_key[s] != v) s = (s + 1) & 1023;
        u32 bit = (u32)k * (u32)bits;
        c->data[bit >> 5] |= (u32)slot_idx[s] << (bit & 31);
    }
}

void chunk_compact(Chunk *c) {
    if (c->bits == 0) return;
    u16 *flat = xmalloc(CHUNK_VOL * sizeof(u16));
    chunk_unpack(c, flat);
    chunk_pack_from(c, flat);
    free(flat);
}

size_t chunk_memory_bytes(const Chunk *c) {
    size_t n = sizeof *c;
    if (c->bits) n += (c->bits == 16 ? CHUNK_VOL * 2 : (size_t)CHUNK_VOL * c->bits / 8) + c->pal_n * 2;
    if (c->light) n += CHUNK_VOL * 2;
    return n;
}

u16 chunk_get_light(const Chunk *c, int idx) { return c->light ? c->light[idx] : c->light_uniform; }

void chunk_set_light(Chunk *c, int idx, u16 light) {
    if (!c->light) {
        if (c->light_uniform == light) return;
        c->light = xmalloc(CHUNK_VOL * sizeof(u16));
        for (int i = 0; i < CHUNK_VOL; i++) c->light[i] = c->light_uniform;
    }
    c->light[idx] = light;
}

void chunk_set_light_from(Chunk *c, const u16 *flat) {
    free(c->light);
    c->light = NULL;
    int i = 1;
    while (i < CHUNK_VOL && flat[i] == flat[0]) i++;
    c->light_uniform = flat[0];
    if (i == CHUNK_VOL) return;
    c->light = xmalloc(CHUNK_VOL * sizeof(u16));
    memcpy(c->light, flat, CHUNK_VOL * sizeof(u16));
}

/* ------------------------------------------------------------------ world */

#define MAX_WORKERS 16
#define MAX_STREAM_RADIUS 40
#define LIGHT_STEPS_PER_FRAME 40000
#define MESH_JOBS_PER_WORKER 2
#define GEN_JOBS_PER_WORKER 2
#define UNLOAD_MARGIN 2
#define NEAR_PRIORITY_CHUNKS 2.0f
#define DIRECTION_PRIORITY_BIAS 3.0f

typedef struct ColumnOffset { int dx, dz; float dist2; } ColumnOffset;

typedef struct WorkerBuffers {
    u16 *states, *light;
    GenScratch *scratch;
    int layers;
} WorkerBuffers;

typedef struct GenJob {
    int cx, cz;
    u32 serial;
    int lo, hi;
    Chunk **chunks;
} GenJob;

typedef struct MeshJob {
    MeshInput in;
    MeshOutput out;
} MeshJob;

static struct {
    bool active;
    u64 seed;
    PtrMap chunks, columns;
    int rd, fcx, fcy, fcz;
    u32 gen_serial, mesh_serial;
    int gen_inflight, mesh_inflight;
    int band_lo, band_hi;
    u16 deep_state;
    ColumnOffset *offsets;
    int offset_count;
    WorkerBuffers wb[MAX_WORKERS];
    Chunk *cache_chunk;
    u64 cache_key;
    WorldStats stats;
} W;

void world_init(u64 seed) {
    memset(&W, 0, sizeof W);
    W.seed = seed;
    gen_init(seed);
    gen_band(&W.band_lo, &W.band_hi);
    W.deep_state = gen_deep_state();
    int r = MAX_STREAM_RADIUS;
    W.offsets = xmalloc((size_t)(2 * r + 1) * (size_t)(2 * r + 1) * sizeof(ColumnOffset));
    for (int dz = -r; dz <= r; dz++)
        for (int dx = -r; dx <= r; dx++) {
            float d2 = (float)(dx * dx + dz * dz);
            if (d2 <= (float)(r * r)) W.offsets[W.offset_count++] = (ColumnOffset){dx, dz, d2};
        }
    /* Nearest first, so scans stop early once the radius is exceeded. */
    for (int i = 1; i < W.offset_count; i++) {
        ColumnOffset v = W.offsets[i];
        int j = i - 1;
        while (j >= 0 && W.offsets[j].dist2 > v.dist2) { W.offsets[j + 1] = W.offsets[j]; j--; }
        W.offsets[j + 1] = v;
    }
    W.active = true;
}

static void column_unload(Column *col);

void world_shutdown(void) {
    if (!W.active) return;
    W.active = false;
    jobs_wait_idle();
    while (jobs_in_flight() > 0) jobs_pump(1.0);
    for (u32 i = 0; i < W.columns.cap; i++)
        if (W.columns.vals[i] && W.columns.vals[i] != TOMB) column_unload(W.columns.vals[i]);
    ptrmap_free(&W.columns);
    ptrmap_free(&W.chunks);
    for (int i = 0; i < MAX_WORKERS; i++) {
        free(W.wb[i].states);
        free(W.wb[i].light);
        if (W.wb[i].scratch) gen_scratch_destroy(W.wb[i].scratch);
    }
    free(W.offsets);
    gen_shutdown();
    memset(&W, 0, sizeof W);
}

u64 world_seed(void) { return W.seed; }

Chunk *world_chunk(int cx, int cy, int cz) {
    u64 key = pack3(cx, cy, cz);
    if (W.cache_chunk && W.cache_key == key) return W.cache_chunk;
    Chunk *c = ptrmap_get(&W.chunks, key);
    if (c) { W.cache_chunk = c; W.cache_key = key; }
    return c;
}

Column *world_column(int cx, int cz) { return ptrmap_get(&W.columns, pack3(cx, 0, cz)); }

u16 world_get_state(int x, int y, int z) {
    int cx = x >> CHUNK_SHIFT, cy = y >> CHUNK_SHIFT, cz = z >> CHUNK_SHIFT;
    Chunk *c = world_chunk(cx, cy, cz);
    if (c) return chunk_get(c, ((y & 31) << 10) | ((z & 31) << 5) | (x & 31));
    Column *col = world_column(cx, cz);
    if (!col || col->state != COLUMN_READY) return STATE_UNLOADED;
    return cy > col->hi_cy ? STATE_AIR : col->deep_state;
}

u16 world_get_light(int x, int y, int z) {
    int cx = x >> CHUNK_SHIFT, cy = y >> CHUNK_SHIFT, cz = z >> CHUNK_SHIFT;
    Chunk *c = world_chunk(cx, cy, cz);
    if (c) return chunk_get_light(c, ((y & 31) << 10) | ((z & 31) << 5) | (x & 31));
    Column *col = world_column(cx, cz);
    if (!col || col->state != COLUMN_READY) return 0;
    return cy > col->hi_cy ? LIGHT_FULL_SKY : 0;
}

void world_mark_mesh_dirty(Chunk *c) { if (c) c->flags |= CF_MESH_DIRTY; }

void world_mark_neighbours_dirty(int cx, int cy, int cz) {
    for (int d = 0; d < 6; d++) {
        Chunk *n = world_chunk(cx + DIR_VEC[d][0], cy + DIR_VEC[d][1], cz + DIR_VEC[d][2]);
        if (n) n->flags |= CF_MESH_DIRTY;
    }
}

/* A changed voxel is also read by every chunk whose 1-voxel padding contains it. */
static void mark_voxel_dirty(int x, int y, int z) {
    int lx = x & 31, ly = y & 31, lz = z & 31;
    int xs[2] = {0, lx == 0 ? -1 : (lx == 31 ? 1 : 0)};
    int ys[2] = {0, ly == 0 ? -1 : (ly == 31 ? 1 : 0)};
    int zs[2] = {0, lz == 0 ? -1 : (lz == 31 ? 1 : 0)};
    for (int a = 0; a < 2; a++)
        for (int b = 0; b < 2; b++)
            for (int c2 = 0; c2 < 2; c2++) {
                Chunk *c = world_chunk((x >> 5) + xs[a], (y >> 5) + ys[b], (z >> 5) + zs[c2]);
                if (c) c->flags |= CF_MESH_DIRTY;
            }
}

void world_light_touched(int x, int y, int z) { mark_voxel_dirty(x, y, z); }

/* Gives an edit somewhere above or below the generated band a real chunk to live in. */
static Chunk *column_expand_to(Column *col, int cy) {
    while (cy > col->hi_cy) {
        Chunk *c = chunk_create(col->cx, ++col->hi_cy, col->cz);
        c->uniform = STATE_AIR;
        c->light_uniform = LIGHT_FULL_SKY;
        c->flags = CF_GENERATED | CF_VIRTUAL;
        ptrmap_set(&W.chunks, pack3(c->cx, c->cy, c->cz), c);
    }
    while (cy < col->lo_cy) {
        Chunk *c = chunk_create(col->cx, --col->lo_cy, col->cz);
        c->uniform = col->deep_state;
        c->light_uniform = 0;
        c->flags = CF_GENERATED | CF_VIRTUAL;
        ptrmap_set(&W.chunks, pack3(c->cx, c->cy, c->cz), c);
    }
    return world_chunk(col->cx, cy, col->cz);
}

Chunk *world_chunk_materialize(int cx, int cy, int cz) {
    Chunk *c = world_chunk(cx, cy, cz);
    if (c) return c;
    Column *col = world_column(cx, cz);
    if (!col || col->state != COLUMN_READY) return NULL;
    return column_expand_to(col, cy);
}

bool world_set_state(int x, int y, int z, u16 state) {
    int cx = x >> CHUNK_SHIFT, cy = y >> CHUNK_SHIFT, cz = z >> CHUNK_SHIFT;
    Chunk *c = world_chunk_materialize(cx, cy, cz);
    if (!c) return false;
    int idx = ((y & 31) << 10) | ((z & 31) << 5) | (x & 31);
    u16 old = chunk_get(c, idx);
    if (old == state) return false;
    chunk_set(c, idx, state);
    c->flags |= CF_SAVE_DIRTY | CF_MESH_DIRTY;
    c->flags &= ~CF_VIRTUAL;
    mark_voxel_dirty(x, y, z);
    light_on_block_changed(x, y, z, old, state);
    return true;
}

void world_set_light_raw(int x, int y, int z, u16 light) {
    Chunk *c = world_chunk(x >> CHUNK_SHIFT, y >> CHUNK_SHIFT, z >> CHUNK_SHIFT);
    if (!c) return;
    chunk_set_light(c, ((y & 31) << 10) | ((z & 31) << 5) | (x & 31), light);
    c->flags |= CF_SAVE_DIRTY;
    mark_voxel_dirty(x, y, z);
}

void world_each_chunk(void (*fn)(Chunk *, void *), void *user) {
    for (u32 i = 0; i < W.chunks.cap; i++)
        if (W.chunks.vals[i] && W.chunks.vals[i] != TOMB) fn(W.chunks.vals[i], user);
}

/* ------------------------------------------------------------- generation */

static WorkerBuffers *worker_buffers(int worker, int layers) {
    WorkerBuffers *wb = &W.wb[worker % MAX_WORKERS];
    if (wb->layers != layers) {
        free(wb->states);
        free(wb->light);
        wb->states = xmalloc((size_t)layers * CHUNK_VOL * sizeof(u16));
        wb->light = xmalloc((size_t)layers * CHUNK_VOL * sizeof(u16));
        wb->layers = layers;
    }
    if (!wb->scratch) wb->scratch = gen_scratch_create();
    return wb;
}

static void gen_job_run(void *data, int worker) {
    GenJob *j = data;
    int layers = j->hi - j->lo + 1;
    WorkerBuffers *wb = worker_buffers(worker, layers);
    gen_column(wb->scratch, j->cx, j->cz, wb->states);
    light_init_column(wb->states, layers, wb->light);
    j->chunks = xcalloc((size_t)layers, sizeof(Chunk *));
    for (int k = 0; k < layers; k++) {
        Chunk *c = chunk_create(j->cx, j->lo + k, j->cz);
        chunk_pack_from(c, wb->states + (size_t)k * CHUNK_VOL);
        chunk_set_light_from(c, wb->light + (size_t)k * CHUNK_VOL);
        c->flags = CF_GENERATED;
        j->chunks[k] = c;
    }
}

static void gen_job_complete(void *data) {
    GenJob *j = data;
    int layers = j->hi - j->lo + 1;
    W.gen_inflight--;
    Column *col = W.active ? world_column(j->cx, j->cz) : NULL;
    if (!col || col->state != COLUMN_PENDING || col->flags != j->serial) {
        for (int k = 0; k < layers; k++) chunk_destroy(j->chunks[k]);
    } else {
        for (int k = 0; k < layers; k++) ptrmap_set(&W.chunks, pack3(j->cx, j->lo + k, j->cz), j->chunks[k]);
        col->state = COLUMN_READY;
        W.stats.columns_generated++;
        light_seed_column_borders(j->cx, j->cz);
    }
    free(j->chunks);
    free(j);
}

static void column_submit(int cx, int cz, float prio) {
    Column *col = xcalloc(1, sizeof *col);
    col->cx = cx; col->cz = cz;
    col->lo_cy = W.band_lo; col->hi_cy = W.band_hi;
    col->deep_state = W.deep_state;
    col->state = COLUMN_PENDING;
    col->flags = ++W.gen_serial;
    ptrmap_set(&W.columns, pack3(cx, 0, cz), col);
    GenJob *j = xcalloc(1, sizeof *j);
    j->cx = cx; j->cz = cz; j->serial = col->flags;
    j->lo = W.band_lo; j->hi = W.band_hi;
    W.gen_inflight++;
    jobs_submit(JOB_KIND_GEN, prio, gen_job_run, gen_job_complete, j);
}

static void column_unload(Column *col) {
    for (int cy = col->lo_cy; cy <= col->hi_cy; cy++) {
        Chunk *c = ptrmap_get(&W.chunks, pack3(col->cx, cy, col->cz));
        if (!c) continue;
        scene_free_chunk(c);
        ptrmap_remove(&W.chunks, pack3(col->cx, cy, col->cz));
        if (W.cache_chunk == c) W.cache_chunk = NULL;
        chunk_destroy(c);
    }
    ptrmap_remove(&W.columns, pack3(col->cx, 0, col->cz));
    free(col);
}

/* --------------------------------------------------------------- meshing */

/* Source range inside the neighbour and destination coordinate (or offset when d is 0) inside the padded cube. */
static void snapshot_axis(int d, int *src_lo, int *src_hi, int *dst_bias) {
    if (d < 0) { *src_lo = *src_hi = 31; *dst_bias = 0; }
    else if (d > 0) { *src_lo = *src_hi = 0; *dst_bias = 33; }
    else { *src_lo = 0; *src_hi = 31; *dst_bias = 1; }
}

static void snapshot_copy_neighbour(u16 *dst_states, u16 *dst_light, int cx, int cy, int cz, int dx, int dy, int dz) {
    int bx = cx + dx, by = cy + dy, bz = cz + dz;
    Chunk *n = world_chunk(bx, by, bz);
    u16 fill_state = STATE_AIR, fill_light = 0;
    if (!n) {
        Column *col = world_column(bx, bz);
        if (col && col->state == COLUMN_READY) {
            if (by > col->hi_cy) fill_light = LIGHT_FULL_SKY;
            else fill_state = col->deep_state;
        }
    }
    int xl, xh, xb, yl, yh, yb, zl, zh, zb;
    snapshot_axis(dx, &xl, &xh, &xb);
    snapshot_axis(dy, &yl, &yh, &yb);
    snapshot_axis(dz, &zl, &zh, &zb);
    for (int y = yl; y <= yh; y++)
        for (int z = zl; z <= zh; z++)
            for (int x = xl; x <= xh; x++) {
                int px = dx == 0 ? x + xb : xb, py = dy == 0 ? y + yb : yb, pz = dz == 0 ? z + zb : zb;
                int di = (py * MESH_PAD + pz) * MESH_PAD + px;
                if (n) {
                    int si = (y << 10) | (z << 5) | x;
                    dst_states[di] = chunk_get(n, si);
                    dst_light[di] = chunk_get_light(n, si);
                } else {
                    dst_states[di] = fill_state;
                    dst_light[di] = fill_light;
                }
            }
}

void mesh_snapshot(MeshInput *in, int cx, int cy, int cz) {
    in->cx = cx; in->cy = cy; in->cz = cz;
    in->states = xmalloc(MESH_PAD_VOL * sizeof(u16));
    in->light = xmalloc(MESH_PAD_VOL * sizeof(u16));
    Chunk *c = world_chunk(cx, cy, cz);
    u16 *flat = xmalloc(CHUNK_VOL * sizeof(u16));
    chunk_unpack(c, flat);
    for (int y = 0; y < 32; y++)
        for (int z = 0; z < 32; z++)
            memcpy(in->states + ((y + 1) * MESH_PAD + (z + 1)) * MESH_PAD + 1, flat + (y << 10) + (z << 5), 32 * sizeof(u16));
    if (c->light) {
        for (int y = 0; y < 32; y++)
            for (int z = 0; z < 32; z++)
                memcpy(in->light + ((y + 1) * MESH_PAD + (z + 1)) * MESH_PAD + 1, c->light + (y << 10) + (z << 5), 32 * sizeof(u16));
    } else {
        for (int y = 0; y < 32; y++)
            for (int z = 0; z < 32; z++)
                for (int x = 0; x < 32; x++) in->light[((y + 1) * MESH_PAD + (z + 1)) * MESH_PAD + 1 + x] = c->light_uniform;
    }
    free(flat);
    for (int dy = -1; dy <= 1; dy++)
        for (int dz = -1; dz <= 1; dz++)
            for (int dx = -1; dx <= 1; dx++)
                if (dx || dy || dz) snapshot_copy_neighbour(in->states, in->light, cx, cy, cz, dx, dy, dz);
}

void mesh_input_free(MeshInput *in) {
    free(in->states);
    free(in->light);
    in->states = in->light = NULL;
}

static void mesh_job_run(void *data, int worker) {
    MeshJob *j = data;
    mesh_build(&j->in, &j->out);
    mesh_input_free(&j->in);
}

static void mesh_job_complete(void *data) {
    MeshJob *j = data;
    W.mesh_inflight--;
    Chunk *c = W.active ? world_chunk(j->out.cx, j->out.cy, j->out.cz) : NULL;
    if (c && c->mesh_version == j->out.version) {
        c->flags &= ~CF_MESH_PENDING;
        c->flags |= CF_MESHED_ONCE;
        c->conn = j->out.conn;
        scene_upload_mesh(c, &j->out);
        W.stats.chunks_meshed_total++;
    }
    mesh_output_free(&j->out);
    free(j);
}

static void mesh_submit(Chunk *c, float prio) {
    MeshJob *j = xcalloc(1, sizeof *j);
    mesh_snapshot(&j->in, c->cx, c->cy, c->cz);
    c->mesh_version = ++W.mesh_serial;
    j->in.version = j->out.version = c->mesh_version;
    c->flags |= CF_MESH_PENDING;
    c->flags &= ~CF_MESH_DIRTY;
    W.mesh_inflight++;
    jobs_submit(JOB_KIND_MESH, prio, mesh_job_run, mesh_job_complete, j);
}

/* ---------------------------------------------------------------- stream */

typedef struct Candidate { float prio; int a, b, c; } Candidate;

static void candidate_insert(Candidate *list, int *n, int cap, Candidate cand) {
    int i;
    if (*n < cap) i = (*n)++;
    else {
        if (list[cap - 1].prio <= cand.prio) return;
        i = cap - 1;
    }
    while (i > 0 && list[i - 1].prio > cand.prio) { list[i] = list[i - 1]; i--; }
    list[i] = cand;
}

static float stream_priority(float dx, float dz, V3 fwd) {
    float dist = sqrtf(dx * dx + dz * dz);
    if (dist <= NEAR_PRIORITY_CHUNKS) return dist - 100.0f;
    float dir = (dx * fwd.x + dz * fwd.z) / (dist * MAX(sqrtf(fwd.x * fwd.x + fwd.z * fwd.z), 1e-4f));
    return dist - DIRECTION_PRIORITY_BIAS * dir;
}

static bool column_neighbourhood_ready(int cx, int cz) {
    for (int dz = -1; dz <= 1; dz++)
        for (int dx = -1; dx <= 1; dx++) {
            Column *n = world_column(cx + dx, cz + dz);
            if (!n || n->state != COLUMN_READY) return false;
        }
    return true;
}

void world_stream(V3 focus, V3 fwd, int rd, bool first_load) {
    if (!W.active) return;
    rd = CLAMP(rd, 2, MAX_STREAM_RADIUS - 2);
    W.rd = rd;
    W.fcx = ifloor(focus.x / 32.0f);
    W.fcy = ifloor(focus.y / 32.0f);
    W.fcz = ifloor(focus.z / 32.0f);
    int workers = jobs_worker_count();

    /* Unload with hysteresis so walking along a border does not thrash. */
    int unload_r2 = (rd + UNLOAD_MARGIN + 1) * (rd + UNLOAD_MARGIN + 1);
    for (u32 i = 0; i < W.columns.cap; i++) {
        Column *col = W.columns.vals[i] && W.columns.vals[i] != TOMB ? W.columns.vals[i] : NULL;
        if (!col) continue;
        int dx = col->cx - W.fcx, dz = col->cz - W.fcz;
        if (dx * dx + dz * dz > unload_r2) {
            if (col->state == COLUMN_PENDING) { ptrmap_remove(&W.columns, pack3(col->cx, 0, col->cz)); free(col); }
            else column_unload(col);
        }
    }

    /* Generation: two column rings beyond the draw radius so every edge chunk has all eight neighbour columns to mesh against. */
    int gen_cap = first_load ? 4096 : workers * GEN_JOBS_PER_WORKER;
    int gen_r2 = (rd + 2) * (rd + 2);
    Candidate best[64];
    int nbest = 0, want = MIN(MAX(gen_cap - W.gen_inflight, 0), 64), missing = 0;
    {
        for (int i = 0; i < W.offset_count && W.offsets[i].dist2 <= (float)gen_r2; i++) {
            int cx = W.fcx + W.offsets[i].dx, cz = W.fcz + W.offsets[i].dz;
            if (world_column(cx, cz)) continue;
            missing++;
            if (want <= 0) continue;
            candidate_insert(best, &nbest, want, (Candidate){stream_priority((float)W.offsets[i].dx, (float)W.offsets[i].dz, fwd), cx, cz, 0});
        }
        for (int i = 0; i < nbest; i++) column_submit(best[i].a, best[i].b, best[i].prio);
    }

    light_process(first_load ? 1 << 30 : LIGHT_STEPS_PER_FRAME);

    /* Meshing: only when all eight neighbouring columns exist and lighting has settled. */
    int mesh_cap = first_load ? 4096 : workers * MESH_JOBS_PER_WORKER + 2;
    bool light_idle = light_queue_size() == 0;
    Candidate meshes[64];
    int nm = 0, want_m = MIN(MAX(mesh_cap - W.mesh_inflight, 0), 64);
    int draw_r2 = rd * rd;
    int chunks_total = 0, chunks_meshed = 0, unmeshed = 0;
    for (int i = 0; i < W.offset_count && W.offsets[i].dist2 <= (float)draw_r2; i++) {
        int cx = W.fcx + W.offsets[i].dx, cz = W.fcz + W.offsets[i].dz;
        Column *col = world_column(cx, cz);
        if (!col || col->state != COLUMN_READY) continue;
        bool ready = column_neighbourhood_ready(cx, cz);
        for (int cy = col->lo_cy; cy <= col->hi_cy; cy++) {
            Chunk *c = world_chunk(cx, cy, cz);
            if (!c) continue;
            chunks_total++;
            if (c->flags & CF_HAS_MESH) chunks_meshed++;
            if (!(c->flags & CF_MESHED_ONCE)) unmeshed++;
            if (c->flags & CF_MESH_PENDING) continue;
            bool needs = (c->flags & CF_MESH_DIRTY) || !(c->flags & CF_MESHED_ONCE);
            if (!needs || !ready) continue;
            bool edit = (c->flags & CF_MESHED_ONCE) != 0;
            if (!edit && !light_idle) continue;
            if (!edit && c->bits == 0 && c->uniform == STATE_AIR) {
                /* Empty chunks need no mesh job, only a connectivity record of "everything connected". */
                c->conn = 0x7FFF;
                c->flags |= CF_MESHED_ONCE;
                c->flags &= ~CF_MESH_DIRTY;
                continue;
            }
            if (want_m <= 0 && !edit) continue;
            float p = edit ? -1000.0f : stream_priority((float)W.offsets[i].dx, (float)W.offsets[i].dz, fwd) + 0.1f * (float)abs(cy - W.fcy);
            candidate_insert(meshes, &nm, 64, (Candidate){p, cx, cy, cz});
        }
    }
    for (int i = 0; i < nm; i++) {
        Chunk *c = world_chunk(meshes[i].a, meshes[i].b, meshes[i].c);
        bool edit = meshes[i].prio < -500.0f;
        if (!edit && W.mesh_inflight >= mesh_cap) break;
        mesh_submit(c, meshes[i].prio);
    }

    int columns = 0, pending = 0;
    for (u32 i = 0; i < W.columns.cap; i++) {
        Column *col = W.columns.vals[i] && W.columns.vals[i] != TOMB ? W.columns.vals[i] : NULL;
        if (!col) continue;
        columns++;
        if (col->state == COLUMN_PENDING) pending++;
    }
    W.stats.columns_loaded = columns;
    W.stats.columns_pending = pending;
    W.stats.chunks_loaded = (int)W.chunks.count;
    W.stats.chunks_meshed = chunks_meshed;
    W.stats.mesh_pending = W.mesh_inflight;
    W.stats.light_queue = light_queue_size();
    W.stats.columns_missing = missing;
    W.stats.chunks_unmeshed = unmeshed;
}

bool world_ready(void) {
    return W.active && W.stats.columns_missing == 0 && W.stats.columns_pending == 0 && W.stats.chunks_unmeshed == 0 &&
           W.stats.mesh_pending == 0 && W.stats.light_queue == 0;
}

void world_stats(WorldStats *out) { *out = W.stats; }

void world_flush_generation(int cx, int cz, int radius) {
    V3 focus = v3((float)cx * 32.0f + 16.0f, 0, (float)cz * 32.0f + 16.0f);
    V3 fwd = v3(0, 0, -1);
    while (true) {
        world_stream(focus, fwd, radius, true);
        jobs_pump(0.05);
        bool done = true;
        for (int dz = -radius; dz <= radius && done; dz++)
            for (int dx = -radius; dx <= radius && done; dx++) {
                Column *col = world_column(cx + dx, cz + dz);
                if (!col || col->state != COLUMN_READY) done = false;
            }
        if (done && light_queue_size() == 0) break;
        sleep_ms(1);
    }
}
