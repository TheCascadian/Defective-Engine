/* Quality presets (data) and player settings (file), combined into the configuration the renderer reads.
 *
 * Presets are data so a mod can add or retune them. The player's choices live in settings.json next to the saves
 * folder, and every setting has an automatic value that follows the preset, so changing the preset alone is enough
 * for most players. Rejected: baking presets into the executable, which would leave mods unable to target a
 * machine class. */
#include "dfe.h"
#include "ui.h"

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

static FogLevel g_fog_levels[MAX_FOG_LEVELS];
static int g_fog_level_count;

int preset_count(void) { return g_preset_count; }
const Preset *preset_at(int i) { return i >= 0 && i < g_preset_count ? &g_presets[i] : NULL; }

int fog_level_count(void) { return g_fog_level_count; }
const FogLevel *fog_level_at(int i) { return i >= 0 && i < g_fog_level_count ? &g_fog_levels[i] : NULL; }

const Preset *preset_find(const char *id) {
    for (int i = 0; i < g_preset_count; i++) if (!strcmp(g_presets[i].id, id)) return &g_presets[i];
    return NULL;
}

const FogLevel *fog_level_find(const char *id) {
    for (int i = 0; i < g_fog_level_count; i++) if (!strcmp(g_fog_levels[i].id, id)) return &g_fog_levels[i];
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
    p->fog = json_bool(root, "fog", true);
    p->min_scale = (float)json_num(root, "min_scale", DEFAULT_MIN_SCALE);
    p->target_fps = (float)json_num(root, "target_fps", DEFAULT_TARGET_FPS);
    p->entity_max_drawn = json_int(root, "entity_max_drawn", MAX_ENTITIES);
    p->entity_lod1 = (float)json_num(root, "entity_lod1", 32.0);
    p->entity_lod2 = (float)json_num(root, "entity_lod2", 96.0);
    snprintf(p->shadows, sizeof p->shadows, "%s", json_str(root, "shadows", ""));
    snprintf(p->godrays, sizeof p->godrays, "%s", json_str(root, "godrays", ""));
    snprintf(p->fog_quality, sizeof p->fog_quality, "%s", json_str(root, "fog_quality", ""));
    int errors = 0;
    if (p->render_distance < MIN_RENDER_DISTANCE || p->render_distance > MAX_RENDER_DISTANCE) {
        data_error(owner, rel, root->line, "render_distance must be %d to %d chunks.", MIN_RENDER_DISTANCE, MAX_RENDER_DISTANCE);
        errors++;
    }
    if (p->entity_max_drawn < 0 || p->entity_max_drawn > MAX_ENTITIES) { data_error(owner, rel, root->line, "entity_max_drawn must be 0 to %d entities.", MAX_ENTITIES); errors++; }
    if (!(p->entity_lod1 >= 4.0f && p->entity_lod2 >= p->entity_lod1 && p->entity_lod2 <= 1024.0f)) { data_error(owner, rel, root->line, "entity_lod1 and entity_lod2 are distances in blocks with 4 <= entity_lod1 <= entity_lod2 <= 1024."); errors++; }
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
    /* Keep the previous set so a reload with a mistake in it leaves the running game untouched. */
    Preset previous[MAX_PRESETS];
    int previous_count = g_preset_count;
    memcpy(previous, g_presets, sizeof previous);
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
    registry_load_shadow_levels();
    registry_load_fog_levels();
    registry_load_godray_levels();
    int errors = data_error_count() - errors_before;
    if (errors && previous_count) {
        memcpy(g_presets, previous, sizeof previous);
        g_preset_count = previous_count;
    }
    return errors;
}

/* ---------------------------------------------------------------- settings */

void settings_defaults(void) {
    memset(&g_settings, 0, sizeof g_settings);
    snprintf(g_settings.preset, sizeof g_settings.preset, "%s", DEFAULT_PRESET);
    g_settings.dynamic_resolution = -1;
    g_settings.ui_scale = -1;
    g_settings.hud_scale = g_settings.hud_text_scale = g_settings.ui_text_scale = 1.0f;
    g_settings.render_scale = 1.0f;
    g_settings.fov_deg = DEFAULT_FOV;
    g_settings.vsync = true;
    g_settings.fog_off = false;
    g_settings.pbr = *pbr_preset_cfg(3);
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
    g_settings.ui_scale = CLAMP(json_int(root, "ui_scale", -1), -1, 4);
    g_settings.hud_scale = CLAMP((float)json_num(root, "hud_scale", 1.0), 0.5f, 3.0f);
    g_settings.hud_text_scale = CLAMP((float)json_num(root, "hud_text_scale", 1.0), 0.75f, 2.0f);
    g_settings.ui_text_scale = CLAMP((float)json_num(root, "ui_text_scale", 1.0), 0.75f, 2.0f);
    g_settings.render_scale = CLAMP((float)json_num(root, "render_scale", 1.0), MIN_SCALE_FLOOR, 1.0f);
    g_settings.fov_deg = CLAMP((float)json_num(root, "fov", DEFAULT_FOV), MIN_FOV, MAX_FOV);
    g_settings.vsync = json_bool(root, "vsync", true);
    g_settings.shadows_off = !json_bool(root, "shadows", true);
    snprintf(g_settings.shadow_quality, sizeof g_settings.shadow_quality, "%s", json_str(root, "shadow_quality", ""));
    g_settings.shadow_distance = CLAMP(json_int(root, "shadow_distance", 0), 0, 1024);
    g_settings.godrays_off = !json_bool(root, "godrays", true);
    snprintf(g_settings.godray_quality, sizeof g_settings.godray_quality, "%s", json_str(root, "godray_quality", ""));
    g_settings.fog_off = !json_bool(root, "fog", true);
    snprintf(g_settings.fog_quality, sizeof g_settings.fog_quality, "%s", json_str(root, "fog_quality", ""));
    snprintf(g_settings.texture_quality, sizeof g_settings.texture_quality, "%s", json_str(root, "texture_quality", ""));
    snprintf(g_settings.texture_style, sizeof g_settings.texture_style, "%s", json_str(root, "texture_style", ""));
    const Json *sh = json_get(root, "shaders");
    if (sh && sh->type == JSON_OBJECT)
        for (int i = 0; i < PBR_FIELD_COUNT; i++) g_settings.pbr.v[i] = (float)json_num(sh, PBR_FIELDS[i].key, g_settings.pbr.v[i]);
    g_settings.auto_jump_off = !json_bool(root, "auto_jump", true);
    g_settings.view_bob_off = !json_bool(root, "view_bobbing", true);
    g_settings.motion_fx_off = !json_bool(root, "motion_effects", true);
    ui_overrides_load(root);
    json_free(root);
}

bool settings_save(void) {
    char text[3584], hud[512] = "", shaders[1536] = "";
    int sn = 0;
    for (int i = 0; i < PBR_FIELD_COUNT; i++) sn += snprintf(shaders + sn, sizeof shaders - (size_t)sn, "%s\"%s\": %.4g", i ? ", " : "", PBR_FIELDS[i].key, g_settings.pbr.v[i]);
    ui_overrides_write(hud, sizeof hud);
    int n = snprintf(text, sizeof text,
                     "{\n  \"preset\": \"%s\",\n  \"ui_scale\": %d,\n  \"render_distance\": %d,\n  \"dynamic_resolution\": %d,\n  \"render_scale\": %.2f,\n  \"fov\": %.0f,\n  \"vsync\": %s,\n  \"shadows\": %s,\n  \"shadow_quality\": \"%s\",\n  \"shadow_distance\": %d,\n  \"godrays\": %s,\n  \"godray_quality\": \"%s\",\n  \"fog\": %s,\n  \"fog_quality\": \"%s\",\n  \"auto_jump\": %s,\n  \"view_bobbing\": %s,\n  \"motion_effects\": %s,\n  \"texture_quality\": \"%s\",\n  \"texture_style\": \"%s\",\n  \"shaders\": {%s},\n  \"hud_scale\": %.2f,\n  \"hud_text_scale\": %.2f,\n  \"ui_text_scale\": %.2f,\n  \"hud_elements\": {%s}\n}\n",
                     g_settings.preset, g_settings.ui_scale, g_settings.render_distance, g_settings.dynamic_resolution, g_settings.render_scale, g_settings.fov_deg,
                     g_settings.vsync ? "true" : "false", g_settings.shadows_off ? "false" : "true", g_settings.shadow_quality, g_settings.shadow_distance,
                     g_settings.godrays_off ? "false" : "true", g_settings.godray_quality, g_settings.fog_off ? "false" : "true", g_settings.fog_quality,
                     g_settings.auto_jump_off ? "false" : "true", g_settings.view_bob_off ? "false" : "true", g_settings.motion_fx_off ? "false" : "true",
                     g_settings.texture_quality, g_settings.texture_style, shaders, g_settings.hud_scale, g_settings.hud_text_scale, g_settings.ui_text_scale, hud);
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
    pbr_resolve();
    GraphicsConfig *g = &g_gfx;
    g->render_distance = g_settings.render_distance > 0 ? g_settings.render_distance : p->render_distance;
    g->far_chunks = g_opt.no_render ? 0 : p->far_chunks;
    g->clouds = p->clouds;
    g->stars = p->stars;
    g->dynamic_resolution = g_settings.dynamic_resolution < 0 ? p->dynamic_resolution : g_settings.dynamic_resolution == 1;
    g->min_scale = p->min_scale;
    g->fixed_scale = g_settings.render_scale;
    g->target_ms = 1000.0f / p->target_fps;
    g->fov_deg = g_settings.fov_deg;
    g->vsync = g_settings.vsync;
    /* Shadows need a window, the player's say-so and a quality level that exists; the player's level wins over the preset's. */
    const ShadowLevel *level = g_settings.shadow_quality[0] ? shadow_level_find(g_settings.shadow_quality) : NULL;
    if (!level) level = shadow_level_find(p->shadows);
    g->shadows = !g_settings.shadows_off && !g_opt.no_render && level != NULL;
    if (level) g->shadow = *level;
    if (level && g_settings.shadow_distance > 0)
        g->shadow.distance = (float)CLAMP(g_settings.shadow_distance, 16, 1024);
    const FogLevel *fl = g_settings.fog_quality[0] ? fog_level_find(g_settings.fog_quality) : NULL;
    if (!fl) fl = fog_level_find(p->fog_quality);
    if (!fl && p->fog && fog_level_count()) fl = fog_level_at(fog_level_count() / 2);
    g->fog = !g_settings.fog_off && !g_opt.no_render && fl != NULL && (p->fog || g_settings.fog_quality[0]);
    if (fl) g->fog_level = *fl;
    /* Godrays: the player's level wins, then the preset's; a preset that only says light_shafts gets the middle level. */
    const GodrayLevel *gl = g_settings.godray_quality[0] ? godray_level_find(g_settings.godray_quality) : NULL;
    if (!gl) gl = godray_level_find(p->godrays);
    if (!gl && p->light_shafts && godray_level_count()) gl = godray_level_at(godray_level_count() / 2);
    g->light_shafts = !g_settings.godrays_off && !g_opt.no_render && gl != NULL;
    if (gl) g->godray = *gl;
    g_scene_cfg.render_distance = g->render_distance;
    g_scene_cfg.far_chunks = g->far_chunks;
    g_entity_cfg.max_drawn = p->entity_max_drawn;
    g_entity_cfg.lod1_distance = p->entity_lod1;
    g_entity_cfg.lod2_distance = p->entity_lod2;
    g_entity_cfg.instancing = !g_opt.entity_legacy;
    if (g_opt.entity_legacy) { g_entity_cfg.lod1_distance = g_entity_cfg.lod2_distance = 1e9f; g_entity_cfg.min_screen = 0.0f; }
    g_scene_cfg.fov_deg = g->fov_deg;
    g_atmo.clouds = g->clouds;
    g_atmo.stars = g->stars;
}

/* ----------------------------------------------------------------- fog levels */

static bool read_fog_level(FogLevel *l, const char *stem, const char *rel, const char *owner, const Json *root) {
    memset(l, 0, sizeof *l);
    snprintf(l->id, sizeof l->id, "%s", stem);
    snprintf(l->name, sizeof l->name, "%s", json_str(root, "name", stem));
    l->order = json_int(root, "order", 0);
    l->density = (float)json_num(root, "density", 0.28f);
    l->near_start = (float)json_num(root, "near_start", 4.0f);
    l->near_end = (float)json_num(root, "near_end", 28.0f);
    l->sun_boost = (float)json_num(root, "sun_boost", 1.0f);
    int errors = 0;
    if (l->density < 0.0f || l->density > 1.5f) { data_error(owner, rel, root->line, "density must be 0 to 1.5."); errors++; }
    if (l->near_start < 0.0f || l->near_start > 64.0f) { data_error(owner, rel, root->line, "near_start must be 0 to 64 blocks."); errors++; }
    if (l->near_end <= l->near_start || l->near_end > 256.0f) { data_error(owner, rel, root->line, "near_end must be larger than near_start and no more than 256 blocks."); errors++; }
    if (l->sun_boost < 0.0f || l->sun_boost > 2.0f) { data_error(owner, rel, root->line, "sun_boost must be 0 to 2."); errors++; }
    return errors == 0;
}

static void add_fog_level(const FogLevel *l) {
    for (int i = 0; i < g_fog_level_count; i++)
        if (!strcmp(g_fog_levels[i].id, l->id)) { g_fog_levels[i] = *l; return; }
    if (g_fog_level_count < MAX_FOG_LEVELS) g_fog_levels[g_fog_level_count++] = *l;
    else LOGW("fog level '%s' ignored: at most %d levels are supported", l->id, MAX_FOG_LEVELS);
}

static void load_fog_level_file(const char *rel, const char *stem) {
    size_t size;
    const char *owner = "?";
    u8 *text = vfs_read(rel, &size, &owner);
    if (!text) return;
    char err[200];
    int err_line = 0;
    Json *root = json_parse((const char *)text, size, err, sizeof err, &err_line);
    free(text);
    if (!root) { data_error(owner, rel, err_line, "%s. Fix the JSON syntax at that line.", err); return; }
    FogLevel l;
    if (root->type != JSON_OBJECT) data_error(owner, rel, 1, "a fog level file must contain one JSON object like {\"density\": 0.35, \"near_start\": 4, \"near_end\": 28, ...}");
    else if (read_fog_level(&l, stem, rel, owner, root)) add_fog_level(&l);
    json_free(root);
}

static int fog_level_order_cmp(const void *a, const void *b) {
    const FogLevel *x = a, *y = b;
    return x->order != y->order ? x->order - y->order : strcmp(x->id, y->id);
}

int registry_load_fog_levels(void) {
    int errors_before = data_error_count();
    FogLevel previous[MAX_FOG_LEVELS];
    int previous_count = g_fog_level_count;
    memcpy(previous, g_fog_levels, sizeof previous);
    g_fog_level_count = 0;
    StrList namespaces = {0};
    vfs_list("data", &namespaces);
    for (int n = 0; n < namespaces.n; n++) {
        char dir[160];
        snprintf(dir, sizeof dir, "data/%s/fog", namespaces.d[n]);
        StrList files = {0};
        vfs_list(dir, &files);
        for (int f = 0; f < files.n; f++) {
            size_t len = strlen(files.d[f]);
            if (len < 6 || strcmp(files.d[f] + len - 5, ".json")) continue;
            char rel[260], stem[PRESET_ID_MAX];
            snprintf(rel, sizeof rel, "%s/%s", dir, files.d[f]);
            snprintf(stem, sizeof stem, "%.*s", (int)MIN(len - 5, sizeof stem - 1), files.d[f]);
            load_fog_level_file(rel, stem);
        }
        strlist_free(&files);
    }
    strlist_free(&namespaces);
    qsort(g_fog_levels, (size_t)g_fog_level_count, sizeof g_fog_levels[0], fog_level_order_cmp);
    int errors = data_error_count() - errors_before;
    if (errors && previous_count) {
        memcpy(g_fog_levels, previous, sizeof previous);
        g_fog_level_count = previous_count;
    }
    return errors;
}
