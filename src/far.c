/* Far terrain: a coarse heightmap mesh that continues the world past the chunk render distance.
 *
 * The surface comes straight from the generator's analytic height and biome functions, so no chunk data is
 * needed and a tile costs a few milliseconds on a worker. Tiles are 256 blocks square and are built from 8 by 8
 * block columns: each is a flat-topped box face at a whole-block height, with a vertical wall wherever a
 * neighbour is lower, and one flat colour and one face shade per quad. That keeps the far layer in the same
 * blocky idiom as the chunks. Rejected: a smooth interpolated heightmap (it read as a different, low-poly
 * game beyond the chunk ring) and faceted normals on a smooth mesh (still rounded silhouettes).
 * Vertices are 8 bytes (grid x and z, height, colour, face and water flags).
 *
 * Where real chunks exist the far layer must not draw. A small coverage texture marks columns that are
 * fully meshed (eroded by one column so the two layers overlap by a ring), and the fragment shader discards
 * covered fragments. Rejected: a depth offset alone (water planes would paint over the seabed) and a fixed
 * circular cutoff (leaves holes while chunks are still streaming in).
 *
 * Ocean needs special care. A flat water plane that starts where the chunk ring ends leaves a gap: looking
 * across the ring edge one sees through the cut end of the water volume, between the sea bed and the plane,
 * into the sky. Ocean vertices therefore carry the real sea bed height, and the vertex shader raises them to
 * the surface over WATER_RAMP_BLOCKS beyond the ring, so the surface is continuous from the near sea bed out to
 * a correct horizon line. Rejected: skirts (the ring edge is inside a tile, not on its border) and drawing the
 * plane over the ring (it would paint over the near sea bed).
 *
 * Rejected: several LODs per tile. One resolution keeps the code and the draw count small and the far layer
 * is always fogged by the time its triangles get large on screen. */
#include "dfe.h"

#define TILE_BLOCKS 256
#define TILE_CELLS 32
#define CELL_BLOCKS (TILE_BLOCKS / TILE_CELLS)
#define HEIGHT_UNITS_PER_BLOCK 4.0f
#define MAX_TILES 400
#define MAX_TILE_JOBS 2
#define RESCAN_FRAMES 20
#define FAR_DROP_BLOCKS 3.0f
#define FAR_WATER_DROP_BLOCKS 0.6f
#define WATER_RAMP_BLOCKS 192.0f /* distance over which the sea bed rises to the surface beyond the chunk ring */
#define OCEAN_STEP_BLOCKS 4.0f
#define COLOR_JITTER 0.05f
#define DEEP_WATER_DEPTH 40.0f
static const float SHALLOW_WATER[3] = {0.16f, 0.38f, 0.58f};
static const float DEEP_WATER[3] = {0.07f, 0.20f, 0.40f};

typedef struct FarVertex {
    u8 x, z;      /* corner of the 8-block grid inside the tile, 0..TILE_CELLS */
    i16 height;   /* blocks * HEIGHT_UNITS_PER_BLOCK */
    u8 r, g, b;
    u8 flags;     /* bits 0..2 face (DIR_*), bit 3 ocean: height is then the sea bed and the shader lifts it towards the surface */
} FarVertex;
#define FLAG_WATER 8
#define MAX_TILE_QUADS (TILE_CELLS * TILE_CELLS * 5) /* a top and up to four walls per column */

typedef struct FarTile {
    int tx, tz;
    u32 serial;
    bool ready;
    GLuint vbo, vao;
    int index_count;
    float ymin, ymax;
} FarTile;

typedef struct FarJob {
    int tx, tz;
    u32 serial;
    FarVertex *verts;
    int vert_count;
    float ymin, ymax;
} FarJob;

static struct {
    bool ready;
    Shader shader;
    GLuint ibo, cover_tex;
    FarTile tiles[MAX_TILES];
    int tile_count;
    int jobs_inflight;
    u32 serial;
    int center_tx, center_tz, range_blocks, frames_since_scan;
    u8 *cover;
    int cover_dim, cover_cap, cover_gl_dim;
} F;

static bool load_far_shader(Shader *s) {
    return shader_load(s, "far_terrain", "assets/dfe/shaders/far.vert", "assets/dfe/shaders/far.frag", "");
}

bool far_init(void) {
    memset(&F, 0, sizeof F);
    if (!load_far_shader(&F.shader)) return false;
    u16 *idx = xmalloc((size_t)MAX_TILE_QUADS * 6 * sizeof(u16));
    for (int q = 0; q < MAX_TILE_QUADS; q++) {
        u16 *p = idx + q * 6, v = (u16)(q * 4);
        p[0] = v; p[1] = (u16)(v + 1); p[2] = (u16)(v + 2); p[3] = v; p[4] = (u16)(v + 2); p[5] = (u16)(v + 3);
    }
    glGenBuffers(1, &F.ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, F.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)((size_t)MAX_TILE_QUADS * 6 * sizeof(u16)), idx, GL_STATIC_DRAW);
    free(idx);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glGenTextures(1, &F.cover_tex);
    F.ready = true;
    return true;
}

static void tile_destroy_gl(FarTile *t) {
    if (t->vbo) glDeleteBuffers(1, &t->vbo);
    if (t->vao) glDeleteVertexArrays(1, &t->vao);
    t->vbo = t->vao = 0;
}

void far_shutdown(void) {
    if (!F.ready) return;
    for (int i = 0; i < F.tile_count; i++) tile_destroy_gl(&F.tiles[i]);
    shader_destroy(&F.shader);
    glDeleteBuffers(1, &F.ibo);
    glDeleteTextures(1, &F.cover_tex);
    free(F.cover);
    memset(&F, 0, sizeof F);
}

bool far_reload_shader(void) {
    Shader fresh;
    memset(&fresh, 0, sizeof fresh);
    if (!load_far_shader(&fresh)) return false;
    shader_destroy(&F.shader);
    F.shader = fresh;
    return true;
}

/* ----------------------------------------------------------------- build */

static float water_depth_mix(float depth) { return CLAMP(depth / DEEP_WATER_DEPTH, 0.0f, 1.0f); }

typedef struct TileBuild {
    FarVertex *v;
    int n;
    float ymin, ymax;
} TileBuild;

static FarVertex far_vertex(int gx, int gz, int top_block, const u8 rgb[3], int face, bool water) {
    FarVertex v;
    v.x = (u8)gx; v.z = (u8)gz;
    v.height = (i16)CLAMP(top_block * (int)HEIGHT_UNITS_PER_BLOCK, -32768, 32767);
    v.r = rgb[0]; v.g = rgb[1]; v.b = rgb[2];
    v.flags = (u8)(face | (water ? FLAG_WATER : 0));
    return v;
}

static void emit_quad(TileBuild *tb, const FarVertex q[4]) {
    memcpy(tb->v + tb->n, q, 4 * sizeof(FarVertex));
    tb->n += 4;
    for (int k = 0; k < 4; k++) {
        float y = (float)q[k].height / HEIGHT_UNITS_PER_BLOCK;
        if (y < tb->ymin) tb->ymin = y;
        if (y > tb->ymax) tb->ymax = y;
    }
}

/* Wall on the side `d` of column (i, j), from the lower neighbour's top up to this column's top. Corners are
 * ordered counter-clockwise seen from outside. */
static void emit_wall(TileBuild *tb, int i, int j, int d, int top, bool top_water, int bottom, bool bottom_water, const u8 rgb[3]) {
    /* Per side: x offset, z offset, high or low, for the four corners. */
    static const int CORNER[6][4][3] = {
        [DIR_PX] = {{1, 0, 0}, {1, 0, 1}, {1, 1, 1}, {1, 1, 0}},
        [DIR_NX] = {{0, 1, 0}, {0, 1, 1}, {0, 0, 1}, {0, 0, 0}},
        [DIR_PZ] = {{0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 1, 1}},
        [DIR_NZ] = {{1, 0, 0}, {0, 0, 0}, {0, 0, 1}, {1, 0, 1}},
    };
    FarVertex q[4];
    for (int k = 0; k < 4; k++) {
        bool high = CORNER[d][k][2];
        q[k] = far_vertex(i + CORNER[d][k][0], j + CORNER[d][k][1], high ? top : bottom, rgb, d, high ? top_water : bottom_water);
    }
    emit_quad(tb, q);
}

static void far_job_run(void *data, int worker) {
    FarJob *j = data;
    enum { SIDE = TILE_CELLS + 2 };
    int sea = gen_sea_level();
    /* Column tops sampled at the column centre, with one extra ring so border walls agree with the neighbouring tile. */
    int *top = xmalloc((size_t)SIDE * SIDE * sizeof(int));
    float *hf = xmalloc((size_t)SIDE * SIDE * sizeof(float));
    for (int z = 0; z < SIDE; z++)
        for (int x = 0; x < SIDE; x++) {
            float wx = (float)(j->tx * TILE_BLOCKS + (x - 1) * CELL_BLOCKS) + CELL_BLOCKS * 0.5f;
            float wz = (float)(j->tz * TILE_BLOCKS + (z - 1) * CELL_BLOCKS) + CELL_BLOCKS * 0.5f;
            float h = gen_height_at(wx, wz);
            hf[z * SIDE + x] = h;
            top[z * SIDE + x] = (int)floorf(h) - (int)FAR_DROP_BLOCKS;
            /* Sea beds are quantised coarsely: their steps are lifted into one level surface by the vertex shader,
             * and fewer distinct heights mean fewer walls to lift. */
            if (h < (float)sea) top[z * SIDE + x] = (int)floorf(h / OCEAN_STEP_BLOCKS) * (int)OCEAN_STEP_BLOCKS;
        }
    TileBuild tb = {.v = xmalloc((size_t)MAX_TILE_QUADS * 4 * sizeof(FarVertex)), .ymin = 1e9f, .ymax = -1e9f};
    for (int cz = 0; cz < TILE_CELLS; cz++)
        for (int cx = 0; cx < TILE_CELLS; cx++) {
            int idx = (cz + 1) * SIDE + cx + 1;
            float hc = hf[idx];
            int wx = j->tx * TILE_BLOCKS + cx * CELL_BLOCKS, wz = j->tz * TILE_BLOCKS + cz * CELL_BLOCKS;
            bool water = hc < (float)sea;
            float r, g, b;
            if (water) {
                float m = water_depth_mix((float)sea - hc);
                r = lerpf(SHALLOW_WATER[0], DEEP_WATER[0], m); g = lerpf(SHALLOW_WATER[1], DEEP_WATER[1], m); b = lerpf(SHALLOW_WATER[2], DEEP_WATER[2], m);
            } else {
                u32 col = gen_far_color_at((float)wx + CELL_BLOCKS * 0.5f, (float)wz + CELL_BLOCKS * 0.5f, hc);
                r = (float)(col & 255) / 255.0f; g = (float)((col >> 8) & 255) / 255.0f; b = (float)((col >> 16) & 255) / 255.0f;
            }
            /* Per-column brightness variation stands in for the texture detail the far layer does not have. */
            float jitter = 1.0f + (hash_to_unit(hash3(7, wx, 0, wz)) - 0.5f) * 2.0f * COLOR_JITTER;
            u8 rgb[3] = {(u8)CLAMP((int)(r * jitter * 255.0f), 0, 255), (u8)CLAMP((int)(g * jitter * 255.0f), 0, 255), (u8)CLAMP((int)(b * jitter * 255.0f), 0, 255)};
            int t = top[idx];
            FarVertex q[4] = {far_vertex(cx, cz, t, rgb, DIR_PY, water), far_vertex(cx, cz + 1, t, rgb, DIR_PY, water),
                              far_vertex(cx + 1, cz + 1, t, rgb, DIR_PY, water), far_vertex(cx + 1, cz, t, rgb, DIR_PY, water)};
            emit_quad(&tb, q);
            for (int d = 0; d < 6; d++) {
                if (d == DIR_PY || d == DIR_NY) continue;
                int ni = idx + DIR_VEC[d][2] * SIDE + DIR_VEC[d][0];
                bool nwater = hf[ni] < (float)sea;
                if (top[ni] >= t) continue;
                emit_wall(&tb, cx, cz, d, t, water, top[ni], nwater, rgb);
            }
            if (water) tb.ymax = MAX(tb.ymax, (float)sea - FAR_WATER_DROP_BLOCKS);
        }
    j->verts = tb.v;
    j->vert_count = tb.n;
    j->ymin = tb.ymin; j->ymax = tb.ymax;
    free(top);
    free(hf);
}

static FarTile *find_tile(int tx, int tz) {
    for (int i = 0; i < F.tile_count; i++)
        if (F.tiles[i].tx == tx && F.tiles[i].tz == tz) return &F.tiles[i];
    return NULL;
}

static void far_job_complete(void *data) {
    FarJob *j = data;
    F.jobs_inflight--;
    FarTile *t = NULL;
    for (int i = 0; i < F.tile_count; i++) if (F.tiles[i].serial == j->serial) t = &F.tiles[i];
    if (t && F.ready) {
        glGenVertexArrays(1, &t->vao);
        glGenBuffers(1, &t->vbo);
        glBindVertexArray(t->vao);
        glBindBuffer(GL_ARRAY_BUFFER, t->vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)j->vert_count * sizeof(FarVertex)), j->verts, GL_STATIC_DRAW);
        glVertexAttribIPointer(0, 2, GL_UNSIGNED_BYTE, sizeof(FarVertex), (void *)offsetof(FarVertex, x));
        glVertexAttribIPointer(1, 1, GL_SHORT, sizeof(FarVertex), (void *)offsetof(FarVertex, height));
        glVertexAttribPointer(2, 3, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(FarVertex), (void *)offsetof(FarVertex, r));
        glVertexAttribIPointer(3, 1, GL_UNSIGNED_BYTE, sizeof(FarVertex), (void *)offsetof(FarVertex, flags));
        for (int a = 0; a < 4; a++) glEnableVertexAttribArray((GLuint)a);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, F.ibo);
        glBindVertexArray(0);
        t->ymin = j->ymin; t->ymax = j->ymax;
        t->index_count = j->vert_count / 4 * 6;
        t->ready = true;
    }
    free(j->verts);
    free(j);
}

static void tile_remove(int i) {
    tile_destroy_gl(&F.tiles[i]);
    F.tiles[i] = F.tiles[--F.tile_count];
}

static float tile_dist2(int tx, int tz, V3 p) {
    float cx = (float)tx * TILE_BLOCKS + TILE_BLOCKS * 0.5f - p.x, cz = (float)tz * TILE_BLOCKS + TILE_BLOCKS * 0.5f - p.z;
    return cx * cx + cz * cz;
}

/* Keeps the tile set matched to the camera. A full scan is cheap but not free, so it runs on tile changes or periodically. */
static void far_schedule(const Camera *cam) {
    int ctx = floor_div(ifloor(cam->pos.x), TILE_BLOCKS), ctz = floor_div(ifloor(cam->pos.z), TILE_BLOCKS);
    F.frames_since_scan++;
    bool moved = ctx != F.center_tx || ctz != F.center_tz;
    if (!moved && F.frames_since_scan < RESCAN_FRAMES) return;
    F.center_tx = ctx; F.center_tz = ctz; F.frames_since_scan = 0;
    float keep = (float)F.range_blocks + TILE_BLOCKS * 1.5f, want = (float)F.range_blocks + TILE_BLOCKS * 0.7f;
    for (int i = F.tile_count - 1; i >= 0; i--)
        if (tile_dist2(F.tiles[i].tx, F.tiles[i].tz, cam->pos) > keep * keep) tile_remove(i);
    int r = F.range_blocks / TILE_BLOCKS + 2;
    for (int n = 0; n < MAX_TILE_JOBS - F.jobs_inflight; n++) {
        float best = 1e30f;
        int bx = 0, bz = 0;
        for (int dz = -r; dz <= r; dz++)
            for (int dx = -r; dx <= r; dx++) {
                int tx = ctx + dx, tz = ctz + dz;
                float d2 = tile_dist2(tx, tz, cam->pos);
                if (d2 > want * want || d2 >= best || find_tile(tx, tz)) continue;
                best = d2; bx = tx; bz = tz;
            }
        if (best >= 1e30f || F.tile_count == MAX_TILES) break;
        FarTile *t = &F.tiles[F.tile_count++];
        memset(t, 0, sizeof *t);
        t->tx = bx; t->tz = bz; t->serial = ++F.serial;
        FarJob *j = xcalloc(1, sizeof *j);
        j->tx = bx; j->tz = bz; j->serial = t->serial;
        F.jobs_inflight++;
        jobs_submit(JOB_KIND_FAR, best, far_job_run, far_job_complete, j);
    }
}

/* ---------------------------------------------------------------- render */

static void upload_coverage(const Camera *cam, int rd, int *origin_x, int *origin_z) {
    int fcx = floor_div(ifloor(cam->pos.x), CHUNK_SIZE), fcz = floor_div(ifloor(cam->pos.z), CHUNK_SIZE);
    int dim = 2 * rd + 5;
    int ox = fcx - rd - 2, oz = fcz - rd - 2;
    if (dim * dim > F.cover_cap) {
        F.cover_cap = dim * dim;
        F.cover = xrealloc(F.cover, (size_t)F.cover_cap);
    }
    /* meshed[] first, then erode by one column so the layers overlap in a ring. */
    u8 *meshed = xcalloc((size_t)dim * dim, 1);
    for (int z = 0; z < dim; z++)
        for (int x = 0; x < dim; x++) {
            int dx = ox + x - fcx, dz = oz + z - fcz;
            if (dx * dx + dz * dz <= rd * rd && world_column_meshed(ox + x, oz + z)) meshed[z * dim + x] = 1;
        }
    for (int z = 0; z < dim; z++)
        for (int x = 0; x < dim; x++) {
            bool in = meshed[z * dim + x];
            if (in) {
                static const int d4[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (int k = 0; k < 4; k++) {
                    int nx = x + d4[k][0], nz = z + d4[k][1];
                    if (nx < 0 || nz < 0 || nx >= dim || nz >= dim) continue; /* beyond the window nothing is drawn near anyway */
                    if (!meshed[nz * dim + nx]) { in = false; break; }
                }
            }
            F.cover[z * dim + x] = in ? 255 : 0;
        }
    free(meshed);
    F.cover_dim = dim;
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, F.cover_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (dim != F.cover_gl_dim) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, dim, dim, 0, GL_RED, GL_UNSIGNED_BYTE, F.cover);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        F.cover_gl_dim = dim;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, dim, dim, GL_RED, GL_UNSIGNED_BYTE, F.cover);
    }
    *origin_x = ox;
    *origin_z = oz;
}

void far_render(const Camera *cam, double time_s, int rd, int far_chunks, const FarFog *fog) {
    if (!F.ready || far_chunks <= 0) return;
    F.range_blocks = (rd + far_chunks) * CHUNK_SIZE;
    far_schedule(cam);
    Shader *sh = &F.shader;
    shader_use(sh);
    int bx = ifloor(cam->pos.x), by = ifloor(cam->pos.y), bz = ifloor(cam->pos.z);
    glUniform3i(shader_uniform(sh, "u_cam_base"), bx, by, bz);
    glUniform3f(shader_uniform(sh, "u_cam_frac"), cam->pos.x - (float)bx, cam->pos.y - (float)by, cam->pos.z - (float)bz);
    M4 rot_view = m4_look_dir(v3(0, 0, 0), cam->forward, v3(0, 1, 0));
    M4 vp = m4_mul(cam->proj, rot_view);
    glUniformMatrix4fv(shader_uniform(sh, "u_viewproj"), 1, GL_FALSE, vp.m);
    glUniform1f(shader_uniform(sh, "u_height_scale"), 1.0f / HEIGHT_UNITS_PER_BLOCK);
    glUniform1f(shader_uniform(sh, "u_cell"), (float)CELL_BLOCKS);
    glUniform1f(shader_uniform(sh, "u_sea"), (float)gen_sea_level() - FAR_WATER_DROP_BLOCKS);
    glUniform1f(shader_uniform(sh, "u_ramp_start"), (float)(rd * CHUNK_SIZE));
    glUniform1f(shader_uniform(sh, "u_ramp_end"), (float)(rd * CHUNK_SIZE) + WATER_RAMP_BLOCKS);
    glUniform3f(shader_uniform(sh, "u_sun_dir"), fog->sun.x, fog->sun.y, fog->sun.z);
    glUniform3f(shader_uniform(sh, "u_sky_color"), fog->sky.x, fog->sky.y, fog->sky.z);
    glUniform3f(shader_uniform(sh, "u_fog_color"), fog->color.x, fog->color.y, fog->color.z);
    glUniform1f(shader_uniform(sh, "u_fog_start"), fog->start);
    glUniform1f(shader_uniform(sh, "u_fog_end"), fog->end);
    int ox, oz;
    upload_coverage(cam, rd, &ox, &oz);
    glUniform1i(shader_uniform(sh, "u_cover"), 3);
    glUniform2i(shader_uniform(sh, "u_cover_origin"), ox, oz);
    glUniform1i(shader_uniform(sh, "u_cover_dim"), F.cover_dim);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, F.cover_tex);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    int drawn = 0;
    for (int i = 0; i < F.tile_count; i++) {
        FarTile *t = &F.tiles[i];
        if (!t->ready) continue;
        V3 lo = v3((float)t->tx * TILE_BLOCKS, t->ymin, (float)t->tz * TILE_BLOCKS);
        V3 hi = v3(lo.x + TILE_BLOCKS, t->ymax, lo.z + TILE_BLOCKS);
        if (!frustum_box_visible(&cam->frustum, lo, hi)) continue;
        glUniform2i(shader_uniform(sh, "u_tile_origin"), t->tx * TILE_BLOCKS, t->tz * TILE_BLOCKS);
        glBindVertexArray(t->vao);
        glDrawElements(GL_TRIANGLES, t->index_count, GL_UNSIGNED_SHORT, NULL);
        g_scene_stats.draw_calls++;
        drawn++;
    }
    g_scene_stats.far_tiles_drawn = drawn;
    g_scene_stats.far_tiles_total = F.tile_count;
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
    (void)time_s;
}
