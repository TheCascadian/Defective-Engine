/* Mod discovery, dependency resolution, mounting, native plugins, and the event bus and command table that
 * both Lua and native plugins use through the dfe_api_t table.
 *
 * Load order is deterministic: the base mod first, then dependencies before dependents, with ties broken by
 * mod id so the result never depends on directory order or the machine. A mod that fails (bad manifest, missing
 * or too old dependency, cycle) is excluded and everything that needs it is excluded with it; each exclusion
 * is reported through data_error with the mod, the file and what to change.
 *
 * Rejected: loading mods in directory order (differs between file systems and breaks overrides unpredictably),
 * and a priority number in the manifest (every mod author guesses high, so it carries no information). */
#include "dfe.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define MOD_MAX 128
#define MOD_ID_MIN 2

static ModInfo g_mods[MOD_MAX];
static int g_mod_count;
static int g_order[MOD_MAX];
static int g_loaded;
static char g_dir[512];

/* ---------------------------------------------------------------- versions */

static bool parse_version(const char *s, int out[3]) {
    out[0] = out[1] = out[2] = 0;
    int n = 0;
    while (*s && n < 3) {
        if (*s < '0' || *s > '9') return false;
        int v = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); if (v > 100000) return false; s++; }
        out[n++] = v;
        if (*s == '.') s++;
        else break;
    }
    return n > 0 && *s == '\0';
}

static int version_cmp(const int a[3], const int b[3]) {
    for (int i = 0; i < 3; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

static bool dep_satisfied(const ModDep *d, const ModInfo *m) {
    int c = version_cmp(m->ver, d->ver);
    switch (d->op) {
    case '>': return c >= 0;
    case '=': return c == 0;
    case '^': return m->ver[0] == d->ver[0] && c >= 0;
    default: return true;
    }
}

/* Dependency strings: "id", "id>=1.2", "id=1.0.0", "id^1.2", with a leading "?" for an optional dependency. */
static bool parse_dep(const char *text, ModDep *d, char *why, size_t cap) {
    memset(d, 0, sizeof *d);
    if (*text == '?') { d->optional = true; text++; }
    size_t n = strspn(text, "abcdefghijklmnopqrstuvwxyz0123456789_");
    if (n == 0 || n >= sizeof d->id) { snprintf(why, cap, "dependency \"%s\" does not start with a mod id (lowercase letters, digits and underscores)", text); return false; }
    memcpy(d->id, text, n);
    const char *rest = text + n;
    if (*rest == '\0') return true;
    if (!strncmp(rest, ">=", 2)) { d->op = '>'; rest += 2; }
    else if (*rest == '=') { d->op = '='; rest++; }
    else if (*rest == '^') { d->op = '^'; rest++; }
    else { snprintf(why, cap, "dependency \"%s\" has an unknown version operator; use >=, = or ^ followed by a version such as 1.2.0", text); return false; }
    if (!parse_version(rest, d->ver)) { snprintf(why, cap, "dependency \"%s\" has a bad version; write it as major.minor.patch, for example 1.2.0", text); return false; }
    return true;
}

/* ---------------------------------------------------------------- discovery */

static bool valid_id(const char *id) {
    size_t n = strlen(id);
    if (n < MOD_ID_MIN || n >= sizeof((ModInfo *)0)->id) return false;
    if (id[0] < 'a' || id[0] > 'z') return false;
    return strspn(id, "abcdefghijklmnopqrstuvwxyz0123456789_") == n;
}

/* A manifest path must stay inside the mod folder, so a mod cannot point the loader at other files. */
static bool safe_relative(const char *p) {
    if (!p[0] || p[0] == '/' || p[0] == '\\' || strchr(p, ':') || strstr(p, "..")) return false;
    return true;
}

static const char *platform_key(void) {
#ifdef _WIN32
    return "windows";
#else
    return "linux";
#endif
}

static bool read_manifest(const char *folder, const char *manifest_path, ModInfo *m) {
    size_t size = 0;
    u8 *text = file_read(manifest_path, &size);
    if (!text) { data_error(folder, "mod.json", 0, "cannot be read. Check that the file exists and is readable."); return false; }
    char err[160];
    int err_line = 0;
    Json *root = json_parse((const char *)text, size, err, sizeof err, &err_line);
    free(text);
    if (!root) { data_error(folder, "mod.json", err_line, "%s. Fix the JSON syntax at that line.", err); return false; }
    bool ok = false;
    do {
        if (root->type != JSON_OBJECT) { data_error(folder, "mod.json", 1, "the manifest must be a JSON object with at least \"id\" and \"version\"."); break; }
        const char *id = json_str(root, "id", "");
        if (!valid_id(id)) { data_error(folder, "mod.json", json_get(root, "id") ? json_get(root, "id")->line : 1, "\"id\" must be %d to 31 characters: lowercase letters, digits and underscores, starting with a letter.", MOD_ID_MIN); break; }
        snprintf(m->id, sizeof m->id, "%s", id);
        snprintf(m->name, sizeof m->name, "%s", json_str(root, "name", id));
        const char *ver = json_str(root, "version", "");
        if (!parse_version(ver, m->ver)) { data_error(id, "mod.json", json_get(root, "version") ? json_get(root, "version")->line : 1, "\"version\" must look like 1.2.0 (major.minor.patch)."); break; }
        snprintf(m->version, sizeof m->version, "%s", ver);
        int api = json_int(root, "api", DFE_API_VERSION);
        if (api != DFE_API_VERSION) {
            data_error(id, "mod.json", json_get(root, "api") ? json_get(root, "api")->line : 1, "targets mod API %d but this engine provides API %d. Update the mod, or use an engine build that matches.", api, DFE_API_VERSION);
            break;
        }
        const Json *deps = json_get(root, "depends");
        bool deps_ok = true;
        if (deps && deps->type != JSON_ARRAY) { data_error(id, "mod.json", deps->line, "\"depends\" must be an array of strings such as [\"base>=1.0\"]."); break; }
        for (int i = 0; deps && i < deps->count; i++) {
            const Json *it = deps->items[i];
            char why[200];
            if (m->dep_count >= MOD_MAX_DEPS) { data_error(id, "mod.json", it->line, "more than %d dependencies. Remove some or merge mods.", MOD_MAX_DEPS); deps_ok = false; break; }
            if (it->type != JSON_STRING || !parse_dep(it->str, &m->deps[m->dep_count], why, sizeof why)) {
                data_error(id, "mod.json", it->line, "%s.", it->type == JSON_STRING ? why : "each dependency must be a string");
                deps_ok = false;
                break;
            }
            m->dep_count++;
        }
        if (!deps_ok) break;
        const Json *after = json_get(root, "load_after");
        for (int i = 0; after && after->type == JSON_ARRAY && i < after->count && m->after_count < MOD_MAX_AFTER; i++)
            snprintf(m->load_after[m->after_count++], sizeof m->load_after[0], "%s", json_as_str(after->items[i], ""));
        const char *script = json_str(root, "script", "");
        if (script[0] && !safe_relative(script)) { data_error(id, "mod.json", json_get(root, "script")->line, "\"script\" must be a relative path inside the mod folder, such as scripts/main.lua."); break; }
        snprintf(m->script, sizeof m->script, "%s", script);
        const Json *plugins = json_get(root, "plugin");
        const char *plug = plugins && plugins->type == JSON_OBJECT ? json_str(plugins, platform_key(), "") : "";
        if (plug[0] && !safe_relative(plug)) { data_error(id, "mod.json", plugins->line, "the plugin path must be a relative path inside the mod folder."); break; }
        snprintf(m->plugin, sizeof m->plugin, "%s", plug);
        ok = true;
    } while (0);
    json_free(root);
    return ok;
}

void mods_reset(void) {
    memset(g_mods, 0, sizeof g_mods);
    g_mod_count = g_loaded = 0;
    g_dir[0] = '\0';
}

int mods_discover(const char *dir) {
    snprintf(g_dir, sizeof g_dir, "%s", dir);
    StrList entries = {0};
    dir_list(dir, &entries);
    for (int i = 0; i < entries.n; i++) {
        char folder[1100], manifest[1200];
        snprintf(folder, sizeof folder, "%s/%s", dir, entries.d[i]);
        snprintf(manifest, sizeof manifest, "%s/mod.json", folder);
        if (!path_is_dir(folder) || !path_exists(manifest)) continue;
        if (g_mod_count >= MOD_MAX) { data_error(entries.d[i], "mod.json", 0, "more than %d mods are installed; remove some.", MOD_MAX); continue; }
        ModInfo *m = &g_mods[g_mod_count];
        memset(m, 0, sizeof *m);
        m->order = -1;
        snprintf(m->dir, sizeof m->dir, "%s", folder);
        snprintf(m->manifest, sizeof m->manifest, "%s", manifest);
        if (!read_manifest(entries.d[i], manifest, m)) continue;
        bool dup = false;
        for (int k = 0; k < g_mod_count; k++) {
            if (strcmp(g_mods[k].id, m->id)) continue;
            data_error(m->id, "mod.json", 1, "id \"%s\" is already used by the folder %s. Give one of the two mods a different id or delete a folder.", m->id, g_mods[k].dir);
            dup = true;
            break;
        }
        if (!dup) g_mod_count++;
    }
    strlist_free(&entries);
    return g_mod_count;
}

/* ---------------------------------------------------------------- resolution */

static ModInfo *find_active(const char *id) {
    for (int i = 0; i < g_mod_count; i++)
        if (!g_mods[i].failed && !g_mods[i].disabled && !strcmp(g_mods[i].id, id)) return &g_mods[i];
    return NULL;
}

const ModInfo *mods_find(const char *id) {
    for (int i = 0; i < g_mod_count; i++) if (!strcmp(g_mods[i].id, id)) return &g_mods[i];
    return NULL;
}

static void apply_mods_json(void) {
    char path[600];
    snprintf(path, sizeof path, "%s/mods.json", g_dir);
    size_t size = 0;
    u8 *text = file_read(path, &size);
    if (!text) return;
    char err[160];
    int err_line = 0;
    Json *root = json_parse((const char *)text, size, err, sizeof err, &err_line);
    free(text);
    if (!root) { data_error("?", "mods/mods.json", err_line, "%s. Fix the JSON syntax, or delete the file to enable every mod.", err); return; }
    const Json *off = json_get(root, "disabled");
    for (int i = 0; off && off->type == JSON_ARRAY && i < off->count; i++) {
        const char *id = json_as_str(off->items[i], "");
        bool known = false;
        for (int k = 0; k < g_mod_count; k++)
            if (!strcmp(g_mods[k].id, id)) { g_mods[k].disabled = true; known = true; }
        if (!known) LOGW("mods.json disables \"%s\" but no such mod is installed. Remove the entry or check the id.", id);
    }
    json_free(root);
}

/* Excludes every mod whose required dependencies are absent or too old, repeating until nothing changes. */
static void check_dependencies(void) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < g_mod_count; i++) {
            ModInfo *m = &g_mods[i];
            if (m->failed || m->disabled) continue;
            for (int d = 0; d < m->dep_count; d++) {
                const ModDep *dep = &m->deps[d];
                const ModInfo *target = find_active(dep->id);
                if (!target && !dep->optional) {
                    data_error(m->id, "mod.json", 1, "requires mod \"%s\" which is not installed, is disabled or failed to load. Install it into the mods folder, or remove \"%s\".", dep->id, m->id);
                    m->failed = changed = true;
                    break;
                }
                if (target && !dep_satisfied(dep, target)) {
                    data_error(m->id, "mod.json", 1, "requires \"%s\" %c%d.%d.%d but version %s is installed. Update \"%s\" or relax the requirement.", dep->id, dep->op == '=' ? '=' : (dep->op == '^' ? '^' : '>'), dep->ver[0], dep->ver[1], dep->ver[2], target->version, dep->id);
                    m->failed = changed = true;
                    break;
                }
            }
        }
    }
}

static bool must_precede(const ModInfo *a, const ModInfo *b) {
    if (!strcmp(a->id, "base")) return strcmp(b->id, "base") != 0;
    for (int d = 0; d < b->dep_count; d++) if (!strcmp(b->deps[d].id, a->id)) return true;
    for (int k = 0; k < b->after_count; k++) if (!strcmp(b->load_after[k], a->id)) return true;
    return false;
}

static void compute_order(void) {
    bool placed[MOD_MAX] = {0};
    g_loaded = 0;
    int remaining = 0;
    for (int i = 0; i < g_mod_count; i++) if (!g_mods[i].failed && !g_mods[i].disabled) remaining++;
    while (remaining > 0) {
        int pick = -1;
        for (int i = 0; i < g_mod_count; i++) {
            const ModInfo *m = &g_mods[i];
            if (placed[i] || m->failed || m->disabled) continue;
            bool ready = true;
            for (int k = 0; k < g_mod_count && ready; k++) {
                const ModInfo *o = &g_mods[k];
                if (k == i || placed[k] || o->failed || o->disabled) continue;
                if (must_precede(o, m)) ready = false;
            }
            if (ready && (pick < 0 || strcmp(m->id, g_mods[pick].id) < 0)) pick = i;
        }
        if (pick < 0) break;
        placed[pick] = true;
        g_mods[pick].order = g_loaded;
        g_order[g_loaded++] = pick;
        remaining--;
    }
    if (remaining == 0) return;
    char names[256] = "";
    for (int i = 0; i < g_mod_count; i++)
        if (!placed[i] && !g_mods[i].failed && !g_mods[i].disabled) {
            size_t used = strlen(names);
            snprintf(names + used, sizeof names - used, "%s%.31s", used ? ", " : "", g_mods[i].id);
        }
    for (int i = 0; i < g_mod_count; i++)
        if (!placed[i] && !g_mods[i].failed && !g_mods[i].disabled) {
            data_error(g_mods[i].id, "mod.json", 1, "takes part in or depends on a dependency cycle among: %s. Remove one \"depends\" or \"load_after\" entry to break it.", names);
            g_mods[i].failed = true;
        }
}

int mods_resolve(void) {
    apply_mods_json();
    check_dependencies();
    compute_order();
    const ModInfo *base = find_active("base");
    if (!base) data_error("base", "mods/base/mod.json", 0, "the base mod is missing or failed to load, so there is no game content. Restore the mods/base folder.");
    return g_loaded;
}

void mods_mount(void) {
    for (int i = 0; i < g_loaded; i++) {
        const ModInfo *m = &g_mods[g_order[i]];
        vfs_add_root(m->dir, m->id);
        LOGI("mod %d: %s %s (%s)", i + 1, m->id, m->version, m->name);
    }
}

int mods_total(void) { return g_mod_count; }
const ModInfo *mods_at(int index) { return index >= 0 && index < g_mod_count ? &g_mods[index] : NULL; }
int mods_loaded_count(void) { return g_loaded; }
const ModInfo *mods_loaded_at(int order) { return order >= 0 && order < g_loaded ? &g_mods[g_order[order]] : NULL; }

/* ---------------------------------------------------------------- event bus */

#define SUB_MAX 512
#define CMD_MAX 128

typedef struct Sub {
    int handle;
    int event;
    dfe_event_fn fn;
    void *user;
    char mod[32];
} Sub;

typedef struct Cmd {
    int handle;
    char name[24], help[96], mod[32];
    dfe_command_fn fn;
    void *user;
} Cmd;

static const char *const EVENT_NAMES[] = {"tick", "block_place", "block_break", "world_load", "world_unload", "command", "random_tick"};
static Sub g_subs[SUB_MAX];
static int g_sub_count, g_next_handle = 1;
static Cmd g_cmds[CMD_MAX];
static int g_cmd_count;
static double g_game_time;

static int event_index(const char *name) {
    for (int i = 0; i < ARRAY_LEN(EVENT_NAMES); i++) if (!strcmp(EVENT_NAMES[i], name)) return i;
    return -1;
}

static int api_subscribe(const char *event, dfe_event_fn fn, void *user, const char *mod_id) {
    int idx = event_index(event);
    if (idx < 0) {
        LOGE("[mod %s] unknown event \"%s\". Valid events: tick, block_place, block_break, world_load, world_unload, command, random_tick.", mod_id, event);
        return 0;
    }
    if (g_sub_count >= SUB_MAX) { LOGE("[mod %s] too many event subscriptions (limit %d).", mod_id, SUB_MAX); return 0; }
    Sub *s = &g_subs[g_sub_count++];
    s->handle = g_next_handle++;
    s->event = idx;
    s->fn = fn;
    s->user = user;
    snprintf(s->mod, sizeof s->mod, "%s", mod_id ? mod_id : "?");
    return s->handle;
}

bool event_fire(const dfe_event_t *ev) {
    int idx = event_index(ev->name);
    if (idx < 0) return false;
    int count = g_sub_count; /* handlers added while dispatching run from the next event */
    for (int i = 0; i < count; i++) {
        if (g_subs[i].event != idx) continue;
        if (g_subs[i].fn(ev, g_subs[i].user)) return idx == 1 || idx == 2 || idx == 5;
    }
    return false;
}

/* The one entry point for edits made on behalf of the player (the console now, the interaction code later).
 * It fires block_break when the result is air and block_place otherwise, and a handler that cancels leaves
 * the world untouched. Mods writing through dfe.set_block bypass it on purpose: a handler that edits blocks
 * would otherwise trigger itself. */
bool game_edit_block(int x, int y, int z, u16 state) {
    u16 old = world_get_state(x, y, z);
    if (old == STATE_UNLOADED) return false;
    bool breaking = state == STATE_AIR;
    dfe_event_t ev = {.name = breaking ? "block_break" : "block_place", .x = x, .y = y, .z = z, .state = breaking ? old : state};
    if (event_fire(&ev)) return false;
    return world_set_state(x, y, z, state);
}

void events_clear(const char *mod_id) {
    int w = 0;
    for (int i = 0; i < g_sub_count; i++) if (strcmp(g_subs[i].mod, mod_id)) g_subs[w++] = g_subs[i];
    g_sub_count = w;
    w = 0;
    for (int i = 0; i < g_cmd_count; i++) if (strcmp(g_cmds[i].mod, mod_id)) g_cmds[w++] = g_cmds[i];
    g_cmd_count = w;
}

void events_clear_all(void) { g_sub_count = g_cmd_count = 0; }

/* ---------------------------------------------------------------- commands */

static int api_register_command(const char *name, const char *help, dfe_command_fn fn, void *user, const char *mod_id) {
    if (!name || !name[0] || strlen(name) >= sizeof g_cmds[0].name || strpbrk(name, " \t")) {
        LOGE("[mod %s] command name \"%s\" must be 1 to 23 characters without spaces.", mod_id, name ? name : "");
        return 0;
    }
    for (int i = 0; i < g_cmd_count; i++)
        if (!strcmp(g_cmds[i].name, name)) {
            LOGE("[mod %s] command \"%s\" is already registered by mod %s. Pick another name.", mod_id, name, g_cmds[i].mod);
            return 0;
        }
    if (g_cmd_count >= CMD_MAX) { LOGE("[mod %s] too many commands (limit %d).", mod_id, CMD_MAX); return 0; }
    Cmd *c = &g_cmds[g_cmd_count++];
    c->handle = g_next_handle++;
    snprintf(c->name, sizeof c->name, "%s", name);
    snprintf(c->help, sizeof c->help, "%s", help ? help : "");
    snprintf(c->mod, sizeof c->mod, "%s", mod_id ? mod_id : "?");
    c->fn = fn;
    c->user = user;
    return c->handle;
}

void command_run(const char *line) {
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '/') line++;
    if (!*line) return;
    size_t n = strcspn(line, " \t");
    const char *args = line + n;
    while (*args == ' ' || *args == '\t') args++;
    for (int i = 0; i < g_cmd_count; i++)
        if (strlen(g_cmds[i].name) == n && !strncmp(g_cmds[i].name, line, n)) { g_cmds[i].fn(args, g_cmds[i].user); return; }
    dfe_event_t ev = {.name = "command", .text = line};
    if (event_fire(&ev)) return;
    console_print("unknown command \"%.*s\". Type help for the list.", (int)n, line);
}

int command_count(void) { return g_cmd_count; }
const char *command_name(int i) { return i >= 0 && i < g_cmd_count ? g_cmds[i].name : ""; }
const char *command_help(int i) { return i >= 0 && i < g_cmd_count ? g_cmds[i].help : ""; }

/* ---------------------------------------------------------------- api table */

static void api_log(dfe_log_level level, const char *mod_id, const char *message) {
    LogLevel l = level == DFE_LOG_DEBUG ? LOG_DEBUG : level == DFE_LOG_WARN ? LOG_WARN : level == DFE_LOG_ERROR ? LOG_ERROR : LOG_INFO;
    log_msg(l, "[mod %s] %s", mod_id ? mod_id : "?", message);
}

static uint16_t api_block_state(const char *name) { return block_parse_state(name); }
static bool api_state_string(uint16_t state, char *out, size_t size) { return block_format_state(state, out, size); }

static const char *api_block_name(uint16_t state) {
    if (state == STATE_UNLOADED) return NULL;
    const BlockDef *b = block_of_state(state);
    return b ? b->name : NULL;
}

static uint16_t api_get_state(int x, int y, int z) { return world_get_state(x, y, z); }
static bool api_set_state(int x, int y, int z, uint16_t state) { return world_set_state(x, y, z, state); }

static bool api_get_light(int x, int y, int z, uint8_t out[4]) {
    if (world_get_state(x, y, z) == STATE_UNLOADED) return false;
    u16 l = world_get_light(x, y, z);
    out[0] = (uint8_t)LIGHT_SKY(l); out[1] = (uint8_t)LIGHT_R(l); out[2] = (uint8_t)LIGHT_G(l); out[3] = (uint8_t)LIGHT_B(l);
    return true;
}

static uint64_t api_world_seed(void) { return world_seed(); }
static double api_game_time(void) { return g_game_time; }
static void api_console_print(const char *message) { console_print("%s", message); }

const dfe_api_t *api_get(void) {
    static const dfe_api_t api = {
        .abi_version = DFE_API_VERSION,
        .struct_size = sizeof(dfe_api_t),
        .log = api_log,
        .block_state = api_block_state,
        .block_name = api_block_name,
        .get_state = api_get_state,
        .set_state = api_set_state,
        .get_light = api_get_light,
        .world_seed = api_world_seed,
        .game_time = api_game_time,
        .subscribe = api_subscribe,
        .register_command = api_register_command,
        .console_print = api_console_print,
        .state_string = api_state_string,
    };
    return &api;
}

double game_time_get(void) { return g_game_time; }
void game_time_set(double seconds) { g_game_time = seconds; }

void game_tick(void) {
    g_game_time += GAME_TICK_DT;
    server_tick();
    dfe_event_t ev = {.name = "tick", .dt = GAME_TICK_DT};
    event_fire(&ev);
}

/* ---------------------------------------------------------------- native plugins */

typedef struct Plugin {
    void *lib;
    void (*shutdown)(void);
    char mod[32];
} Plugin;

static Plugin g_plugins[MOD_MAX];
static int g_plugin_count;

static void *lib_open(const char *path) {
#ifdef _WIN32
    return (void *)LoadLibraryA(path);
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}
static void *lib_sym(void *lib, const char *name) {
#ifdef _WIN32
    return (void *)GetProcAddress((HMODULE)lib, name);
#else
    return dlsym(lib, name);
#endif
}
static void lib_close(void *lib) {
#ifdef _WIN32
    FreeLibrary((HMODULE)lib);
#else
    dlclose(lib);
#endif
}

void mods_load_plugins(bool allow_native) {
    for (int i = 0; i < g_loaded; i++) {
        const ModInfo *m = &g_mods[g_order[i]];
        if (!m->plugin[0]) continue;
        if (!allow_native) {
            LOGW("[mod %s] declares the native plugin %s, which runs with full privileges, so it was skipped. Start the game with --allow-native to load it.", m->id, m->plugin);
            continue;
        }
        char path[700];
        snprintf(path, sizeof path, "%s/%s", m->dir, m->plugin);
        void *lib = lib_open(path);
        if (!lib) { data_error(m->id, m->plugin, 0, "the library could not be loaded. Build it for this platform and check its dependencies."); continue; }
        int (*init)(const dfe_api_t *, const char *) = (int (*)(const dfe_api_t *, const char *))lib_sym(lib, "dfe_plugin_init");
        if (!init) { data_error(m->id, m->plugin, 0, "does not export dfe_plugin_init. Export it with DFE_PLUGIN_EXPORT from dfe_api.h."); lib_close(lib); continue; }
        if (init(api_get(), m->id) != 0) {
            data_error(m->id, m->plugin, 0, "dfe_plugin_init returned an error. See the log lines above for the reason.");
            events_clear(m->id);
            lib_close(lib);
            continue;
        }
        Plugin *p = &g_plugins[g_plugin_count++];
        p->lib = lib;
        p->shutdown = (void (*)(void))lib_sym(lib, "dfe_plugin_shutdown");
        snprintf(p->mod, sizeof p->mod, "%s", m->id);
        LOGI("[mod %s] native plugin loaded", m->id);
    }
}

void mods_unload_plugins(void) {
    for (int i = g_plugin_count - 1; i >= 0; i--) {
        if (g_plugins[i].shutdown) g_plugins[i].shutdown();
        events_clear(g_plugins[i].mod);
        lib_close(g_plugins[i].lib);
    }
    g_plugin_count = 0;
}
