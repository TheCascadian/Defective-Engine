/* Experimental feature registry and per-world feature state.  The registry is data (data/dfe/experimental_features.json);
 * nothing here names a feature.  A world stores which features it enabled in world.json under "experimental_features". */
#include "dfe.h"
#include "epoch.h"
#include <time.h>

#define EXP_FILE "data/dfe/experimental_features.json"

static ExpFeatureDef g_defs[EXP_MAX_FEATURES];
static int g_def_count;
static bool g_selection[EXP_MAX_FEATURES];
static bool g_forever_active;

static void copy_field(char *dst, size_t cap, const Json *o, const char *key, const char *file, bool *ok) {
    const char *s = json_str(o, key, NULL);
    if (!s || !*s) { data_error("dfe", file, o ? o->line : 1, "experimental feature needs a non-empty \"%s\" string.", key); *ok = false; return; }
    if (strlen(s) >= cap) { data_error("dfe", file, o->line, "experimental feature \"%s\" is longer than %zu characters.", key, cap - 1); *ok = false; return; }
    snprintf(dst, cap, "%s", s);
}

int experimental_load(void) {
    g_def_count = 0;
    memset(g_defs, 0, sizeof g_defs);
    size_t len = 0;
    const char *owner = "dfe";
    u8 *text = vfs_read(EXP_FILE, &len, &owner);
    if (!text) { data_error("dfe", EXP_FILE, 1, "experimental feature registry is missing. Expected %s in the engine assets.", EXP_FILE); return 0; }
    char err[160];
    int line = 0;
    Json *root = json_parse((const char *)text, len, err, sizeof err, &line);
    free(text);
    if (!root) { data_error("dfe", EXP_FILE, line, "%s", err); return 0; }
    if (root->type != JSON_ARRAY) { data_error("dfe", EXP_FILE, 1, "the registry must be an array of feature objects."); json_free(root); return 0; }
    for (int i = 0; i < json_len(root); i++) {
        const Json *o = json_at(root, i);
        if (!o || o->type != JSON_OBJECT) { data_error("dfe", EXP_FILE, o ? o->line : 1, "each feature must be an object."); continue; }
        if (g_def_count >= EXP_MAX_FEATURES) { data_error("dfe", EXP_FILE, o->line, "at most %d experimental features are supported.", EXP_MAX_FEATURES); break; }
        ExpFeatureDef d = {0};
        bool ok = true;
        copy_field(d.id, sizeof d.id, o, "id", EXP_FILE, &ok);
        copy_field(d.name, sizeof d.name, o, "name", EXP_FILE, &ok);
        copy_field(d.version, sizeof d.version, o, "version", EXP_FILE, &ok);
        copy_field(d.released, sizeof d.released, o, "released", EXP_FILE, &ok);
        copy_field(d.status, sizeof d.status, o, "status", EXP_FILE, &ok);
        copy_field(d.description, sizeof d.description, o, "description", EXP_FILE, &ok);
        d.requires_new_world = json_bool(o, "requires_new_world", false);
        if (!ok) continue;
        bool dup = false;
        for (int k = 0; k < g_def_count; k++) if (!strcmp(g_defs[k].id, d.id)) dup = true;
        if (dup) { data_error("dfe", EXP_FILE, o->line, "duplicate experimental feature id \"%s\".", d.id); continue; }
        g_defs[g_def_count++] = d;
    }
    json_free(root);
    return g_def_count;
}

int experimental_count(void) { return g_def_count; }
const ExpFeatureDef *experimental_def(int i) { return i >= 0 && i < g_def_count ? &g_defs[i] : NULL; }

const ExpFeatureDef *experimental_find(const char *id) {
    for (int i = 0; id && i < g_def_count; i++) if (!strcmp(g_defs[i].id, id)) return &g_defs[i];
    return NULL;
}

static int meta_slot(const SaveMeta *m, const char *id) {
    for (int i = 0; i < m->exp_count; i++) if (!strcmp(m->exp[i].id, id)) return i;
    return -1;
}

bool experimental_enabled(const SaveMeta *m, const char *id) {
    int i = meta_slot(m, id);
    return i >= 0 && m->exp[i].enabled;
}

static void utc_now(char *out, size_t cap) {
    time_t t = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

ExpResult experimental_set(SaveMeta *m, const char *id, bool enable, bool world_has_chunks, bool confirmed, char *warn, size_t warn_cap) {
    if (warn && warn_cap) warn[0] = 0;
    const ExpFeatureDef *def = experimental_find(id);
    if (!def) return EXP_UNKNOWN; /* a feature that is not in the registry cannot be enabled */
    int slot = meta_slot(m, id);
    bool was = slot >= 0 && m->exp[slot].enabled;
    if (was == enable) return EXP_UNCHANGED;
    if (enable && world_has_chunks && !confirmed) {
        if (warn) snprintf(warn, warn_cap, "%s is meant for new worlds. Every chunk already generated counts as epoch 0, and nothing is blended until the generator changes.", def->name);
        return EXP_NEEDS_CONFIRM;
    }
    if (!enable && !confirmed) {
        if (warn) snprintf(warn, warn_cap, "Turning off %s stops epoch tracking. %sFuture generator changes will leave visible walls at old chunk borders.", def->name, m->cgm_present ? "The recorded metadata stays in the save, but no new blending occurs. " : "");
        return EXP_NEEDS_CONFIRM;
    }
    if (slot < 0) {
        if (m->exp_count >= 8) return EXP_UNKNOWN;
        slot = m->exp_count++;
        memset(&m->exp[slot], 0, sizeof m->exp[slot]);
        snprintf(m->exp[slot].id, sizeof m->exp[slot].id, "%s", id);
    }
    m->exp[slot].enabled = enable;
    if (enable) {
        snprintf(m->exp[slot].enabled_version, sizeof m->exp[slot].enabled_version, "%s", def->version);
        utc_now(m->exp[slot].enabled_at, sizeof m->exp[slot].enabled_at);
    }
    return EXP_OK;
}

void experimental_write_json(JsonWriter *w, const SaveMeta *m) {
    if (!m->exp_count) return;
    jw_key(w, "experimental_features");
    jw_begin_obj(w);
    for (int i = 0; i < m->exp_count; i++) {
        jw_key(w, m->exp[i].id);
        jw_begin_obj(w);
        jw_key(w, "enabled"); jw_bool(w, m->exp[i].enabled);
        jw_key(w, "enabled_version"); jw_str(w, m->exp[i].enabled_version);
        jw_key(w, "enabled_at"); jw_str(w, m->exp[i].enabled_at);
        jw_end_obj(w);
    }
    jw_end_obj(w);
}

void experimental_read_json(SaveMeta *m, const Json *obj) {
    m->exp_count = 0;
    if (!obj || obj->type != JSON_OBJECT) return;
    for (int i = 0; i < obj->count && m->exp_count < 8; i++) {
        const Json *f = obj->items[i];
        const char *key = obj->keys ? obj->keys[i] : NULL;
        if (!f || !key || f->type != JSON_OBJECT || strlen(key) >= sizeof m->exp[0].id) continue;
        int s = m->exp_count++;
        memset(&m->exp[s], 0, sizeof m->exp[s]);
        snprintf(m->exp[s].id, sizeof m->exp[s].id, "%s", key);
        m->exp[s].enabled = json_bool(f, "enabled", false);
        snprintf(m->exp[s].enabled_version, sizeof m->exp[s].enabled_version, "%s", json_str(f, "enabled_version", ""));
        snprintf(m->exp[s].enabled_at, sizeof m->exp[s].enabled_at, "%s", json_str(f, "enabled_at", ""));
    }
}

void experimental_selection_clear(void) { memset(g_selection, 0, sizeof g_selection); }

bool experimental_selection_get(const char *id) {
    for (int i = 0; i < g_def_count; i++) if (!strcmp(g_defs[i].id, id)) return g_selection[i];
    return false;
}

void experimental_selection_set(const char *id, bool on) {
    for (int i = 0; i < g_def_count; i++) if (!strcmp(g_defs[i].id, id)) g_selection[i] = on;
}

/* Only features the player explicitly switched on become active; the default is off. */
bool experimental_selection_apply(SaveMeta *m) {
    bool any = false;
    for (int i = 0; i < g_def_count; i++) {
        if (!g_selection[i]) continue;
        any |= experimental_set(m, g_defs[i].id, true, false, true, NULL, 0) == EXP_OK;
    }
    experimental_selection_clear();
    return any;
}

bool forever_worlds_start(const char *save_dir, char *err, size_t err_cap) {
    if (!epoch_registry_load(NULL, err, err_cap)) return false;
    if (!cgm_store_open(save_dir, err, err_cap)) { epoch_registry_clear(); return false; }
    cgm_store_set_legacy_probe(save_has_column);
    forever_worlds_set_active(true);
    return true;
}

bool forever_worlds_active(void) { return g_forever_active; }
void forever_worlds_set_active(bool on) { g_forever_active = on; gen_refresh_epoch(); }
