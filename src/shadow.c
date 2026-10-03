/* Cascaded sun shadows.
 *
 * One depth texture array holds a few orthographic maps of increasing size centred near the camera, so shadows are
 * sharp close up and still reach far. Quality levels are data (data/<namespace>/shadows/*.json) and the lookup that
 * turns the maps into light is plain GLSL (assets/dfe/shaders/shadow.glsl), so a mod can add levels or change how
 * shadows look without touching the engine. Rejected: baked shadows, which cannot follow a moving sun or edited
 * terrain, and a perspective-fitted map, which shimmers as the camera turns. Each map is snapped to whole texels
 * in light space so shadow edges stay still while the camera moves. */
#include "dfe.h"

#define SHADOW_MIN_SUN_Y 0.05f      /* below this the light is nearly level and shadows would stretch for ever */
#define SHADOW_FADE_SUN_Y 0.30f
#define SHADOW_BASE_STRENGTH 0.62f
#define SHADOW_TOWARD_LIGHT 256.0f  /* casters this far towards the sun still reach the map, for tall peaks */
#define SHADOW_AWAY 128.0f
#define SHADOW_FORWARD_SHIFT 0.4f   /* share of the radius a map is moved ahead of the camera */

static ShadowLevel g_levels[MAX_SHADOW_LEVELS];
static int g_level_count;

int shadow_level_count(void) { return g_level_count; }
const ShadowLevel *shadow_level_at(int i) { return i >= 0 && i < g_level_count ? &g_levels[i] : NULL; }

const ShadowLevel *shadow_level_find(const char *id) {
    for (int i = 0; i < g_level_count; i++) if (!strcmp(g_levels[i].id, id)) return &g_levels[i];
    return NULL;
}

/* ------------------------------------------------------------------- data */

static bool read_level(ShadowLevel *l, const char *stem, const char *rel, const char *owner, const Json *root) {
    memset(l, 0, sizeof *l);
    snprintf(l->id, sizeof l->id, "%s", stem);
    snprintf(l->name, sizeof l->name, "%s", json_str(root, "name", stem));
    l->order = json_int(root, "order", 0);
    l->resolution = json_int(root, "resolution", 2048);
    l->cascades = json_int(root, "cascades", 3);
    l->taps = json_int(root, "taps", 5);
    l->distance = (float)json_num(root, "distance", 160.0);
    l->softness = (float)json_num(root, "softness", 1.5);
    l->bias = (float)json_num(root, "bias", 0.12);
    int errors = 0;
    if (l->resolution < 256 || l->resolution > 8192) { data_error(owner, rel, root->line, "resolution must be 256 to 8192 texels."); errors++; }
    if (l->cascades < 1 || l->cascades > MAX_CASCADES) { data_error(owner, rel, root->line, "cascades must be 1 to %d.", MAX_CASCADES); errors++; }
    if (l->taps < 1 || l->taps > 16) { data_error(owner, rel, root->line, "taps must be 1 to 16."); errors++; }
    if (l->distance < 16.0f || l->distance > 1024.0f) { data_error(owner, rel, root->line, "distance must be 16 to 1024 blocks."); errors++; }
    if (l->softness < 0.0f || l->softness > 8.0f) { data_error(owner, rel, root->line, "softness must be 0 to 8 texels."); errors++; }
    return errors == 0;
}

static void add_level(const ShadowLevel *l) {
    for (int i = 0; i < g_level_count; i++)
        if (!strcmp(g_levels[i].id, l->id)) { g_levels[i] = *l; return; }
    if (g_level_count < MAX_SHADOW_LEVELS) g_levels[g_level_count++] = *l;
    else LOGW("shadow level '%s' ignored: at most %d levels are supported", l->id, MAX_SHADOW_LEVELS);
}

static void load_level_file(const char *rel, const char *stem) {
    size_t size;
    const char *owner = "?";
    u8 *text = vfs_read(rel, &size, &owner);
    if (!text) return;
    char err[200];
    int err_line = 0;
    Json *root = json_parse((const char *)text, size, err, sizeof err, &err_line);
    free(text);
    if (!root) { data_error(owner, rel, err_line, "%s. Fix the JSON syntax at that line.", err); return; }
    ShadowLevel l;
    if (root->type != JSON_OBJECT) data_error(owner, rel, 1, "a shadow level file must contain one JSON object like {\"resolution\": 2048, \"cascades\": 3, ...}");
    else if (read_level(&l, stem, rel, owner, root)) add_level(&l);
    json_free(root);
}

static int level_order_cmp(const void *a, const void *b) {
    const ShadowLevel *x = a, *y = b;
    return x->order != y->order ? x->order - y->order : strcmp(x->id, y->id);
}

int registry_load_shadow_levels(void) {
    int errors_before = data_error_count();
    ShadowLevel previous[MAX_SHADOW_LEVELS];
    int previous_count = g_level_count;
    memcpy(previous, g_levels, sizeof previous);
    g_level_count = 0;
    StrList namespaces = {0};
    vfs_list("data", &namespaces);
    for (int n = 0; n < namespaces.n; n++) {
        char dir[160];
        snprintf(dir, sizeof dir, "data/%s/shadows", namespaces.d[n]);
        StrList files = {0};
        vfs_list(dir, &files);
        for (int f = 0; f < files.n; f++) {
            size_t len = strlen(files.d[f]);
            if (len < 6 || strcmp(files.d[f] + len - 5, ".json")) continue;
            char rel[260], stem[PRESET_ID_MAX];
            snprintf(rel, sizeof rel, "%s/%s", dir, files.d[f]);
            snprintf(stem, sizeof stem, "%.*s", (int)MIN(len - 5, sizeof stem - 1), files.d[f]);
            load_level_file(rel, stem);
        }
        strlist_free(&files);
    }
    strlist_free(&namespaces);
    qsort(g_levels, (size_t)g_level_count, sizeof g_levels[0], level_order_cmp);
    int errors = data_error_count() - errors_before;
    if (errors && previous_count) {
        memcpy(g_levels, previous, sizeof previous);
        g_level_count = previous_count;
    }
    return errors;
}

/* --------------------------------------------------------------- GL state */

static struct {
    GLuint tex, fbo[MAX_CASCADES], dummy;
    int res, layers;
    int count;
    float radius[MAX_CASCADES], texel[MAX_CASCADES], depth_span[MAX_CASCADES];
    M4 vp[MAX_CASCADES];
    float strength;
    GLint saved_fbo, saved_viewport[4];
    bool ready;
} H;

static void release_targets(void) {
    if (H.fbo[0]) glDeleteFramebuffers(MAX_CASCADES, H.fbo);
    if (H.tex) glDeleteTextures(1, &H.tex);
    memset(H.fbo, 0, sizeof H.fbo);
    H.tex = 0;
    H.res = H.layers = 0;
}

static void depth_array_params(void) {
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    const float border[4] = {1, 1, 1, 1}; /* outside a map counts as lit */
    glTexParameterfv(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BORDER_COLOR, border);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
}

static bool ensure_targets(int res, int layers) {
    if (H.tex && H.res == res && H.layers == layers) return true;
    release_targets();
    glGenTextures(1, &H.tex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, H.tex);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT24, res, res, layers, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
    depth_array_params();
    glGenFramebuffers(MAX_CASCADES, H.fbo);
    bool ok = true;
    for (int i = 0; i < layers; i++) {
        glBindFramebuffer(GL_FRAMEBUFFER, H.fbo[i]);
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, H.tex, 0, i);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) ok = false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!ok) {
        LOGE("Shadow map of %dx%d x%d could not be created; shadows are off. Try a lower shadow quality.", res, res, layers);
        release_targets();
        return false;
    }
    H.res = res;
    H.layers = layers;
    return true;
}

/* The sampler must always point at a valid depth array, even with shadows off, or the driver rejects the draw
 * because it sees two sampler types on one unit. */
bool shadow_gl_init(void) {
    glGenTextures(1, &H.dummy);
    glBindTexture(GL_TEXTURE_2D_ARRAY, H.dummy);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT24, 1, 1, 1, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
    depth_array_params();
    H.ready = true;
    return true;
}

void shadow_gl_shutdown(void) {
    release_targets();
    if (H.dummy) glDeleteTextures(1, &H.dummy);
    memset(&H, 0, sizeof H);
}

/* ---------------------------------------------------------------- cascades */

static const float CASCADE_SHARE[MAX_CASCADES][MAX_CASCADES] = {{1.0f, 0, 0}, {0.30f, 1.0f, 0}, {0.12f, 0.38f, 1.0f}};

float shadow_cascade_radius(int i) { return H.radius[CLAMP(i, 0, MAX_CASCADES - 1)]; }

int shadow_prepare(const Camera *cam, V3 L) {
    H.count = 0;
    H.strength = 0.0f;
    if (!H.ready || !g_gfx.shadows || L.y < SHADOW_MIN_SUN_Y) return 0;
    const ShadowLevel *lv = &g_gfx.shadow;
    if (!ensure_targets(lv->resolution, lv->cascades)) { g_gfx.shadows = false; return 0; }
    float fade = CLAMP((L.y - SHADOW_MIN_SUN_Y) / (SHADOW_FADE_SUN_Y - SHADOW_MIN_SUN_Y), 0.0f, 1.0f);
    H.strength = SHADOW_BASE_STRENGTH * fade * CLAMP(g_atmo.shade_strength, 0.0f, 1.0f);
    V3 fwd = v3_norm(v3(-L.x, -L.y, -L.z)); /* direction the light travels */
    V3 up = fabsf(L.y) > 0.99f ? v3(0, 0, 1) : v3(0, 1, 0);
    V3 right = v3_norm(v3_cross(fwd, up));
    V3 up_ls = v3_cross(right, fwd);
    M4 view = m4_look_dir(v3(0, 0, 0), fwd, up);
    V3 flat = v3(cam->forward.x, 0, cam->forward.z);
    flat = v3_len(flat) > 1e-3f ? v3_norm(flat) : v3(0, 0, 0);
    for (int i = 0; i < lv->cascades; i++) {
        float r = lv->distance * CASCADE_SHARE[lv->cascades - 1][i];
        float texel = 2.0f * r / (float)lv->resolution;
        V3 shift = v3(flat.x * r * SHADOW_FORWARD_SHIFT, 0, flat.z * r * SHADOW_FORWARD_SHIFT);
        /* Snap the map's position in light space to whole texels, using doubles so far from the origin still works. */
        double ax = (double)cam->pos.x * right.x + (double)cam->pos.y * right.y + (double)cam->pos.z * right.z;
        double ay = (double)cam->pos.x * up_ls.x + (double)cam->pos.y * up_ls.y + (double)cam->pos.z * up_ls.z;
        float cx = v3_dot(shift, right), cy = v3_dot(shift, up_ls);
        double sx = floor((ax + cx) / texel) * texel - (ax + cx), sy = floor((ay + cy) / texel) * texel - (ay + cy);
        float ox = cx + (float)sx, oy = cy + (float)sy;
        float near_z = -(r + SHADOW_TOWARD_LIGHT), far_z = r + SHADOW_AWAY;
        M4 proj = m4_ortho(ox - r, ox + r, oy - r, oy + r, near_z, far_z);
        H.vp[i] = m4_mul(proj, view);
        H.radius[i] = r;
        H.texel[i] = texel;
        H.depth_span[i] = far_z - near_z;
    }
    H.count = lv->cascades;
    return H.count;
}

M4 shadow_begin_cascade(int i) {
    if (i == 0) {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &H.saved_fbo);
        glGetIntegerv(GL_VIEWPORT, H.saved_viewport);
    }
    /* Keep the map out of every sampler while it is being written, or the draw would read and write one texture. */
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D_ARRAY, H.dummy);
    glActiveTexture(GL_TEXTURE0);
    glBindFramebuffer(GL_FRAMEBUFFER, H.fbo[i]);
    glViewport(0, 0, H.res, H.res);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glClear(GL_DEPTH_BUFFER_BIT);
    return H.vp[i];
}

void shadow_end(void) {
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)H.saved_fbo);
    glViewport(H.saved_viewport[0], H.saved_viewport[1], H.saved_viewport[2], H.saved_viewport[3]);
}

void shadow_set_uniforms(Shader *sh) {
    int n = H.count;
    glUniform1i(shader_uniform(sh, "u_shadow_count"), n);
    glUniform1i(shader_uniform(sh, "u_shadow_map"), 3);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D_ARRAY, n ? H.tex : H.dummy);
    glActiveTexture(GL_TEXTURE0);
    if (!n) return;
    float vp[MAX_CASCADES * 16] = {0}, info[MAX_CASCADES * 4] = {0};
    for (int i = 0; i < n; i++) {
        memcpy(vp + i * 16, H.vp[i].m, sizeof H.vp[i].m);
        info[i * 4] = H.texel[i];
        info[i * 4 + 1] = H.depth_span[i];
    }
    glUniformMatrix4fv(shader_uniform(sh, "u_shadow_vp"), MAX_CASCADES, GL_FALSE, vp);
    glUniform4fv(shader_uniform(sh, "u_shadow_info"), MAX_CASCADES, info);
    glUniform1i(shader_uniform(sh, "u_shadow_taps"), g_gfx.shadow.taps);
    glUniform1f(shader_uniform(sh, "u_shadow_strength"), H.strength);
    glUniform1f(shader_uniform(sh, "u_shadow_softness"), g_gfx.shadow.softness);
    glUniform1f(shader_uniform(sh, "u_shadow_bias"), g_gfx.shadow.bias);
}
