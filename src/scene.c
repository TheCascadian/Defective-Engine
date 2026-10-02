/* Chunk rendering: shared vertex arena, visibility walk, multi-draw.
 *
 * Vertex memory is a few large buffers ("pages") cut into 256-vertex granules, so the whole world is drawn
 * with one glMultiDrawElementsBaseVertex per page and pass instead of one call per chunk. Each granule has
 * an entry in a texture buffer holding its chunk origin, looked up with gl_VertexID, which avoids a
 * per-draw uniform and avoids repeating the origin in every vertex. Rejected: per-chunk VBOs (thousands of
 * draw calls, bad for a driver-bound integrated GPU) and glDrawElementsIndirect (needs GL 4.0).
 */
#include "dfe.h"

#define PAGE_VERTS (4u << 20)
#define PAGE_GRANULES (PAGE_VERTS / MESH_GRANULE)
#define MAX_PAGES 16
#define VIS_VERTICAL_ABOVE 8
#define INDEX_QUADS MESH_MAX_QUADS_PER_DRAW

SceneConfig g_scene_cfg = {.render_distance = 8, .fov_deg = 75.0f, .occlusion_culling = true};
SceneStats g_scene_stats;

typedef struct Page {
    GLuint vbo, vao, origin_buf, origin_tex;
    u64 used[PAGE_GRANULES / 64];
    u32 free_granules, cursor;
} Page;

static struct {
    Page pages[MAX_PAGES];
    int page_count;
    GLuint ibo;
    Shader shader[LAYER_COUNT];
    bool ready;
    u32 frame;
    u64 resident_granules;
    /* Per-frame scratch. */
    Chunk **visible;
    int visible_n, visible_cap;
    u32 *cell_stamp;
    Chunk **cell_chunk;
    u8 *cell_entry;
    size_t cell_cap;
    GLsizei *draw_count;
    const void **draw_index;
    GLint *draw_base;
    int draw_cap;
} S;

/* -------------------------------------------------------------------- arena */

static void page_init(Page *p) {
    memset(p, 0, sizeof *p);
    p->free_granules = PAGE_GRANULES;
    glGenBuffers(1, &p->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, p->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)PAGE_VERTS * sizeof(MeshVertex), NULL, GL_DYNAMIC_DRAW);
    glGenVertexArrays(1, &p->vao);
    glBindVertexArray(p->vao);
    glVertexAttribIPointer(0, 1, GL_UNSIGNED_INT, sizeof(MeshVertex), (void *)0);
    glVertexAttribIPointer(1, 1, GL_UNSIGNED_INT, sizeof(MeshVertex), (void *)4);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, S.ibo);
    glBindVertexArray(0);
    glGenBuffers(1, &p->origin_buf);
    glBindBuffer(GL_TEXTURE_BUFFER, p->origin_buf);
    glBufferData(GL_TEXTURE_BUFFER, (GLsizeiptr)PAGE_GRANULES * 4 * sizeof(i32), NULL, GL_DYNAMIC_DRAW);
    glGenTextures(1, &p->origin_tex);
    glBindTexture(GL_TEXTURE_BUFFER, p->origin_tex);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32I, p->origin_buf);
}

static void page_destroy(Page *p) {
    glDeleteBuffers(1, &p->vbo);
    glDeleteBuffers(1, &p->origin_buf);
    glDeleteTextures(1, &p->origin_tex);
    glDeleteVertexArrays(1, &p->vao);
}

static inline bool bit_get(const Page *p, u32 i) { return (p->used[i >> 6] >> (i & 63)) & 1; }
static inline void bit_set(Page *p, u32 i, bool v) {
    if (v) p->used[i >> 6] |= 1ull << (i & 63); else p->used[i >> 6] &= ~(1ull << (i & 63));
}

/* First-fit scan starting at a rotating cursor, which spreads allocations and keeps the search short. */
static int page_find_run(Page *p, u32 n) {
    if (p->free_granules < n) return -1;
    for (int pass = 0; pass < 2; pass++) {
        u32 start = pass == 0 ? p->cursor : 0, end = pass == 0 ? PAGE_GRANULES : p->cursor + n;
        if (end > PAGE_GRANULES) end = PAGE_GRANULES;
        u32 run = 0;
        for (u32 i = start; i < end; i++) {
            if (bit_get(p, i)) { run = 0; continue; }
            if (++run == n) return (int)(i + 1 - n);
        }
    }
    return -1;
}

static bool arena_alloc(u32 granules, MeshSlot *slot) {
    for (int i = 0; i < MAX_PAGES; i++) {
        if (i == S.page_count) {
            page_init(&S.pages[S.page_count++]);
            LOGI("Vertex arena page %d allocated (%u MB each)", i, (unsigned)(PAGE_VERTS * sizeof(MeshVertex) >> 20));
        }
        Page *p = &S.pages[i];
        int at = page_find_run(p, granules);
        if (at < 0) continue;
        for (u32 g = 0; g < granules; g++) bit_set(p, (u32)at + g, true);
        p->free_granules -= granules;
        p->cursor = (u32)at + granules;
        slot->page = i;
        slot->first = (u32)at * MESH_GRANULE;
        slot->granules = granules;
        S.resident_granules += granules;
        return true;
    }
    LOGE("Vertex arena is full (%d pages). Lower the render distance or raise MAX_PAGES in scene.c.", MAX_PAGES);
    return false;
}

static void slot_release(MeshSlot *slot) {
    if (slot->page < 0 || slot->granules == 0) { slot->page = -1; slot->count = 0; return; }
    Page *p = &S.pages[slot->page];
    u32 first = slot->first / MESH_GRANULE;
    for (u32 g = 0; g < slot->granules; g++) bit_set(p, first + g, false);
    p->free_granules += slot->granules;
    S.resident_granules -= slot->granules;
    slot->page = -1;
    slot->count = 0;
    slot->granules = 0;
}

void scene_free_chunk(Chunk *c) {
    if (!S.ready) return;
    for (int l = 0; l < LAYER_COUNT; l++) slot_release(&c->mesh[l]);
    c->flags &= ~CF_HAS_MESH;
}

void scene_upload_mesh(Chunk *c, MeshOutput *out) {
    if (!S.ready) return;
    bool any = false;
    for (int l = 0; l < LAYER_COUNT; l++) {
        MeshSlot old = c->mesh[l];
        MeshSlot fresh = {.page = -1};
        u32 n = out->count[l];
        if (n) {
            u32 granules = (n + MESH_GRANULE - 1) / MESH_GRANULE;
            if (arena_alloc(granules, &fresh)) {
                fresh.count = n;
                Page *p = &S.pages[fresh.page];
                glBindBuffer(GL_ARRAY_BUFFER, p->vbo);
                glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)fresh.first * sizeof(MeshVertex), (GLsizeiptr)n * sizeof(MeshVertex), out->verts[l]);
                i32 origin[4] = {c->cx * CHUNK_SIZE, c->cy * CHUNK_SIZE, c->cz * CHUNK_SIZE, 0};
                glBindBuffer(GL_TEXTURE_BUFFER, p->origin_buf);
                for (u32 g = 0; g < granules; g++)
                    glBufferSubData(GL_TEXTURE_BUFFER, (GLintptr)((fresh.first / MESH_GRANULE + g) * 4 * sizeof(i32)), sizeof origin, origin);
                g_scene_stats.upload_bytes_total += (u64)n * sizeof(MeshVertex);
                any = true;
            }
        }
        /* The old mesh is released only after the new one is resident, so a chunk never flickers empty. */
        slot_release(&old);
        c->mesh[l] = fresh;
    }
    g_scene_stats.uploads_this_frame++;
    c->flags |= CF_HAS_MESH;
    (void)any;
}

/* ------------------------------------------------------------------ set up */

static bool build_index_buffer(void) {
    u16 *idx = xmalloc((size_t)INDEX_QUADS * 6 * sizeof(u16));
    for (u32 q = 0; q < INDEX_QUADS; q++) {
        u16 b = (u16)(q * 4);
        u16 *p = idx + q * 6;
        p[0] = b; p[1] = b + 1; p[2] = b + 2; p[3] = b; p[4] = b + 2; p[5] = b + 3;
    }
    glGenBuffers(1, &S.ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, S.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)INDEX_QUADS * 6 * sizeof(u16), idx, GL_STATIC_DRAW);
    free(idx);
    return true;
}

static bool load_shaders(Shader out[LAYER_COUNT]) {
    static const char *const defs[LAYER_COUNT] = {"#define PASS_OPAQUE 1\n", "#define PASS_CUTOUT 1\n", "#define PASS_TRANSLUCENT 1\n"};
    static const char *const names[LAYER_COUNT] = {"chunk_opaque", "chunk_cutout", "chunk_translucent"};
    for (int l = 0; l < LAYER_COUNT; l++)
        if (!shader_load(&out[l], names[l], "assets/dfe/shaders/chunk.vert", "assets/dfe/shaders/chunk.frag", defs[l])) return false;
    return true;
}

bool scene_init(void) {
    memset(&S, 0, sizeof S);
    if (!build_index_buffer()) return false;
    if (!load_shaders(S.shader)) return false;
    page_init(&S.pages[S.page_count++]);
    S.ready = true;
    return true;
}

bool scene_reload_shaders(void) {
    Shader fresh[LAYER_COUNT];
    memset(fresh, 0, sizeof fresh);
    if (!load_shaders(fresh)) {
        for (int l = 0; l < LAYER_COUNT; l++) if (fresh[l].program) shader_destroy(&fresh[l]);
        return false;
    }
    for (int l = 0; l < LAYER_COUNT; l++) { shader_destroy(&S.shader[l]); S.shader[l] = fresh[l]; }
    return true;
}

void scene_shutdown(void) {
    if (!S.ready) return;
    for (int i = 0; i < S.page_count; i++) page_destroy(&S.pages[i]);
    for (int l = 0; l < LAYER_COUNT; l++) shader_destroy(&S.shader[l]);
    glDeleteBuffers(1, &S.ibo);
    free(S.visible); free(S.cell_stamp); free(S.cell_chunk); free(S.cell_entry);
    free(S.draw_count); free(S.draw_index); free(S.draw_base);
    memset(&S, 0, sizeof S);
}

void scene_resize(int w, int h) { (void)w; (void)h; }
void scene_mesh_job_complete_hook(void) {}

/* --------------------------------------------------------------- visibility */

static inline int opposite(int d) { return d ^ 1; }

/* The walk travels through the face-connectivity graph. A chunk entered while moving in directions T may be
 * left in direction d only if some entry face is connected to d inside the chunk, and never back toward the
 * camera (no step may reverse a direction already taken), which removes cycles and over-inclusion. */
static bool can_leave(u16 conn, u8 travelled, int d, bool is_start) {
    if (is_start) return true;
    if (travelled & (1u << opposite(d))) return false;
    for (int t = 0; t < 6; t++)
        if ((travelled & (1u << t)) && chunk_faces_connected(conn, opposite(t), d)) return true;
    return false;
}

static void ensure_cells(size_t n) {
    if (n <= S.cell_cap) return;
    S.cell_cap = n;
    S.cell_stamp = xrealloc(S.cell_stamp, n * sizeof(u32));
    S.cell_chunk = xrealloc(S.cell_chunk, n * sizeof(Chunk *));
    S.cell_entry = xrealloc(S.cell_entry, n);
    memset(S.cell_stamp, 0, n * sizeof(u32));
}

static void visible_push(Chunk *c) {
    if (S.visible_n == S.visible_cap) {
        S.visible_cap = S.visible_cap ? S.visible_cap * 2 : 1024;
        S.visible = xrealloc(S.visible, (size_t)S.visible_cap * sizeof(Chunk *));
    }
    S.visible[S.visible_n++] = c;
}

typedef struct QItem { i32 cx, cy, cz; u8 travelled; } QItem;

static void walk_visibility(const Camera *cam, int rd) {
    S.visible_n = 0;
    int lo, hi;
    gen_band(&lo, &hi);
    int vmin = lo - 1, vmax = hi + VIS_VERTICAL_ABOVE;
    int dim = 2 * rd + 3, layers = vmax - vmin + 1;
    size_t cells = (size_t)dim * dim * layers;
    ensure_cells(cells);
    int ccx = ifloor(cam->pos.x / 32.0f), ccz = ifloor(cam->pos.z / 32.0f);
    int ccy = CLAMP(ifloor(cam->pos.y / 32.0f), vmin, vmax);
    int ox = ccx - rd - 1, oz = ccz - rd - 1;
    S.frame++;
    QItem *queue = xmalloc(cells * sizeof(QItem));
    size_t qh = 0, qt = 0;
    queue[qt++] = (QItem){ccx, ccy, ccz, 0};
    #define CELL(cx, cy, cz) ((((size_t)((cy) - vmin) * dim) + (size_t)((cz) - oz)) * dim + (size_t)((cx) - ox))
    S.cell_stamp[CELL(ccx, ccy, ccz)] = S.frame;
    int rd2 = rd * rd;
    int in_range = 0, culled_frustum = 0;
    while (qh < qt) {
        QItem it = queue[qh++];
        bool is_start = it.cx == ccx && it.cy == ccy && it.cz == ccz;
        Chunk *c = world_chunk(it.cx, it.cy, it.cz);
        u16 conn = 0x7FFF;
        if (c) {
            if (c->flags & CF_MESHED_ONCE) conn = c->conn;
            if (c->flags & CF_HAS_MESH) visible_push(c);
        } else {
            Column *col = world_column(it.cx, it.cz);
            if (!col || col->state != COLUMN_READY) continue;
            if (it.cy < col->lo_cy) conn = 0;
        }
        for (int d = 0; d < 6; d++) {
            int nx = it.cx + DIR_VEC[d][0], ny = it.cy + DIR_VEC[d][1], nz = it.cz + DIR_VEC[d][2];
            if (ny < vmin || ny > vmax) continue;
            int dx = nx - ccx, dz = nz - ccz;
            if (dx * dx + dz * dz > rd2) continue;
            size_t ci = CELL(nx, ny, nz);
            if (S.cell_stamp[ci] == S.frame) continue;
            if (g_scene_cfg.occlusion_culling && !can_leave(conn, it.travelled, d, is_start)) continue;
            in_range++;
            V3 lo3 = v3((float)nx * 32.0f, (float)ny * 32.0f, (float)nz * 32.0f);
            V3 hi3 = v3(lo3.x + 32.0f, lo3.y + 32.0f, lo3.z + 32.0f);
            if (!frustum_box_visible(&cam->frustum, lo3, hi3)) { culled_frustum++; continue; }
            S.cell_stamp[ci] = S.frame;
            queue[qt++] = (QItem){nx, ny, nz, (u8)(it.travelled | (1u << d))};
        }
    }
    #undef CELL
    free(queue);
    g_scene_stats.chunks_in_range = in_range;
    g_scene_stats.chunks_culled_frustum = culled_frustum;
    g_scene_stats.chunks_visible = S.visible_n;
}

/* -------------------------------------------------------------------- draw */

static void ensure_draws(int n) {
    if (n <= S.draw_cap) return;
    S.draw_cap = n * 2;
    S.draw_count = xrealloc(S.draw_count, (size_t)S.draw_cap * sizeof(GLsizei));
    S.draw_index = xrealloc(S.draw_index, (size_t)S.draw_cap * sizeof(void *));
    S.draw_base = xrealloc(S.draw_base, (size_t)S.draw_cap * sizeof(GLint));
}

static void draw_layer(int layer, bool reverse) {
    for (int pg = 0; pg < S.page_count; pg++) {
        int n = 0;
        for (int k = 0; k < S.visible_n; k++) {
            Chunk *c = S.visible[reverse ? S.visible_n - 1 - k : k];
            const MeshSlot *m = &c->mesh[layer];
            if (m->page != pg || m->count == 0) continue;
            u32 quads = m->count / 4, done = 0;
            ensure_draws(n + (int)(quads / INDEX_QUADS) + 1);
            while (done < quads) {
                u32 take = MIN(quads - done, (u32)INDEX_QUADS);
                S.draw_count[n] = (GLsizei)(take * 6);
                S.draw_index[n] = NULL;
                S.draw_base[n] = (GLint)(m->first + done * 4);
                n++;
                done += take;
            }
            g_scene_stats.chunks_drawn[layer]++;
            g_scene_stats.vertices_drawn += m->count;
        }
        if (!n) continue;
        Page *p = &S.pages[pg];
        glBindVertexArray(p->vao);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_BUFFER, p->origin_tex);
        glMultiDrawElementsBaseVertex(GL_TRIANGLES, S.draw_count, GL_UNSIGNED_SHORT, S.draw_index, n, S.draw_base);
        g_scene_stats.draw_calls++;
    }
}

static void set_pass_uniforms(Shader *sh, const Camera *cam, double time_s, int rd) {
    shader_use(sh);
    int bx = ifloor(cam->pos.x), by = ifloor(cam->pos.y), bz = ifloor(cam->pos.z);
    glUniform3i(shader_uniform(sh, "u_cam_base"), bx, by, bz);
    glUniform3f(shader_uniform(sh, "u_cam_frac"), cam->pos.x - (float)bx, cam->pos.y - (float)by, cam->pos.z - (float)bz);
    M4 rot_view = m4_look_dir(v3(0, 0, 0), cam->forward, v3(0, 1, 0));
    M4 vp = m4_mul(cam->proj, rot_view);
    glUniformMatrix4fv(shader_uniform(sh, "u_viewproj"), 1, GL_FALSE, vp.m);
    glUniform1f(shader_uniform(sh, "u_time"), (float)time_s);
    static const float tints[12] = {1, 1, 1, 0.55f, 0.78f, 0.35f, 0.42f, 0.66f, 0.30f, 0.30f, 0.52f, 0.80f};
    glUniform3fv(shader_uniform(sh, "u_tint"), 4, tints);
    glUniform3f(shader_uniform(sh, "u_water_deep"), 0.08f, 0.22f, 0.45f);
    glUniform1i(shader_uniform(sh, "u_origins"), 1);
    glUniform1i(shader_uniform(sh, "u_tex"), 0);
    glUniform1i(shader_uniform(sh, "u_anim"), 2);
    glUniform3f(shader_uniform(sh, "u_sky_color"), 1.0f, 1.0f, 1.0f);
    glUniform1f(shader_uniform(sh, "u_ambient"), 0.04f);
    glUniform3f(shader_uniform(sh, "u_fog_color"), 0.62f, 0.76f, 0.95f);
    float end = (float)rd * 32.0f - 12.0f;
    glUniform1f(shader_uniform(sh, "u_fog_start"), end * 0.55f);
    glUniform1f(shader_uniform(sh, "u_fog_end"), end);
}

void scene_render(const Camera *cam, double time_s) {
    if (!S.ready) return;
    int rd = g_scene_cfg.render_distance;
    g_scene_stats.draw_calls = 0;
    g_scene_stats.vertices_drawn = 0;
    memset(g_scene_stats.chunks_drawn, 0, sizeof g_scene_stats.chunks_drawn);
    walk_visibility(cam, rd);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, g_tex.gl_array);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, g_tex.gl_anim);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    for (int l = 0; l < LAYER_COUNT; l++) {
        bool translucent = l == LAYER_TRANSLUCENT;
        if (translucent) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
        }
        set_pass_uniforms(&S.shader[l], cam, time_s, rd);
        draw_layer(l, translucent);
        if (translucent) {
            glDisable(GL_BLEND);
            glDepthMask(GL_TRUE);
        }
    }
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
    g_scene_stats.arena_pages = S.page_count;
    g_scene_stats.arena_used_mb = (double)S.resident_granules * MESH_GRANULE * sizeof(MeshVertex) / 1048576.0;
}
