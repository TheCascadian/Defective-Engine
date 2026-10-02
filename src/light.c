#include "dfe.h"

/*
 * Light is four 4-bit channels (sky, R, G, B) packed in a u16. One breadth-first pass handles all channels
 * at once: a candidate value is computed per channel and the voxel is rewritten when any channel improves.
 *
 * Decisions:
 *  - Sky light travelling straight down at level 15 does not decay, so a shaft of open sky stays bright to
 *    the bottom. Every other step costs one level plus the opacity of the voxel entered.
 *  - Removal uses the classic two-queue scheme, run per channel through a mask carried in the queue entry,
 *    so deleting a torch does not disturb unrelated sky light in the same voxel.
 *  - Generation workers only light inside their own column (no shared state). The main thread stitches
 *    columns together afterwards. A global recompute per edit was rejected as too slow for large caves.
 */

static inline u16 propagate(u16 src, bool down, int op) {
    int s = LIGHT_SKY(src);
    int ns = (down && s == 15) ? 15 - op : s - 1 - op;
    int nr = LIGHT_R(src) - 1 - op, ng = LIGHT_G(src) - 1 - op, nb = LIGHT_B(src) - 1 - op;
    if (ns < 0) ns = 0;
    if (nr < 0) nr = 0;
    if (ng < 0) ng = 0;
    if (nb < 0) nb = 0;
    return LIGHT_PACK(ns, nr, ng, nb);
}

static inline u16 light_max(u16 a, u16 b) {
    int s = MAX(LIGHT_SKY(a), LIGHT_SKY(b)), r = MAX(LIGHT_R(a), LIGHT_R(b));
    int g = MAX(LIGHT_G(a), LIGHT_G(b)), bl = MAX(LIGHT_B(a), LIGHT_B(b));
    return LIGHT_PACK(s, r, g, bl);
}

/* ------------------------------------------------------------ column local */

typedef struct IntQueue {
    u32 *v;
    size_t head, tail, cap;
} IntQueue;

static void iq_push(IntQueue *q, u32 x) {
    if (q->tail == q->cap) {
        if (q->head > q->cap / 2) {
            memmove(q->v, q->v + q->head, (q->tail - q->head) * sizeof(u32));
            q->tail -= q->head;
            q->head = 0;
        } else {
            q->cap = q->cap ? q->cap * 2 : 4096;
            q->v = xrealloc(q->v, q->cap * sizeof(u32));
        }
    }
    q->v[q->tail++] = x;
}

static const int NEIGH[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};

static inline int op_of(u16 state) { return g_state_opacity[state]; }

/* Only a voxel that could still improve a neighbour needs to enter the queue. */
static bool column_can_improve(const u16 *states, const u16 *light, int x, int y, int z, int h) {
    u16 here = light[(y << 10) | (z << 5) | x];
    if (!here) return false;
    for (int d = 0; d < 6; d++) {
        int nx = x + NEIGH[d][0], ny = y + NEIGH[d][1], nz = z + NEIGH[d][2];
        if ((unsigned)nx > 31 || (unsigned)nz > 31 || (unsigned)ny >= (unsigned)h) continue;
        int ni = (ny << 10) | (nz << 5) | nx;
        u16 cand = propagate(here, d == DIR_NY, op_of(states[ni]));
        if (light_max(light[ni], cand) != light[ni]) return true;
    }
    return false;
}

void light_init_column(const u16 *states, int chunk_layers, u16 *light_out) {
    int h = chunk_layers * CHUNK_SIZE;
    for (int z = 0; z < 32; z++)
        for (int x = 0; x < 32; x++) {
            int sky = 15;
            for (int y = h - 1; y >= 0; y--) {
                int i = (y << 10) | (z << 5) | x;
                u16 s = states[i];
                int op = op_of(s);
                /* Same rule as propagate(): full sky passes straight down, anything dimmer fades by one per voxel. */
                sky = op >= 15 ? 0 : (sky == 15 ? 15 - op : MAX(sky - 1 - op, 0));
                light_out[i] = LIGHT_PACK(sky, 0, 0, 0) | g_state_emit[s];
            }
        }
    IntQueue q = {0};
    for (int y = 0; y < h; y++)
        for (int z = 0; z < 32; z++)
            for (int x = 0; x < 32; x++)
                if (column_can_improve(states, light_out, x, y, z, h)) iq_push(&q, (y << 10) | (z << 5) | x);
    while (q.head < q.tail) {
        u32 i = q.v[q.head++];
        int x = i & 31, z = (i >> 5) & 31, y = (int)(i >> 10);
        u16 here = light_out[i];
        for (int d = 0; d < 6; d++) {
            int nx = x + NEIGH[d][0], ny = y + NEIGH[d][1], nz = z + NEIGH[d][2];
            if ((unsigned)nx > 31 || (unsigned)nz > 31 || (unsigned)ny >= (unsigned)h) continue;
            int ni = (ny << 10) | (nz << 5) | nx;
            u16 cand = propagate(here, d == DIR_NY, op_of(states[ni]));
            u16 merged = light_max(light_out[ni], cand);
            if (merged != light_out[ni]) {
                light_out[ni] = merged;
                iq_push(&q, (u32)ni);
            }
        }
    }
    free(q.v);
}

/* ------------------------------------------------------------- main thread */

typedef struct Node { i32 x, y, z; u16 val; } Node;
static struct {
    VEC(Node) add, rem;
    size_t add_head, rem_head;
} L;

/* Drops consumed entries once they dominate the buffer, keeping appends amortised O(1). */
static void queue_compact(void) {
    if (L.add_head == (size_t)L.add.n) { L.add.n = 0; L.add_head = 0; }
    else if (L.add_head > 65536 && L.add_head * 2 > (size_t)L.add.n) {
        memmove(L.add.d, L.add.d + L.add_head, ((size_t)L.add.n - L.add_head) * sizeof(Node));
        L.add.n -= (int)L.add_head; L.add_head = 0;
    }
    if (L.rem_head == (size_t)L.rem.n) { L.rem.n = 0; L.rem_head = 0; }
}

static void push_add(int x, int y, int z) {
    Node n = {x, y, z, 0};
    vec_push(L.add, n);
}

static void push_rem(int x, int y, int z, u16 val) {
    Node n = {x, y, z, val};
    vec_push(L.rem, n);
}

int light_queue_size(void) { return (int)(((size_t)L.add.n - L.add_head) + ((size_t)L.rem.n - L.rem_head)); }

/* Returns the chunk and index holding a voxel, or NULL when it is virtual or unloaded (never written). */
static Chunk *cell(int x, int y, int z, int *idx) {
    Chunk *c = world_chunk(x >> CHUNK_SHIFT, y >> CHUNK_SHIFT, z >> CHUNK_SHIFT);
    if (c) *idx = ((y & 31) << 10) | ((z & 31) << 5) | (x & 31);
    return c;
}

static inline int ch_get(u16 l, int ch) { return (l >> (12 - 4 * ch)) & 15; }
static inline u16 ch_mask(int ch) { return (u16)(15u << (12 - 4 * ch)); }

static void step_add(Node n) {
    u16 here = world_get_light(n.x, n.y, n.z);
    if (!here) return;
    for (int d = 0; d < 6; d++) {
        int x = n.x + NEIGH[d][0], y = n.y + NEIGH[d][1], z = n.z + NEIGH[d][2];
        int idx;
        Chunk *c = cell(x, y, z, &idx);
        if (!c) continue;
        u16 cur = chunk_get_light(c, idx);
        u16 cand = propagate(here, d == DIR_NY, op_of(chunk_get(c, idx)));
        u16 merged = light_max(cur, cand);
        if (merged == cur) continue;
        chunk_set_light(c, idx, merged);
        c->flags |= CF_SAVE_DIRTY;
        world_light_touched(x, y, z);
        push_add(x, y, z);
    }
}

static void step_remove(Node n) {
    for (int d = 0; d < 6; d++) {
        int x = n.x + NEIGH[d][0], y = n.y + NEIGH[d][1], z = n.z + NEIGH[d][2];
        int idx;
        Chunk *c = cell(x, y, z, &idx);
        if (!c) continue;
        u16 cur = chunk_get_light(c, idx);
        u16 clear_mask = 0, removed = 0;
        bool source = false;
        for (int ch = 0; ch < 4; ch++) {
            int vc = ch_get(n.val, ch), nc = ch_get(cur, ch);
            if (!vc || !nc) continue;
            bool down_sky = ch == 0 && d == DIR_NY && vc == 15;
            if (down_sky ? nc <= 15 : nc < vc) {
                clear_mask |= ch_mask(ch);
                removed |= (u16)(cur & ch_mask(ch));
            } else source = true;
        }
        if (clear_mask) {
            chunk_set_light(c, idx, cur & ~clear_mask);
            c->flags |= CF_SAVE_DIRTY;
            world_light_touched(x, y, z);
            push_rem(x, y, z, removed);
        }
        if (source) push_add(x, y, z);
    }
}

void light_on_block_changed(int x, int y, int z, u16 old_state, u16 new_state) {
    int idx;
    Chunk *c = cell(x, y, z, &idx);
    if (!c) return;
    u16 old_light = chunk_get_light(c, idx);
    u16 fresh = g_state_emit[new_state];
    /* An opaque cell cannot hold sky light; a transparent one is refilled by its neighbours below. */
    chunk_set_light(c, idx, fresh);
    world_light_touched(x, y, z);
    if (old_light) push_rem(x, y, z, old_light);
    for (int d = 0; d < 6; d++) push_add(x + NEIGH[d][0], y + NEIGH[d][1], z + NEIGH[d][2]);
    if (fresh) push_add(x, y, z);
}

static void seed_pair(int ax, int ay, int az, int bx, int by, int bz, bool a_is_above_b) {
    u16 la = world_get_light(ax, ay, az), lb = world_get_light(bx, by, bz);
    if (!la && !lb) return;
    u16 sa = world_get_state(ax, ay, az), sb = world_get_state(bx, by, bz);
    if (sa == STATE_UNLOADED || sb == STATE_UNLOADED) return;
    bool vertical = ay != by;
    u16 into_b = propagate(la, vertical && a_is_above_b, op_of(sb));
    u16 into_a = propagate(lb, vertical && !a_is_above_b, op_of(sa));
    if (light_max(lb, into_b) != lb) push_add(ax, ay, az);
    if (light_max(la, into_a) != la) push_add(bx, by, bz);
}

void light_seed_column_borders(int cx, int cz) {
    Column *col = world_column(cx, cz);
    if (!col) return;
    int y0 = col->lo_cy * CHUNK_SIZE, y1 = (col->hi_cy + 1) * CHUNK_SIZE;
    for (int d = 0; d < 4; d++) {
        int dx = DIR_VEC[d < 2 ? d : d + 2][0], dz = DIR_VEC[d < 2 ? d : d + 2][2];
        Column *n = world_column(cx + dx, cz + dz);
        if (!n || n->state != COLUMN_READY) continue;
        for (int y = y0; y < y1; y++)
            for (int t = 0; t < 32; t++) {
                int ax, az;
                if (dx) { ax = cx * 32 + (dx > 0 ? 31 : 0); az = cz * 32 + t; }
                else { ax = cx * 32 + t; az = cz * 32 + (dz > 0 ? 31 : 0); }
                seed_pair(ax, y, az, ax + dx, y, az + dz, false);
            }
    }
}

int light_process(int max_steps) {
    int steps = 0;
    while (steps < max_steps) {
        if (L.rem_head < (size_t)L.rem.n) step_remove(L.rem.d[L.rem_head++]);
        else if (L.add_head < (size_t)L.add.n) step_add(L.add.d[L.add_head++]);
        else break;
        steps++;
    }
    queue_compact();
    return light_queue_size();
}
