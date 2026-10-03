/* Distant terrain as real voxels. Beyond the chunk render distance the world is meshed again from the generator
 * at coarser scales: level k uses voxels 2^k blocks wide, so level 1 is 2 blocks, level 2 is 4 and so on, with
 * the same block textures, ambient occlusion and water as the near chunks. Each level covers a ring that
 * doubles in radius, which keeps the number of tiles, and so the cost, roughly constant per level.
 *
 * A tile is one 32 x 32 column of voxels (so 64 blocks wide at level 1) and is built in one worker job: the
 * generator fills padded voxel cubes for every vertical chunk that holds surface or water, and the normal
 * mesher turns them into the same packed vertices the near chunks use. Meshes live in the shared vertex
 * arena and the origin buffer carries the voxel scale, so a tile costs no extra draw calls beyond its share
 * of the per-page multi-draw.
 *
 * Layers must not draw over finer ones. A small coverage texture holds, per 32-block column, the finest level
 * that is ready there (0 for real chunks, 255 for none) and a fragment of level k is discarded when a finer
 * level exists. The texture is eroded by one column so each finer layer overlaps the coarser one underneath;
 * together with heights that are always rounded down (see gen.c) the overlap hides cracks without z-fighting.
 *
 * Rejected: a smooth heightmap mesh (read as a different game, even when faceted), an octree of stored
 * voxels as in Voxy (the terrain is analytic, so regenerating a coarse tile is cheaper than storing it and
 * needs no save data), and per-level cover textures (one texture with a level number does the same job). */
#include "dfe.h"

#define LOD_MAX_LEVELS 4
#define LOD_MAX_TILES 1400
#define LOD_JOBS_IN_FLIGHT 3
#define LOD_RESCAN_FRAMES 20
#define LOD_INNER_OVERLAP 1.5f  /* tiles start this many tile widths inside the finer ring so they underlie its edge */
#define LOD_OUTER_OVERLAP 1.5f

typedef struct LodChunk {
    int cy;
    MeshSlot slot[LAYER_COUNT];
} LodChunk;

typedef struct LodTile {
    int level, tx, tz;
    u32 serial;
    bool ready;
    int nchunks;
    LodChunk *chunks;
    float ymin, ymax;
} LodTile;

typedef struct LodJob {
    int level, tx, tz;
    u32 serial;
    int n;
    int *cy;
    MeshOutput *out;
} LodJob;

static struct {
    bool ready;
    LodTile *tiles;
    int tile_count;
    int jobs_inflight;
    u32 serial;
    int levels;
    float ring[LOD_MAX_LEVELS + 1]; /* ring[k] is the outer radius of level k in blocks, ring[0] is the render distance */
    int rd, center_cx, center_cz, frames_since_scan;
    float end_radius;
    /* Draw lists, rebuilt each frame, nearest tile first. */
    const MeshSlot **draw[LAYER_COUNT];
    int draw_n[LAYER_COUNT], draw_cap[LAYER_COUNT];
    LodTile **sorted;
    float *sorted_d2;
    int sorted_cap;
    /* Coverage. */
    GLuint cover_tex;
    u8 *cover, *cover_tmp;
    int cover_dim, cover_cap, cover_gl_dim, cover_origin_x, cover_origin_z;
    int tiles_drawn;
} L;

static inline int tile_blocks(int level) { return CHUNK_SIZE << level; }

bool lod_init(void) {
    memset(&L, 0, sizeof L);
    L.tiles = xcalloc(LOD_MAX_TILES, sizeof(LodTile));
    glGenTextures(1, &L.cover_tex);
    L.ready = true;
    return true;
}

static void tile_release(LodTile *t) {
    for (int i = 0; i < t->nchunks; i++) scene_release_slots(t->chunks[i].slot);
    free(t->chunks);
    t->chunks = NULL;
    t->nchunks = 0;
}

void lod_shutdown(void) {
    if (!L.ready) return;
    for (int i = 0; i < L.tile_count; i++) tile_release(&L.tiles[i]);
    free(L.tiles);
    for (int l = 0; l < LAYER_COUNT; l++) free(L.draw[l]);
    free(L.sorted); free(L.sorted_d2); free(L.cover); free(L.cover_tmp);
    glDeleteTextures(1, &L.cover_tex);
    memset(&L, 0, sizeof L);
}

/* ------------------------------------------------------------------ build */

static void lod_job_run(void *data, int worker) {
    LodJob *j = data;
    GenLodGrid *g = xmalloc(sizeof *g);
    gen_lod_grid(j->level, j->tx, j->tz, g);
    int lo = floor_div(g->vmin, CHUNK_SIZE), hi = floor_div(g->vmax, CHUNK_SIZE);
    j->n = hi - lo + 1;
    j->cy = xmalloc((size_t)j->n * sizeof(int));
    j->out = xcalloc((size_t)j->n, sizeof(MeshOutput));
    u16 *states = xmalloc(MESH_PAD_VOL * sizeof(u16)), *light = xmalloc(MESH_PAD_VOL * sizeof(u16));
    for (int k = 0; k < j->n; k++) {
        j->cy[k] = lo + k;
        gen_lod_fill(j->level, lo + k, g, states);
        gen_lod_light(j->level, lo + k, g, light); /* no caves or roofs: open sky, dimmed only under the sea */
        MeshInput in = {.cx = j->tx, .cy = lo + k, .cz = j->tz, .states = states, .light = light, .scale_shift = j->level};
        mesh_build(&in, &j->out[k]);
    }
    free(states); free(light); free(g);
}

static void lod_job_complete(void *data) {
    LodJob *j = data;
    L.jobs_inflight--;
    LodTile *t = NULL;
    for (int i = 0; i < L.tile_count; i++) if (L.tiles[i].serial == j->serial) t = &L.tiles[i];
    if (t && L.ready) {
        int span = tile_blocks(t->level);
        t->chunks = xcalloc((size_t)j->n, sizeof(LodChunk));
        t->nchunks = j->n;
        for (int k = 0; k < j->n; k++) {
            LodChunk *c = &t->chunks[k];
            c->cy = j->cy[k];
            for (int l = 0; l < LAYER_COUNT; l++) c->slot[l].page = -1;
            i32 origin[4] = {t->tx * span, c->cy * span, t->tz * span, t->level};
            scene_upload_slots(c->slot, &j->out[k], origin);
        }
        t->ymin = (float)(j->cy[0] * span);
        t->ymax = (float)((j->cy[j->n - 1] + 1) * span);
        t->ready = true;
    }
    for (int k = 0; k < j->n; k++) mesh_output_free(&j->out[k]);
    free(j->out); free(j->cy); free(j);
}

/* -------------------------------------------------------------- scheduling */

static void configure_levels(int rd, int far_chunks) {
    L.rd = rd;
    L.end_radius = (float)(rd + far_chunks) * CHUNK_SIZE;
    L.ring[0] = (float)rd * CHUNK_SIZE;
    int n = 0;
    float r = L.ring[0];
    while (r < L.end_radius && n < LOD_MAX_LEVELS) {
        r = MIN(r * 2.0f, L.end_radius);
        L.ring[++n] = r;
    }
    L.levels = n;
}

/* The band of centre distances a level builds tiles for. */
static void level_band(int level, float *lo, float *hi) {
    float t = (float)tile_blocks(level);
    *lo = L.ring[level - 1] - LOD_INNER_OVERLAP * t;
    *hi = level == L.levels ? L.end_radius + 0.8f * t : L.ring[level] + LOD_OUTER_OVERLAP * t;
}

static float tile_dist2(const LodTile *t, V3 p) {
    float span = (float)tile_blocks(t->level);
    float cx = ((float)t->tx + 0.5f) * span - p.x, cz = ((float)t->tz + 0.5f) * span - p.z;
    return cx * cx + cz * cz;
}

static LodTile *find_tile(int level, int tx, int tz) {
    for (int i = 0; i < L.tile_count; i++)
        if (L.tiles[i].level == level && L.tiles[i].tx == tx && L.tiles[i].tz == tz) return &L.tiles[i];
    return NULL;
}

static void remove_stale(V3 pos) {
    for (int i = L.tile_count - 1; i >= 0; i--) {
        LodTile *t = &L.tiles[i];
        if (t->level > L.levels) { tile_release(t); L.tiles[i] = L.tiles[--L.tile_count]; continue; }
        float lo, hi, t_w = (float)tile_blocks(t->level);
        level_band(t->level, &lo, &hi);
        float d = sqrtf(tile_dist2(t, pos));
        if (d < lo - t_w || d > hi + t_w) {
            /* A job still in flight finds no tile with its serial and discards its result. */
            tile_release(t);
            L.tiles[i] = L.tiles[--L.tile_count];
        }
    }
}

static void schedule(const Camera *cam) {
    int ccx = floor_div(ifloor(cam->pos.x), CHUNK_SIZE), ccz = floor_div(ifloor(cam->pos.z), CHUNK_SIZE);
    L.frames_since_scan++;
    if (ccx == L.center_cx && ccz == L.center_cz && L.frames_since_scan < LOD_RESCAN_FRAMES) return;
    L.center_cx = ccx; L.center_cz = ccz; L.frames_since_scan = 0;
    remove_stale(cam->pos);
    while (L.jobs_inflight < LOD_JOBS_IN_FLIGHT && L.tile_count < LOD_MAX_TILES) {
        float best = 1e30f;
        int bl = 0, bx = 0, bz = 0;
        for (int level = 1; level <= L.levels; level++) {
            float lo, hi, span = (float)tile_blocks(level);
            level_band(level, &lo, &hi);
            int r = (int)(hi / span) + 2;
            int ctx = floor_div(ifloor(cam->pos.x), (int)span), ctz = floor_div(ifloor(cam->pos.z), (int)span);
            for (int dz = -r; dz <= r; dz++)
                for (int dx = -r; dx <= r; dx++) {
                    LodTile probe = {.level = level, .tx = ctx + dx, .tz = ctz + dz};
                    float d2 = tile_dist2(&probe, cam->pos);
                    if (d2 < lo * lo && lo > 0.0f) continue;
                    if (d2 > hi * hi || d2 >= best || find_tile(level, probe.tx, probe.tz)) continue;
                    best = d2; bl = level; bx = probe.tx; bz = probe.tz;
                }
        }
        if (best >= 1e30f) break;
        LodTile *t = &L.tiles[L.tile_count++];
        memset(t, 0, sizeof *t);
        t->level = bl; t->tx = bx; t->tz = bz; t->serial = ++L.serial;
        LodJob *j = xcalloc(1, sizeof *j);
        j->level = bl; j->tx = bx; j->tz = bz; j->serial = t->serial;
        L.jobs_inflight++;
        jobs_submit(JOB_KIND_FAR, best, lod_job_run, lod_job_complete, j);
    }
}

/* ---------------------------------------------------------------- coverage */

static void build_coverage(const Camera *cam) {
    int fcx = floor_div(ifloor(cam->pos.x), CHUNK_SIZE), fcz = floor_div(ifloor(cam->pos.z), CHUNK_SIZE);
    int half = (int)(L.end_radius / CHUNK_SIZE) + 3, dim = 2 * half + 1;
    int ox = fcx - half, oz = fcz - half;
    if (dim * dim > L.cover_cap) {
        L.cover_cap = dim * dim;
        L.cover = xrealloc(L.cover, (size_t)L.cover_cap);
        L.cover_tmp = xrealloc(L.cover_tmp, (size_t)L.cover_cap);
    }
    memset(L.cover_tmp, 255, (size_t)dim * dim);
    for (int i = 0; i < L.tile_count; i++) {
        const LodTile *t = &L.tiles[i];
        if (!t->ready) continue;
        int cols = 1 << t->level;
        for (int z = 0; z < cols; z++)
            for (int x = 0; x < cols; x++) {
                int cx = t->tx * cols + x - ox, cz = t->tz * cols + z - oz;
                if (cx < 0 || cz < 0 || cx >= dim || cz >= dim) continue;
                u8 *c = &L.cover_tmp[cz * dim + cx];
                if (t->level < *c) *c = (u8)t->level;
            }
    }
    for (int z = 0; z < dim; z++)
        for (int x = 0; x < dim; x++) {
            int dx = ox + x - fcx, dz = oz + z - fcz;
            if (dx * dx + dz * dz <= L.rd * L.rd && world_column_meshed(ox + x, oz + z)) L.cover_tmp[z * dim + x] = 0;
        }
    /* Erode: a column next to a coarser or empty one shows that coarser level too, so the two overlap. */
    static const int D4[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (int z = 0; z < dim; z++)
        for (int x = 0; x < dim; x++) {
            u8 v = L.cover_tmp[z * dim + x], worst = v;
            if (v != 255) {
                for (int k = 0; k < 4; k++) {
                    int nx = x + D4[k][0], nz = z + D4[k][1];
                    if (nx < 0 || nz < 0 || nx >= dim || nz >= dim) continue;
                    u8 nv = L.cover_tmp[nz * dim + nx];
                    if (nv > worst) worst = nv;
                }
            }
            L.cover[z * dim + x] = worst;
        }
    L.cover_dim = dim; L.cover_origin_x = ox; L.cover_origin_z = oz;
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, L.cover_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (dim != L.cover_gl_dim) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, dim, dim, 0, GL_RED, GL_UNSIGNED_BYTE, L.cover);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        L.cover_gl_dim = dim;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, dim, dim, GL_RED, GL_UNSIGNED_BYTE, L.cover);
    }
    glActiveTexture(GL_TEXTURE0);
}

/* ------------------------------------------------------------------ frame */

static void push_slot(int layer, const MeshSlot *s) {
    if (s->page < 0 || s->count == 0) return;
    if (L.draw_n[layer] == L.draw_cap[layer]) {
        L.draw_cap[layer] = L.draw_cap[layer] ? L.draw_cap[layer] * 2 : 256;
        L.draw[layer] = xrealloc(L.draw[layer], (size_t)L.draw_cap[layer] * sizeof(*L.draw[layer]));
    }
    L.draw[layer][L.draw_n[layer]++] = s;
}

static int by_distance(const void *a, const void *b) {
    float da = L.sorted_d2[*(const int *)a], db = L.sorted_d2[*(const int *)b];
    return da < db ? -1 : da > db;
}

void lod_update(const Camera *cam, int rd, int far_chunks) {
    if (!L.ready) return;
    configure_levels(rd, far_chunks);
    for (int l = 0; l < LAYER_COUNT; l++) L.draw_n[l] = 0;
    L.tiles_drawn = 0;
    if (far_chunks <= 0 || L.levels == 0) {
        for (int i = L.tile_count - 1; i >= 0; i--) { tile_release(&L.tiles[i]); }
        L.tile_count = 0;
        return;
    }
    schedule(cam);
    build_coverage(cam);
    if (L.tile_count > L.sorted_cap) {
        L.sorted_cap = L.tile_count * 2;
        L.sorted = xrealloc(L.sorted, (size_t)L.sorted_cap * sizeof(*L.sorted));
        L.sorted_d2 = xrealloc(L.sorted_d2, (size_t)L.sorted_cap * sizeof(*L.sorted_d2));
    }
    int *order = xmalloc((size_t)MAX(L.tile_count, 1) * sizeof(int));
    int n = 0;
    for (int i = 0; i < L.tile_count; i++) {
        const LodTile *t = &L.tiles[i];
        if (!t->ready) continue;
        float span = (float)tile_blocks(t->level);
        V3 lo = v3((float)t->tx * span, t->ymin, (float)t->tz * span), hi = v3(lo.x + span, t->ymax, lo.z + span);
        if (!frustum_box_visible(&cam->frustum, lo, hi)) continue;
        L.sorted_d2[i] = tile_dist2(t, cam->pos);
        order[n++] = i;
    }
    qsort(order, (size_t)n, sizeof(int), by_distance);
    for (int k = 0; k < n; k++) {
        const LodTile *t = &L.tiles[order[k]];
        for (int c = 0; c < t->nchunks; c++)
            for (int l = 0; l < LAYER_COUNT; l++) push_slot(l, &t->chunks[c].slot[l]);
    }
    L.tiles_drawn = n;
    free(order);
    g_scene_stats.lod_tiles_drawn = n;
    g_scene_stats.lod_tiles_total = L.tile_count;
}

const MeshSlot *const *lod_draw_list(int layer, int *count) {
    *count = L.draw_n[layer];
    return (const MeshSlot *const *)L.draw[layer];
}

void lod_set_uniforms(Shader *sh) {
    glUniform1i(shader_uniform(sh, "u_cover"), 3);
    glUniform1i(shader_uniform(sh, "u_cover_dim"), L.cover_dim);
    glUniform2i(shader_uniform(sh, "u_cover_origin"), L.cover_origin_x, L.cover_origin_z);
    glUniform1f(shader_uniform(sh, "u_sea"), (float)gen_sea_level());
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, L.cover_tex);
    glActiveTexture(GL_TEXTURE0);
}
