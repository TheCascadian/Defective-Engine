/* The server side of the game: request validation, scheduled block ticks, fluids and random ticks.
 *
 * Decisions:
 *  - The server and client share one process, but the client never edits the world directly. Every edit is a
 *    ServerMsg that is validated here (reach, block rules, the player's own box) and then applied through
 *    game_edit_block, so mods see one consistent stream of cancellable events.
 *  - Fluids are a "pull" simulation: when a cell is ticked it recomputes what it should hold from its
 *    neighbours. That needs no per-flow state and settles on its own, which a "push" simulation (each cell
 *    writing into its neighbours) does not guarantee when a source is removed.
 *  - Scheduled ticks live in a ring of buckets keyed by due tick, so a tick touches only what is due. */
#include "dfe.h"

#define MSG_QUEUE_CAPACITY 256
#define TICK_RING 64                 /* longest schedulable delay in ticks, longer delays are clamped */
#define MAX_DUE_PER_TICK 4096        /* fluid work per tick; the rest rolls to the next tick */
#define RANDOM_TICK_RADIUS 3         /* chunks around the player, horizontally */
#define RANDOM_TICKS_PER_CHUNK 3
#define BAND_PADDING_CHUNKS 1
#define PLACE_REACH_SLACK 1.0f

typedef struct SchedTick { int x, y, z; } SchedTick;
typedef VEC(SchedTick) TickBucket;

static struct {
    ServerMsg queue[MSG_QUEUE_CAPACITY];
    int head, count;
    TickBucket ring[TICK_RING];
    long tick;
    int scheduled;
    V3 focus;
    Rng rng;
} S;

void server_init(void) {
    server_shutdown();
    S.rng.s = hash64(world_seed() ^ 0x5eed5eedull);
}

void server_shutdown(void) {
    for (int i = 0; i < TICK_RING; i++) vec_free(S.ring[i]);
    memset(&S, 0, sizeof S);
}

void server_set_focus(V3 pos) { S.focus = pos; }
long server_ticks_run(void) { return S.tick; }
int server_scheduled_count(void) { return S.scheduled; }

/* ------------------------------------------------------------ messages */

void server_post(const ServerMsg *m) {
    if (S.count >= MSG_QUEUE_CAPACITY) { LOGW("Server message queue is full; dropping a request."); return; }
    S.queue[(S.head + S.count++) % MSG_QUEUE_CAPACITY] = *m;
}

static bool within_reach(int x, int y, int z) {
    V3 e = player_eye(&g_player);
    float dx = (float)x + 0.5f - e.x, dy = (float)y + 0.5f - e.y, dz = (float)z + 0.5f - e.z;
    float r = REACH_DISTANCE + PLACE_REACH_SLACK + 0.87f; /* half the diagonal of a block */
    return dx * dx + dy * dy + dz * dz <= r * r;
}

static bool cell_overlaps_player(int x, int y, int z) {
    const Player *p = &g_player;
    return (float)x < p->pos.x + p->half_width && (float)(x + 1) > p->pos.x - p->half_width && (float)y < p->pos.y + p->height &&
           (float)(y + 1) > p->pos.y && (float)z < p->pos.z + p->half_width && (float)(z + 1) > p->pos.z - p->half_width;
}

static bool apply_break(const ServerMsg *m) {
    u16 old = world_get_state(m->x, m->y, m->z);
    const BlockDef *b = old == STATE_UNLOADED ? NULL : block_of_state(old);
    if (!b || old == STATE_AIR || b->hardness < 0 || !within_reach(m->x, m->y, m->z)) return false;
    if (!game_edit_block(m->x, m->y, m->z, STATE_AIR)) return false;
    if (!g_creative) inventory_add(&g_inv, item_for_block(b), 1);
    return true;
}

static bool apply_place(const ServerMsg *m) {
    u16 old = world_get_state(m->x, m->y, m->z);
    ItemStack *held = &g_inv.slot[g_inv.selected];
    if (old == STATE_UNLOADED || !(g_state_flags[old] & BF_REPLACEABLE) || !within_reach(m->x, m->y, m->z)) return false;
    if (!held->count || held->state != m->state) return false;
    if (state_solid(m->state) && cell_overlaps_player(m->x, m->y, m->z)) return false;
    if (!game_edit_block(m->x, m->y, m->z, m->state)) return false;
    if (!g_creative) inventory_take_one(&g_inv, g_inv.selected);
    return true;
}

int server_pump(void) {
    int changed = 0;
    while (S.count > 0) {
        ServerMsg m = S.queue[S.head];
        S.head = (S.head + 1) % MSG_QUEUE_CAPACITY;
        S.count--;
        changed += m.type == MSG_BREAK ? apply_break(&m) : apply_place(&m);
    }
    return changed;
}

/* ------------------------------------------------------ scheduled ticks */

void server_schedule(int x, int y, int z, int delay) {
    delay = CLAMP(delay, 1, TICK_RING - 1);
    TickBucket *b = &S.ring[(S.tick + delay) % TICK_RING];
    for (int i = 0; i < b->n; i++)
        if (b->d[i].x == x && b->d[i].y == y && b->d[i].z == z) return;
    SchedTick t = {x, y, z};
    vec_push(*b, t);
    S.scheduled++;
}

/* ---------------------------------------------------------------- fluids */

static const BlockDef *fluid_of(u16 s) {
    if (s == STATE_UNLOADED || !(g_state_flags[s] & BF_FLUID)) return NULL;
    const BlockDef *b = block_of_state(s);
    return b && b->fluid_level_prop >= 0 ? b : NULL;
}

static int level_of(const BlockDef *b, u16 s) { return block_state_prop_index(b, s, b->fluid_level_prop); }
static bool is_falling(const BlockDef *b, int level) { return level == b->fluid_reach + 1; }

static u16 state_at_level(const BlockDef *b, int level) {
    char v[8];
    snprintf(v, sizeof v, "%d", level);
    return block_state_with(b, b->default_state, "level", v);
}

/* A cell below a fluid stops it from falling further, so it can only spread sideways. */
static bool blocks_fall(const BlockDef *f, int x, int y, int z) {
    u16 s = world_get_state(x, y, z);
    if (s == STATE_UNLOADED || state_solid(s)) return true;
    const BlockDef *b = fluid_of(s);
    return b && b->fluid_group == f->fluid_group && level_of(b, s) == 0;
}

typedef struct Feed { const BlockDef *def; int level; } Feed;

static bool neighbour_feed(const BlockDef *self, int x, int y, int z, int dir, Feed *best) {
    int nx = x + DIR_VEC[dir][0], nz = z + DIR_VEC[dir][2];
    u16 s = world_get_state(nx, y, nz);
    const BlockDef *f = fluid_of(s);
    if (!f || (self && f->fluid_group != self->fluid_group) || !blocks_fall(f, nx, y - 1, nz)) return false;
    int ln = level_of(f, s), give;
    if (ln == 0 || is_falling(f, ln)) give = 1;
    else if (ln < f->fluid_reach) give = ln + 1;
    else return false;
    if (best->def && best->level <= give) return false;
    best->def = f;
    best->level = give;
    return true;
}

static int source_neighbours(const BlockDef *f, int x, int y, int z) {
    int n = 0;
    for (int d = 0; d < 6; d++) {
        if (d == DIR_PY || d == DIR_NY) continue;
        u16 s = world_get_state(x + DIR_VEC[d][0], y, z + DIR_VEC[d][2]);
        const BlockDef *b = fluid_of(s);
        if (b && b->fluid_group == f->fluid_group && level_of(b, s) == 0) n++;
    }
    return n;
}

/* What a cell should hold, given its neighbours. `self` restricts the answer to one fluid group. */
static Feed desired_feed(const BlockDef *self, int x, int y, int z) {
    Feed best = {NULL, 0};
    u16 above = world_get_state(x, y + 1, z);
    const BlockDef *fa = fluid_of(above);
    if (fa && (!self || fa->fluid_group == self->fluid_group)) {
        best.def = fa;
        best.level = fa->fluid_reach + 1;
        return best;
    }
    for (int d = 0; d < 6; d++)
        if (d != DIR_PY && d != DIR_NY) neighbour_feed(self, x, y, z, d, &best);
    if (best.def && best.def->fluid_infinite && source_neighbours(best.def, x, y, z) >= 2 && blocks_fall(best.def, x, y - 1, z)) best.level = 0;
    return best;
}

static void tick_fluid_cell(int x, int y, int z) {
    u16 s = world_get_state(x, y, z);
    if (s == STATE_UNLOADED) return;
    const BlockDef *self = fluid_of(s);
    if (self) {
        int level = level_of(self, s);
        if (level == 0) return; /* sources are permanent */
        Feed f = desired_feed(self, x, y, z);
        if (f.def == self) {
            if (f.level != level) world_set_state(x, y, z, state_at_level(self, f.level));
        } else if (level + 1 > self->fluid_reach) {
            world_set_state(x, y, z, STATE_AIR); /* nothing feeds it any more: it ebbs one level per step */
        } else {
            world_set_state(x, y, z, state_at_level(self, level + 1));
        }
        return;
    }
    if (!(g_state_flags[s] & BF_REPLACEABLE)) return;
    Feed f = desired_feed(NULL, x, y, z);
    if (f.def) world_set_state(x, y, z, state_at_level(f.def, f.level));
}

/* ------------------------------------------------------- block changes */

void server_block_changed(int x, int y, int z, u16 old_state, u16 new_state) {
    const BlockDef *involved = fluid_of(old_state) ? fluid_of(old_state) : fluid_of(new_state);
    for (int d = 0; d < 6 && !involved; d++) involved = fluid_of(world_get_state(x + DIR_VEC[d][0], y + DIR_VEC[d][1], z + DIR_VEC[d][2]));
    if (!involved) return;
    int delay = involved->fluid_viscosity;
    server_schedule(x, y, z, delay);
    for (int d = 0; d < 6; d++) server_schedule(x + DIR_VEC[d][0], y + DIR_VEC[d][1], z + DIR_VEC[d][2], delay);
}

/* --------------------------------------------------------- random ticks */

static void random_tick_chunk(const Chunk *c) {
    if (!c || (c->flags & (CF_VIRTUAL | CF_HAS_RANDOM_TICK)) != CF_HAS_RANDOM_TICK) return;
    for (int i = 0; i < RANDOM_TICKS_PER_CHUNK; i++) {
        u64 r = rng_next(&S.rng);
        int x = (int)(r & 31), z = (int)((r >> 5) & 31), y = (int)((r >> 10) & 31);
        u16 st = chunk_get(c, (y << 10) | (z << 5) | x);
        if (!(g_state_flags[st] & BF_RANDOM_TICK)) continue;
        dfe_event_t ev = {.name = "random_tick", .x = c->cx * CHUNK_SIZE + x, .y = c->cy * CHUNK_SIZE + y, .z = c->cz * CHUNK_SIZE + z, .state = st};
        event_fire(&ev);
    }
}

static void run_random_ticks(void) {
    int cx = ifloor(S.focus.x) >> CHUNK_SHIFT, cz = ifloor(S.focus.z) >> CHUNK_SHIFT, lo, hi;
    gen_band(&lo, &hi);
    for (int dz = -RANDOM_TICK_RADIUS; dz <= RANDOM_TICK_RADIUS; dz++)
        for (int dx = -RANDOM_TICK_RADIUS; dx <= RANDOM_TICK_RADIUS; dx++)
            for (int cy = lo - BAND_PADDING_CHUNKS; cy <= hi + BAND_PADDING_CHUNKS; cy++)
                random_tick_chunk(world_chunk(cx + dx, cy, cz + dz));
}

/* ----------------------------------------------------------------- tick */

static void run_due_ticks(void) {
    TickBucket *b = &S.ring[S.tick % TICK_RING];
    int run = MIN(b->n, MAX_DUE_PER_TICK);
    SchedTick *batch = run ? xmalloc((size_t)run * sizeof *batch) : NULL;
    if (run) memcpy(batch, b->d, (size_t)run * sizeof *batch);
    int left = b->n - run;
    if (left) memmove(b->d, b->d + run, (size_t)left * sizeof *batch);
    b->n = left;
    S.scheduled -= run;
    for (int i = 0; i < run; i++) tick_fluid_cell(batch[i].x, batch[i].y, batch[i].z);
    free(batch);
    if (left) { /* work beyond the budget is picked up on the next tick */
        TickBucket *next = &S.ring[(S.tick + 1) % TICK_RING];
        for (int i = 0; i < b->n; i++) vec_push(*next, b->d[i]);
        b->n = 0;
    }
}

void server_tick(void) {
    run_due_ticks();
    run_random_ticks();
    S.tick++;
}
