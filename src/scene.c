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

/* ------------------------------------------------------------- PBR tuning */

/* Every PBR knob lives in PbrCfg (dfe.h, all floats so the menu and the saved file can walk it as a table). The base
 * values come from data/dfe/pbr.json, a texture-quality preset replaces them, and "custom" uses g_settings.pbr. Each
 * is clamped to its range. Reloaded with the shaders. */
const PbrField PBR_FIELDS[PBR_FIELD_COUNT] = {
    {"enabled", "PBR Surfaces", -1, 0, 1, 1, 0},
    {"bump_strength", "Relief Strength", 0, 0, 4, 0.05f, 2},
    {"height_depth", "Relief Depth", 0, 0, 0.5f, 0.005f, 3},
    {"normal_strength", "Normal Strength", 0, 0, 4, 0.05f, 2},
    {"fade_start", "Relief Fade Start", 0, 0, 256, 1, 0},
    {"fade_end", "Relief Fade End", 0, 0, 512, 1, 0},
    {"cavity_ao", "Cavity Darkening", 0, 0, 1, 0.05f, 2},
    {"specular_strength", "Highlight Brightness", 1, 0, 8, 0.1f, 1},
    {"sky_specular", "Sky Reflection", 1, 0, 4, 0.05f, 2},
    {"roughness_scale", "Roughness Scale", 1, 0, 4, 0.05f, 2},
    {"roughness_bias", "Roughness Bias", 1, -1, 1, 0.05f, 2},
    {"metalness_scale", "Metalness Scale", 1, 0, 2, 0.05f, 2},
    {"diffuse_response", "Diffuse Response", 1, 0, 3, 0.05f, 2},
    {"sky_lean", "Sky Ambient Lean", 1, 0, 3, 0.05f, 2},
    {"shade_floor", "Shade Floor", 1, 0, 1, 0.01f, 2},
    {"self_shadow_strength", "Contact Shadow", 2, 0, 1, 0.05f, 2},
    {"self_shadow_reach", "Shadow Depth", 2, 0, 8, 0.1f, 1},
    {"self_shadow_steps", "Shadow Steps", 2, 0, 16, 1, 0},
    {"shadow_tap_cap", "Soft Shadow Taps", 2, 1, 16, 1, 0},
    {"parallax_depth", "Parallax Depth", 4, 0, 0.5f, 0.005f, 3},
    {"parallax_steps", "Parallax Steps", 4, 0, 32, 1, 0},
    {"texel_bevel", "Texel Bevel", 4, 0, 2, 0.05f, 2},
    {"texel_bevel_width", "Texel Bevel Width", 4, 0.05f, 0.5f, 0.01f, 2},
    {"block_bevel", "Block Edge Bevel", 4, 0, 2, 0.05f, 2},
    {"block_bevel_width", "Block Bevel Width", 4, 0.02f, 0.5f, 0.01f, 2},
    {"texel_outline", "Texel Outline", 4, 0, 1, 0.05f, 2},
    {"block_outline", "Block Outline", 4, 0, 1, 0.05f, 2},
    {"albedo_contrast", "Colour Contrast", 1, 0.5f, 2, 0.01f, 2},
    {"saturation", "Saturation", 1, 0, 2, 0.01f, 2},
    {"luminance_relief", "Colour Relief", 0, 0, 2, 0.05f, 2},
};
_Static_assert(sizeof(PbrCfg) == PBR_FIELD_COUNT * sizeof(float), "PbrCfg must be one float per PBR_FIELDS entry");

static const PbrCfg PBR_HIGH = {{1, 1.0f, 0.0625f, 1.0f, 16, 32, 0.5f, 1.0f, 0.3f, 1, 0, 1, 1.0f, 1.0f, 0, 0.5f, 1.0f, 5, 4, 0.02f, 8, 0.0f, 0.4f, 0.0f, 0.12f, 0.0f, 0.0f, 1.1f, 1.08f, 0.9f}};
static const struct { const char *id, *name; PbrCfg cfg; } PBR_PRESETS[] = {
    {"off", "Off", {{0, 1.0f, 0.0625f, 1.0f, 16, 32, 0.5f, 1.0f, 0.3f, 1, 0, 1, 1.0f, 1.0f, 0, 0.5f, 1.0f, 5, 4, 0.02f, 8, 0.0f, 0.4f, 0.0f, 0.12f, 0.0f, 0.0f, 1.1f, 1.08f, 0.9f}}},
    {"low", "Low", {{1, 0.7f, 0.0625f, 0.9f, 8, 16, 0.3f, 0.7f, 0.05f, 1, 0, 1, 0.7f, 0.6f, 0, 0.0f, 1.0f, 0, 1, 0.0f, 0, 0.0f, 0.4f, 0.0f, 0.12f, 0.0f, 0.0f, 1.05f, 1.0f, 0.7f}}},
    {"medium", "Medium", {{1, 0.85f, 0.0625f, 1.0f, 12, 24, 0.4f, 0.8f, 0.15f, 1, 0, 1, 0.8f, 0.8f, 0, 0.3f, 0.8f, 3, 2, 0.02f, 6, 0.0f, 0.4f, 0.0f, 0.12f, 0.0f, 0.0f, 1.08f, 1.04f, 0.8f}}},
    {"high", "High", {{1, 1.0f, 0.0625f, 1.0f, 16, 32, 0.5f, 1.0f, 0.3f, 1, 0, 1, 1.0f, 1.0f, 0, 0.5f, 1.0f, 5, 4, 0.02f, 8, 0.0f, 0.4f, 0.0f, 0.12f, 0.0f, 0.0f, 1.1f, 1.08f, 0.9f}}},
    {"ultra", "Ultra", {{1, 1.15f, 0.07f, 1.1f, 24, 64, 0.55f, 1.2f, 0.4f, 1, 0, 1, 1.1f, 1.1f, 0.03f, 0.7f, 1.5f, 10, 8, 0.05f, 20, 0.0f, 0.4f, 0.0f, 0.14f, 0.0f, 0.0f, 1.12f, 1.1f, 1.0f}}},
};
#define PBR_PRESET_N ((int)(sizeof PBR_PRESETS / sizeof PBR_PRESETS[0]))

/* Texture styles scale groups of the resolved values, so a player picks a look without knowing the sliders. They apply
 * to the named quality presets and the data file, never to "custom", whose sliders are already the final values. */
typedef struct { const char *id, *name; float relief, parallax, bevel, outline, specular, shadow, grade; } PbrStyle;
static const PbrStyle PBR_STYLES[] = {
    {"natural", "Natural", 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
    {"clean", "Clean", 0.8f, 0.5f, 0.0f, 0.0f, 0.8f, 1.0f, 0.6f},
    {"crisp", "Crisp", 1.0f, 0.3f, 1.4f, 1.5f, 1.0f, 0.7f, 1.0f},
    {"realistic", "Realistic", 1.0f, 1.6f, 0.3f, 0.0f, 1.2f, 1.3f, 0.7f},
    {"chunky", "Chunky", 1.3f, 1.2f, 2.0f, 2.0f, 1.0f, 1.0f, 1.3f},
};
#define PBR_STYLE_N ((int)(sizeof PBR_STYLES / sizeof PBR_STYLES[0]))
int pbr_style_count(void) { return PBR_STYLE_N; }
const char *pbr_style_id(int i) { return i >= 0 && i < PBR_STYLE_N ? PBR_STYLES[i].id : ""; }
const char *pbr_style_name(int i) { return i >= 0 && i < PBR_STYLE_N ? PBR_STYLES[i].name : ""; }

static void pbr_apply_style(PbrCfg *c, const char *id) {
    const PbrStyle *st = &PBR_STYLES[0];
    for (int i = 0; i < PBR_STYLE_N; i++) if (!strcmp(id, PBR_STYLES[i].id)) st = &PBR_STYLES[i];
    c->f.bump_strength *= st->relief;
    c->f.normal_strength *= st->relief;
    c->f.luminance_relief *= st->relief;
    c->f.parallax_depth *= st->parallax;
    c->f.texel_bevel *= st->bevel;
    c->f.block_bevel *= st->bevel;
    c->f.texel_outline *= st->outline;
    c->f.block_outline *= st->outline;
    c->f.specular_strength *= st->specular;
    c->f.sky_specular *= st->specular;
    c->f.self_shadow_strength *= st->shadow;
    c->f.albedo_contrast = 1.0f + (c->f.albedo_contrast - 1.0f) * st->grade;
    c->f.saturation = 1.0f + (c->f.saturation - 1.0f) * st->grade;
}

PbrCfg g_pbr;
static PbrCfg g_pbr_data = {{1, 1.0f, 0.0625f, 1.0f, 16, 32, 0.5f, 1.0f, 0.3f, 1, 0, 1, 1.0f, 1.0f, 0, 0.5f, 1.0f, 5, 4, 0.02f, 8, 0.0f, 0.4f, 0.0f, 0.12f, 0.0f, 0.0f, 1.1f, 1.08f, 0.9f}};

int pbr_preset_count(void) { return PBR_PRESET_N; }
const char *pbr_preset_id(int i) { return i >= 0 && i < PBR_PRESET_N ? PBR_PRESETS[i].id : ""; }
const char *pbr_preset_name(int i) { return i >= 0 && i < PBR_PRESET_N ? PBR_PRESETS[i].name : ""; }

const PbrCfg *pbr_preset_cfg(int i) { return i >= 0 && i < PBR_PRESET_N ? &PBR_PRESETS[i].cfg : &PBR_HIGH; }

static void pbr_clamp(PbrCfg *c) {
    for (int i = 0; i < PBR_FIELD_COUNT; i++) c->v[i] = CLAMP(c->v[i], PBR_FIELDS[i].lo, PBR_FIELDS[i].hi);
    if (c->f.fade_end < c->f.fade_start + 1.0f) c->f.fade_end = c->f.fade_start + 1.0f;
}

/* The active configuration: custom values, a named preset, or the data file when the setting is empty. */
void pbr_resolve(void) {
    PbrCfg c = g_pbr_data;
    const char *q = g_settings.texture_quality;
    if (!strcmp(q, "custom")) c = g_settings.pbr;
    else {
        for (int i = 0; i < PBR_PRESET_N; i++) if (!strcmp(q, PBR_PRESETS[i].id)) c = PBR_PRESETS[i].cfg;
        pbr_apply_style(&c, g_settings.texture_style);
    }
    pbr_clamp(&c);
    g_pbr = c;
}

static void pbr_cfg_load(void) {
    PbrCfg c = PBR_HIGH;
    size_t size;
    const char *owner = "?";
    u8 *text = vfs_read("data/dfe/pbr.json", &size, &owner);
    if (text) {
        char err[200];
        int err_line = 0;
        Json *r = json_parse((const char *)text, size, err, sizeof err, &err_line);
        free(text);
        if (!r || r->type != JSON_OBJECT) {
            data_error(owner, "data/dfe/pbr.json", err_line, "%s. Fix the JSON syntax; built-in PBR values are used.", r ? "must be one JSON object" : err);
        } else {
            for (int i = 0; i < PBR_FIELD_COUNT; i++) c.v[i] = (float)json_num(r, PBR_FIELDS[i].key, c.v[i]);
        }
        json_free(r);
    }
    pbr_clamp(&c);
    g_pbr_data = c;
    pbr_resolve();
}

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
    Shader lod_shader[LAYER_COUNT];
    Shader shadow_shader[2]; /* opaque and cutout casters */
    Chunk **casters;
    int caster_n, caster_cap;
    bool ready;
    u32 frame;
    u64 resident_granules;
    /* Per-frame scratch. */
    Chunk **visible;
    int visible_n, visible_cap;
    struct QItem *vis_queue;
    size_t vis_queue_cap;
    u32 *cell_stamp;
    Chunk **cell_chunk;
    u8 *cell_entry;
    size_t cell_cap;
    const MeshSlot **slot_list;
    int slot_cap;
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

void scene_release_slots(MeshSlot slots[LAYER_COUNT]) {
    if (!S.ready) return;
    for (int l = 0; l < LAYER_COUNT; l++) slot_release(&slots[l]);
}

void scene_free_chunk(Chunk *c) {
    scene_release_slots(c->mesh);
    c->flags &= ~CF_HAS_MESH;
}

void scene_upload_slots(MeshSlot slots[LAYER_COUNT], MeshOutput *out, const i32 origin[4]) {
    if (!S.ready) return;
    for (int l = 0; l < LAYER_COUNT; l++) {
        MeshSlot old = slots[l];
        MeshSlot fresh = {.page = -1};
        u32 n = out->count[l];
        if (n) {
            u32 granules = (n + MESH_GRANULE - 1) / MESH_GRANULE;
            if (arena_alloc(granules, &fresh)) {
                fresh.count = n;
                Page *p = &S.pages[fresh.page];
                glBindBuffer(GL_ARRAY_BUFFER, p->vbo);
                glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)fresh.first * sizeof(MeshVertex), (GLsizeiptr)n * sizeof(MeshVertex), out->verts[l]);
                glBindBuffer(GL_TEXTURE_BUFFER, p->origin_buf);
                i32 *origins = xmalloc((size_t)granules * 4 * sizeof(i32));
                for (u32 g = 0; g < granules; g++) memcpy(origins + (size_t)g * 4, origin, 4 * sizeof(i32));
                glBufferSubData(GL_TEXTURE_BUFFER, (GLintptr)(fresh.first / MESH_GRANULE * 4 * sizeof(i32)),
                                (GLsizeiptr)granules * 4 * sizeof(i32), origins);
                free(origins);
                g_scene_stats.upload_bytes_total += (u64)n * sizeof(MeshVertex);
            }
        }
        /* The old mesh is released only after the new one is resident, so nothing flickers empty. */
        slot_release(&old);
        slots[l] = fresh;
    }
    g_scene_stats.uploads_this_frame++;
}

void scene_upload_mesh(Chunk *c, MeshOutput *out) {
    i32 origin[4] = {c->cx * CHUNK_SIZE, c->cy * CHUNK_SIZE, c->cz * CHUNK_SIZE, 0};
    scene_upload_slots(c->mesh, out, origin);
    c->flags |= CF_HAS_MESH;
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

static bool load_shaders(Shader out[LAYER_COUNT], bool lod) {
    static const char *const defs[LAYER_COUNT] = {"#define PASS_OPAQUE 1\n", "#define PASS_CUTOUT 1\n", "#define PASS_TRANSLUCENT 1\n"};
    static const char *const names[LAYER_COUNT] = {"chunk_opaque", "chunk_cutout", "chunk_translucent"};
    static const char *const lod_names[LAYER_COUNT] = {"lod_opaque", "lod_cutout", "lod_translucent"};
    for (int l = 0; l < LAYER_COUNT; l++) {
        char d[96];
        snprintf(d, sizeof d, "%s%s", defs[l], lod ? "#define LOD 1\n" : "");
        if (!shader_load(&out[l], lod ? lod_names[l] : names[l], "assets/dfe/shaders/chunk.vert", "assets/dfe/shaders/chunk.frag", d)) return false;
    }
    return true;
}

static bool load_shadow_shaders(Shader out[2]) {
    static const char *const defs[2] = {"#define SHADOW 1\n#define PASS_OPAQUE 1\n", "#define SHADOW 1\n#define PASS_CUTOUT 1\n"};
    static const char *const names[2] = {"shadow_opaque", "shadow_cutout"};
    for (int l = 0; l < 2; l++)
        if (!shader_load(&out[l], names[l], "assets/dfe/shaders/chunk.vert", "assets/dfe/shaders/chunk.frag", defs[l])) return false;
    return true;
}

bool scene_init(void) {
    memset(&S, 0, sizeof S);
    if (!build_index_buffer()) return false;
    if (!load_shaders(S.shader, false) || !load_shaders(S.lod_shader, true) || !load_shadow_shaders(S.shadow_shader)) return false;
    if (!shadow_gl_init()) return false;
    page_init(&S.pages[S.page_count++]);
    if (!lod_init()) return false;
    if (!atmosphere_gl_init()) return false;
    pbr_cfg_load();
    S.ready = true;
    return true;
}

bool scene_reload_shaders(void) {
    pbr_cfg_load();
    Shader fresh[2][LAYER_COUNT], fresh_shadow[2];
    memset(fresh, 0, sizeof fresh);
    memset(fresh_shadow, 0, sizeof fresh_shadow);
    if (!load_shaders(fresh[0], false) || !load_shaders(fresh[1], true) || !load_shadow_shaders(fresh_shadow)) {
        for (int v = 0; v < 2; v++)
            for (int l = 0; l < LAYER_COUNT; l++) if (fresh[v][l].program) shader_destroy(&fresh[v][l]);
        for (int l = 0; l < 2; l++) if (fresh_shadow[l].program) shader_destroy(&fresh_shadow[l]);
        return false;
    }
    for (int l = 0; l < 2; l++) { shader_destroy(&S.shadow_shader[l]); S.shadow_shader[l] = fresh_shadow[l]; }
    for (int l = 0; l < LAYER_COUNT; l++) {
        shader_destroy(&S.shader[l]); S.shader[l] = fresh[0][l];
        shader_destroy(&S.lod_shader[l]); S.lod_shader[l] = fresh[1][l];
    }
    return true;
}

void scene_shutdown(void) {
    if (!S.ready) return;
    for (int i = 0; i < S.page_count; i++) page_destroy(&S.pages[i]);
    for (int l = 0; l < LAYER_COUNT; l++) { shader_destroy(&S.shader[l]); shader_destroy(&S.lod_shader[l]); }
    for (int l = 0; l < 2; l++) shader_destroy(&S.shadow_shader[l]);
    shadow_gl_shutdown();
    lod_shutdown();
    atmosphere_gl_shutdown();
    glDeleteBuffers(1, &S.ibo);
    free(S.visible); free(S.vis_queue); free(S.casters); free(S.cell_stamp); free(S.cell_chunk); free(S.cell_entry);
    free(S.slot_list); free(S.draw_count); free(S.draw_index); free(S.draw_base);
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
    if (S.vis_queue_cap < cells) {
        S.vis_queue_cap = cells;
        S.vis_queue = xrealloc(S.vis_queue, cells * sizeof(QItem));
    }
    QItem *queue = S.vis_queue;
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
    g_scene_stats.chunks_in_range = in_range;
    g_scene_stats.chunks_culled_frustum = culled_frustum;
    g_scene_stats.chunks_visible = S.visible_n;
}

/* -------------------------------------------------------------------- draw */

static void ensure_slot_list(int n) {
    if (n <= S.slot_cap) return;
    S.slot_cap = n * 2;
    S.slot_list = xrealloc(S.slot_list, (size_t)S.slot_cap * sizeof(*S.slot_list));
}

static void ensure_draws(int n) {
    if (n <= S.draw_cap) return;
    S.draw_cap = n * 2;
    S.draw_count = xrealloc(S.draw_count, (size_t)S.draw_cap * sizeof(GLsizei));
    S.draw_index = xrealloc(S.draw_index, (size_t)S.draw_cap * sizeof(void *));
    S.draw_base = xrealloc(S.draw_base, (size_t)S.draw_cap * sizeof(GLint));
}

/* Draws a list of mesh slots of one layer, one multi-draw per arena page. */
static void draw_slots(const MeshSlot *const *list, int count, int layer, bool reverse) {
    for (int pg = 0; pg < S.page_count; pg++) {
        int n = 0;
        for (int k = 0; k < count; k++) {
            const MeshSlot *m = list[reverse ? count - 1 - k : k];
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

static void draw_layer(int layer, bool reverse) {
    ensure_slot_list(S.visible_n);
    int n = 0;
    for (int k = 0; k < S.visible_n; k++) S.slot_list[n++] = &S.visible[k]->mesh[layer];
    draw_slots(S.slot_list, n, layer, reverse);
}

static void set_pass_uniforms(Shader *sh, const Camera *cam, double time_s, int rd);
static void draw_lod_layer(const Camera *cam, double time_s, int rd, int layer, bool reverse) {
    int n;
    const MeshSlot *const *list = lod_draw_list(layer, &n);
    if (!n) return;
    set_pass_uniforms(&S.lod_shader[layer], cam, time_s, rd);
    lod_set_uniforms(&S.lod_shader[layer]);
    draw_slots(list, n, layer, reverse);
}

/* Fog reaches full density just inside the last drawn geometry, whichever layer that is, so edges never show. */
static void fog_range(int rd, float *start, float *end) {
    float e = (float)(rd + g_scene_cfg.far_chunks) * 32.0f - 12.0f;
    *end = e;
    *start = e * (g_scene_cfg.far_chunks > 0 ? 0.35f : 0.55f);
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
    static const float tints[12] = {1, 1, 1, 0.55f, 0.78f, 0.35f, 0.42f, 0.66f, 0.30f, 0.16f, 0.38f, 0.58f};
    glUniform3fv(shader_uniform(sh, "u_tint"), 4, tints);
    glUniform3f(shader_uniform(sh, "u_water_deep"), 0.11f, 0.31f, 0.52f);
    glUniform1i(shader_uniform(sh, "u_origins"), 1);
    glUniform1i(shader_uniform(sh, "u_tex"), 0);
    glUniform1i(shader_uniform(sh, "u_anim"), 2);
    /* PBR maps; a shader pack that does not declare them gets -1 locations, which GL ignores. */
    glUniform1i(shader_uniform(sh, "u_tex_normal"), 4);
    glUniform1i(shader_uniform(sh, "u_tex_rh"), 5);
    glUniform1f(shader_uniform(sh, "u_bump_strength"), g_pbr.f.bump_strength);
    glUniform4f(shader_uniform(sh, "u_pbr_a"), g_pbr.f.height_depth, g_pbr.f.normal_strength, g_pbr.f.fade_start, g_pbr.f.fade_end);
    glUniform4f(shader_uniform(sh, "u_pbr_b"), g_pbr.f.specular_strength, g_pbr.f.roughness_scale, g_pbr.f.roughness_bias, g_pbr.f.metalness_scale);
    glUniform4f(shader_uniform(sh, "u_pbr_c"), g_pbr.f.diffuse_response, g_pbr.f.sky_lean, g_pbr.f.cavity_ao, g_pbr.f.shade_floor);
    glUniform4f(shader_uniform(sh, "u_pbr_d"), g_pbr.f.self_shadow_strength, g_pbr.f.self_shadow_reach, g_pbr.f.sky_specular, (float)g_pbr.f.shadow_tap_cap);
    glUniform4f(shader_uniform(sh, "u_pbr_e"), g_pbr.f.parallax_depth, g_pbr.f.texel_bevel, g_pbr.f.texel_bevel_width, g_pbr.f.block_bevel);
    glUniform4f(shader_uniform(sh, "u_pbr_f"), g_pbr.f.block_bevel_width, g_pbr.f.texel_outline, g_pbr.f.block_outline, g_pbr.f.luminance_relief);
    glUniform2f(shader_uniform(sh, "u_pbr_g"), g_pbr.f.albedo_contrast, g_pbr.f.saturation);
    glUniform1i(shader_uniform(sh, "u_pbr_pom"), (int)g_pbr.f.parallax_steps);
    glUniform1i(shader_uniform(sh, "u_pbr_on"), g_pbr.f.enabled > 0.5f);
    glUniform1i(shader_uniform(sh, "u_pbr_steps"), (int)g_pbr.f.self_shadow_steps);
    atmosphere_set_uniforms(sh);
    float fog_start, fog_end;
    fog_range(rd, &fog_start, &fog_end);
    shadow_set_uniforms(sh);
    atmosphere_adjust_fog(&fog_start, &fog_end);
    glUniform1f(shader_uniform(sh, "u_fog_start"), fog_start);
    glUniform1f(shader_uniform(sh, "u_fog_end"), fog_end);
    float near_start = 4.0f, near_end = 28.0f, near_density = 0.0f;
    if (g_gfx.fog) {
        const FogLevel *l = &g_gfx.fog_level;
        near_start = l->near_start;
        near_end = l->near_end;
        float daylight = 0.55f + 0.45f * g_atmo.sun_vis + 0.20f * g_atmo.moon_vis;
        float shade = 0.70f + 0.60f * (1.0f - CLAMP(g_atmo.shade_strength, 0.0f, 1.0f));
        float shafts = 1.0f + (g_gfx.light_shafts ? 0.45f * g_gfx.godray.strength : 0.0f);
        near_density = l->density * daylight * shade * shafts * l->sun_boost;
    }
    glUniform1f(shader_uniform(sh, "u_near_fog_density"), near_density);
    glUniform1f(shader_uniform(sh, "u_near_fog_start"), near_start);
    glUniform1f(shader_uniform(sh, "u_near_fog_end"), near_end);
}

/* Draws every meshed chunk near the camera into the shadow cascades, from the sun's point of view. Casters are not
 * limited to the camera's view: a tree behind the player still shades the ground in front of them. */
static void render_shadows(const Camera *cam, double time_s) {
    int cascades = shadow_prepare(cam, g_atmo.shade_dir);
    if (!cascades) return;
    perf_gpu_begin(GPU_OPAQUE);
    S.caster_n = 0;
    int lo, hi;
    gen_band(&lo, &hi);
    int reach = (int)ceilf((shadow_cascade_radius(cascades - 1) * 1.6f + 32.0f) / 32.0f);
    int ccx = ifloor(cam->pos.x / 32.0f), ccz = ifloor(cam->pos.z / 32.0f);
    for (int cz = ccz - reach; cz <= ccz + reach; cz++)
        for (int cx = ccx - reach; cx <= ccx + reach; cx++)
            for (int cy = lo - 1; cy <= hi + VIS_VERTICAL_ABOVE; cy++) {
                Chunk *c = world_chunk(cx, cy, cz);
                if (!c || !(c->flags & CF_HAS_MESH)) continue;
                if (S.caster_n == S.caster_cap) {
                    S.caster_cap = S.caster_cap ? S.caster_cap * 2 : 1024;
                    S.casters = xrealloc(S.casters, (size_t)S.caster_cap * sizeof(Chunk *));
                }
                S.casters[S.caster_n++] = c;
            }
    /* The shadow draws are not part of what the player sees, so they stay out of the statistics. */
    SceneStats saved = g_scene_stats;
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(2.0f, 4.0f);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    for (int i = 0; i < cascades; i++) {
        M4 vp = shadow_begin_cascade(i);
        float limit = shadow_cascade_radius(i) * 1.6f + 48.0f;
        for (int l = 0; l < 2; l++) {
            Shader *sh = &S.shadow_shader[l];
            shader_use(sh);
            int bx = ifloor(cam->pos.x), by = ifloor(cam->pos.y), bz = ifloor(cam->pos.z);
            glUniform3i(shader_uniform(sh, "u_cam_base"), bx, by, bz);
            glUniform3f(shader_uniform(sh, "u_cam_frac"), cam->pos.x - (float)bx, cam->pos.y - (float)by, cam->pos.z - (float)bz);
            glUniformMatrix4fv(shader_uniform(sh, "u_viewproj"), 1, GL_FALSE, vp.m);
            glUniform1f(shader_uniform(sh, "u_time"), (float)time_s);
            glUniform1i(shader_uniform(sh, "u_origins"), 1);
            glUniform1i(shader_uniform(sh, "u_tex"), 0);
            glUniform1i(shader_uniform(sh, "u_anim"), 2);
            glUniform1i(shader_uniform(sh, "u_shadow_map"), 3);
            glUniform1i(shader_uniform(sh, "u_shadow_count"), 0);
            ensure_slot_list(S.caster_n);
            int n = 0;
            for (int k = 0; k < S.caster_n; k++) {
                Chunk *c = S.casters[k];
                float dx = ((float)c->cx + 0.5f) * 32.0f - cam->pos.x, dz = ((float)c->cz + 0.5f) * 32.0f - cam->pos.z;
                if (fabsf(dx) > limit || fabsf(dz) > limit) continue;
                S.slot_list[n++] = &c->mesh[l == 0 ? LAYER_OPAQUE : LAYER_CUTOUT];
            }
            draw_slots(S.slot_list, n, l, false);
        }
    }
    shadow_end();
    glDisable(GL_POLYGON_OFFSET_FILL);
    glEnable(GL_CULL_FACE);
    g_scene_stats = saved;
    perf_gpu_end();
}

void scene_render(const Camera *cam, double time_s) {
    if (!S.ready) return;
    int rd = g_scene_cfg.render_distance;
    g_scene_stats.draw_calls = 0;
    g_scene_stats.vertices_drawn = 0;
    memset(g_scene_stats.chunks_drawn, 0, sizeof g_scene_stats.chunks_drawn);
    walk_visibility(cam, rd);
    lod_update(cam, rd, g_scene_cfg.far_chunks);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, g_tex.gl_array);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, g_tex.gl_anim);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D_ARRAY, g_tex.gl_normal);
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D_ARRAY, g_tex.gl_rh);
    render_shadows(cam, time_s);
    glActiveTexture(GL_TEXTURE0);
    glPolygonMode(GL_FRONT_AND_BACK, g_scene_cfg.wireframe ? GL_LINE : GL_FILL);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    for (int l = 0; l < LAYER_COUNT; l++) {
        bool translucent = l == LAYER_TRANSLUCENT;
        if (translucent) {
            /* The sky goes in before blended surfaces so water over the horizon blends with it, and after the
             * opaque ones so the depth test skips every pixel the terrain covers. */
            perf_gpu_begin(GPU_ENTITY);
            float fog_start, fog_end;
            fog_range(rd, &fog_start, &fog_end);
            atmosphere_adjust_fog(&fog_start, &fog_end);
            g_scene_stats.draw_calls += entity_draw(cam, fog_start, fog_end);
            perf_gpu_end();
            perf_gpu_begin(GPU_SKY);
            atmosphere_draw_sky(cam, time_s);
            perf_gpu_end();
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
        }
        /* Opaque surfaces go near to far so the depth test rejects hidden distant fragments early; blended ones
         * go far to near, and the distant tiles are all behind the real chunks. */
        perf_gpu_begin(l == LAYER_OPAQUE ? GPU_OPAQUE : l == LAYER_CUTOUT ? GPU_CUTOUT : GPU_WATER);
        if (translucent) draw_lod_layer(cam, time_s, rd, l, true);
        set_pass_uniforms(&S.shader[l], cam, time_s, rd);
        draw_layer(l, translucent);
        if (!translucent) draw_lod_layer(cam, time_s, rd, l, false);
        perf_gpu_end();
        if (translucent) {
            glDisable(GL_BLEND);
            glDepthMask(GL_TRUE);
        }
    }
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    perf_gpu_begin(GPU_RAIN);
    atmosphere_draw_rain(cam, time_s);
    perf_gpu_end();
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
    g_scene_stats.arena_pages = S.page_count;
    g_scene_stats.arena_used_mb = (double)S.resident_granules * MESH_GRANULE * sizeof(MeshVertex) / 1048576.0;
}
