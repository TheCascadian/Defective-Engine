/* Quality presets (data) and player settings (file), combined into the configuration the renderer reads.
 *
 * Presets are data so a mod can add or retune them. The player's choices live in settings.json next to the saves
 * folder, and every setting has an automatic value that follows the preset, so changing the preset alone is enough
 * for most players. Rejected: baking presets into the executable, which would leave mods unable to target a
 * machine class. */
#include "dfe.h"

#define SETTINGS_FILE "settings.json"
#define MIN_RENDER_DISTANCE 2
#define MAX_RENDER_DISTANCE 32
#define MIN_FOV 50.0f
#define MAX_FOV 110.0f
#define DEFAULT_FOV 75.0f
#define MIN_SCALE_FLOOR 0.4f
#define DEFAULT_MIN_SCALE 0.6f
#define DEFAULT_TARGET_FPS 60.0f
#define DEFAULT_PRESET "low"

Settings g_settings;
GraphicsConfig g_gfx;

static Preset g_presets[MAX_PRESETS];
static int g_preset_count;

int preset_count(void) { return g_preset_count; }
const Preset *preset_at(int i) { return i >= 0 && i < g_preset_count ? &g_presets[i] : NULL; }

const Preset *preset_find(const char *id) {
    for (int i = 0; i < g_preset_count; i++) if (!strcmp(g_presets[i].id, id)) return &g_presets[i];
    return NULL;
}

/* ----------------------------------------------------------------- presets */

static bool read_preset(Preset *p, const char *stem, const char *rel, const char *owner, const Json *root) {
    memset(p, 0, sizeof *p);
    snprintf(p->id, sizeof p->id, "%s", stem);
    snprintf(p->name, sizeof p->name, "%s", json_str(root, "name", stem));
    p->render_distance = json_int(root, "render_distance", 8);
    p->far_chunks = json_int(root, "far_chunks", 14);
    p->clouds = json_bool(root, "clouds", true);
    p->stars = json_bool(root, "stars", true);
    p->light_shafts = json_bool(root, "light_shafts", false);
    p->dynamic_resolution = json_bool(root, "dynamic_resolution", false);
    p->min_scale = (float)json_num(root, "min_scale", DEFAULT_MIN_SCALE);
    p->target_fps = (float)json_num(root, "target_fps", DEFAULT_TARGET_FPS);
    int errors = 0;
    if (p->render_distance < MIN_RENDER_DISTANCE || p->render_distance > MAX_RENDER_DISTANCE) {
        data_error(owner, rel, root->line, "render_distance must be %d to %d chunks.", MIN_RENDER_DISTANCE, MAX_RENDER_DISTANCE);
        errors++;
    }
    if (p->far_chunks < 0 || p->far_chunks > 64) { data_error(owner, rel, root->line, "far_chunks must be 0 (off) to 64 chunks."); errors++; }
    if (p->min_scale < MIN_SCALE_FLOOR || p->min_scale > 1.0f) {
        data_error(owner, rel, root->line, "min_scale must be %.1f to 1; below %.1f the picture is too soft to play.", MIN_SCALE_FLOOR, MIN_SCALE_FLOOR);
        errors++;
    }
    if (p->target_fps < 15.0f || p->target_fps > 240.0f) { data_error(owner, rel, root->line, "target_fps must be 15 to 240."); errors++; }
    return errors == 0;
}

static void add_preset(const Preset *p) {
    for (int i = 0; i < g_preset_count; i++)
        if (!strcmp(g_presets[i].id, p->id)) { g_presets[i] = *p; return; }
    if (g_preset_count < MAX_PRESETS) g_presets[g_preset_count++] = *p;
    else LOGW("preset '%s' ignored: at most %d presets are supported", p->id, MAX_PRESETS);
}

static void load_preset_file(const char *rel, const char *stem) {
    size_t size;
    const char *owner = "?";
    u8 *text = vfs_read(rel, &size, &owner);
    if (!text) return;
    char err[200];
    int err_line = 0;
    Json *root = json_parse((const char *)text, size, err, sizeof err, &err_line);
    free(text);
    if (!root) { data_error(owner, rel, err_line, "%s. Fix the JSON syntax at that line.", err); return; }
    Preset p;
    if (root->type != JSON_OBJECT) data_error(owner, rel, 1, "a preset file must contain one JSON object like {\"render_distance\": 8, ...}");
    else if (read_preset(&p, stem, rel, owner, root)) add_preset(&p);
    json_free(root);
}

int registry_load_presets(void) {
    int errors_before = data_error_count();
    g_preset_count = 0;
    StrList namespaces = {0};
    vfs_list("data", &namespaces);
    for (int n = 0; n < namespaces.n; n++) {
        char dir[160];
        snprintf(dir, sizeof dir, "data/%s/presets", namespaces.d[n]);
        StrList files = {0};
        vfs_list(dir, &files);
        for (int f = 0; f < files.n; f++) {
            size_t len = strlen(files.d[f]);
            if (len < 6 || strcmp(files.d[f] + len - 5, ".json")) continue;
            char rel[260], stem[PRESET_ID_MAX];
            snprintf(rel, sizeof rel, "%s/%s", dir, files.d[f]);
            snprintf(stem, sizeof stem, "%.*s", (int)MIN(len - 5, sizeof stem - 1), files.d[f]);
            load_preset_file(rel, stem);
        }
        strlist_free(&files);
    }
    strlist_free(&namespaces);
    if (!g_preset_count) data_error("?", "data/<namespace>/presets", 0, "no quality presets found. Add a file such as data/base/presets/low.json with \"render_distance\" and \"far_chunks\"; the base mod ships three.");
    return data_error_count() - errors_before;
}

/* ---------------------------------------------------------------- settings */

void settings_defaults(void) {
    memset(&g_settings, 0, sizeof g_settings);
    snprintf(g_settings.preset, sizeof g_settings.preset, "%s", DEFAULT_PRESET);
    g_settings.dynamic_resolution = -1;
    g_settings.render_scale = 1.0f;
    g_settings.fov_deg = DEFAULT_FOV;
    g_settings.vsync = true;
}

void settings_load(void) {
    settings_defaults();
    size_t size;
    u8 *text = file_read(SETTINGS_FILE, &size);
    if (!text) return;
    char err[200];
    int err_line = 0;
    Json *root = json_parse((const char *)text, size, err, sizeof err, &err_line);
    free(text);
    if (!root || root->type != JSON_OBJECT) {
        LOGW("%s is not valid JSON (%s); using default settings. Delete the file to silence this.", SETTINGS_FILE, root ? "expected an object" : err);
        json_free(root);
        return;
    }
    snprintf(g_settings.preset, sizeof g_settings.preset, "%s", json_str(root, "preset", g_settings.preset));
    g_settings.render_distance = CLAMP(json_int(root, "render_distance", 0), 0, MAX_RENDER_DISTANCE);
    g_settings.dynamic_resolution = CLAMP(json_int(root, "dynamic_resolution", -1), -1, 1);
    g_settings.render_scale = CLAMP((float)json_num(root, "render_scale", 1.0), MIN_SCALE_FLOOR, 1.0f);
    g_settings.fov_deg = CLAMP((float)json_num(root, "fov", DEFAULT_FOV), MIN_FOV, MAX_FOV);
    g_settings.vsync = json_bool(root, "vsync", true);
    json_free(root);
}

bool settings_save(void) {
    char text[512];
    int n = snprintf(text, sizeof text,
                     "{\n  \"preset\": \"%s\",\n  \"render_distance\": %d,\n  \"dynamic_resolution\": %d,\n  \"render_scale\": %.2f,\n  \"fov\": %.0f,\n  \"vsync\": %s\n}\n",
                     g_settings.preset, g_settings.render_distance, g_settings.dynamic_resolution, g_settings.render_scale, g_settings.fov_deg,
                     g_settings.vsync ? "true" : "false");
    if (!file_write_atomic(SETTINGS_FILE, text, (size_t)n)) {
        LOGW("could not write %s; check that the folder is writable. Settings apply for this session only.", SETTINGS_FILE);
        return false;
    }
    return true;
}

/* --------------------------------------------------------------- effective */

void gfx_apply(void) {
    const Preset *p = preset_find(g_settings.preset);
    if (!p) {
        LOGW("preset '%s' does not exist; falling back to '%s'", g_settings.preset, DEFAULT_PRESET);
        snprintf(g_settings.preset, sizeof g_settings.preset, "%s", DEFAULT_PRESET);
        p = preset_find(DEFAULT_PRESET);
        if (!p) p = preset_at(0);
    }
    if (!p) return;
    GraphicsConfig *g = &g_gfx;
    g->render_distance = g_settings.render_distance > 0 ? g_settings.render_distance : p->render_distance;
    g->far_chunks = g_opt.no_render ? 0 : p->far_chunks;
    g->clouds = p->clouds;
    g->stars = p->stars;
    g->light_shafts = p->light_shafts;
    g->dynamic_resolution = g_settings.dynamic_resolution < 0 ? p->dynamic_resolution : g_settings.dynamic_resolution == 1;
    g->min_scale = p->min_scale;
    g->fixed_scale = g_settings.render_scale;
    g->target_ms = 1000.0f / p->target_fps;
    g->fov_deg = g_settings.fov_deg;
    g->vsync = g_settings.vsync;
    g_scene_cfg.render_distance = g->render_distance;
    g_scene_cfg.far_chunks = g->far_chunks;
    g_scene_cfg.fov_deg = g->fov_deg;
    g_atmo.clouds = g->clouds;
    g_atmo.stars = g->stars;
}
