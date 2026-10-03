/* Godray (light shaft) quality levels. A level is data (data/<namespace>/godrays/<id>.json) and the march itself is plain
 * GLSL in assets/dfe/shaders/post.frag, which receives the level's numbers as uniforms; a mod can add levels, retune
 * them, or replace the shader. post.c does the drawing. */
#include "dfe.h"

static GodrayLevel g_levels[MAX_GODRAY_LEVELS];
static int g_level_count;

int godray_level_count(void) { return g_level_count; }
const GodrayLevel *godray_level_at(int i) { return i >= 0 && i < g_level_count ? &g_levels[i] : NULL; }

const GodrayLevel *godray_level_find(const char *id) {
    for (int i = 0; i < g_level_count; i++) if (!strcmp(g_levels[i].id, id)) return &g_levels[i];
    return NULL;
}

static bool read_level(GodrayLevel *l, const char *stem, const char *rel, const char *owner, const Json *root) {
    memset(l, 0, sizeof *l);
    snprintf(l->id, sizeof l->id, "%s", stem);
    snprintf(l->name, sizeof l->name, "%s", json_str(root, "name", stem));
    l->order = json_int(root, "order", 0);
    l->taps = json_int(root, "taps", 14);
    l->divisor = json_int(root, "divisor", 2);
    l->density = (float)json_num(root, "density", 0.85);
    l->decay = (float)json_num(root, "decay", 0.93);
    l->strength = (float)json_num(root, "strength", 0.30);
    l->jitter = (float)json_num(root, "jitter", 1.0);
    int errors = 0;
    if (l->taps < 2 || l->taps > 64) { data_error(owner, rel, root->line, "taps must be 2 to 64."); errors++; }
    if (l->divisor < 1 || l->divisor > 4) { data_error(owner, rel, root->line, "divisor must be 1 to 4: the march runs at 1/divisor of the window size."); errors++; }
    if (l->density < 0.1f || l->density > 1.0f) { data_error(owner, rel, root->line, "density must be 0.1 to 1."); errors++; }
    if (l->decay < 0.5f || l->decay > 1.0f) { data_error(owner, rel, root->line, "decay must be 0.5 to 1."); errors++; }
    if (l->strength < 0.0f || l->strength > 2.0f) { data_error(owner, rel, root->line, "strength must be 0 to 2."); errors++; }
    if (l->jitter < 0.0f || l->jitter > 1.0f) { data_error(owner, rel, root->line, "jitter must be 0 to 1."); errors++; }
    return errors == 0;
}

static void add_level(const GodrayLevel *l) {
    for (int i = 0; i < g_level_count; i++)
        if (!strcmp(g_levels[i].id, l->id)) { g_levels[i] = *l; return; }
    if (g_level_count < MAX_GODRAY_LEVELS) g_levels[g_level_count++] = *l;
    else LOGW("godray level '%s' ignored: at most %d levels are supported", l->id, MAX_GODRAY_LEVELS);
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
    GodrayLevel l;
    if (root->type != JSON_OBJECT) data_error(owner, rel, 1, "a godray level file must contain one JSON object like {\"resolution\": 2048, \"cascades\": 3, ...}");
    else if (read_level(&l, stem, rel, owner, root)) add_level(&l);
    json_free(root);
}

static int level_order_cmp(const void *a, const void *b) {
    const GodrayLevel *x = a, *y = b;
    return x->order != y->order ? x->order - y->order : strcmp(x->id, y->id);
}

int registry_load_godray_levels(void) {
    int errors_before = data_error_count();
    GodrayLevel previous[MAX_GODRAY_LEVELS];
    int previous_count = g_level_count;
    memcpy(previous, g_levels, sizeof previous);
    g_level_count = 0;
    StrList namespaces = {0};
    vfs_list("data", &namespaces);
    for (int n = 0; n < namespaces.n; n++) {
        char dir[160];
        snprintf(dir, sizeof dir, "data/%s/godrays", namespaces.d[n]);
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
