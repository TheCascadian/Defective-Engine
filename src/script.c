/* Sandboxed Lua scripting. The bindings are written against the same dfe_api_t table native plugins use, so a
 * script can never do something a plugin cannot, and the stable C header stays the single source of truth.
 *
 * One state serves every mod, and each mod gets its own global table, so mods cannot read or overwrite each
 * other's variables. The sandbox has no io, os, package, debug, ffi or jit libraries and no way to load
 * arbitrary files, strings or bytecode. Every call runs under an instruction budget enforced by a count hook, and
 * the state has a memory cap, so an endless loop or a runaway string.rep fails that one call with a message that
 * names the mod, file and line instead of freezing the game.
 *
 * Handlers that raise an error are switched off after reporting it once. Raising every tick would bury the
 * console in a few seconds, and a handler that failed once usually fails the same way again.
 *
 * Rejected: one state per mod (memory and start-up cost on the low-end target for isolation the per-mod
 * global table already gives), and a wall-clock timeout (not deterministic, and a slow machine would fail
 * scripts a fast one runs). */
#include "dfe.h"
#include "ui.h"
#include "screen.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luajit.h"

#define HOOK_STEP 1000
#define BUDGET_LOAD 30000000L    /* a script file may do real set-up work once */
#define BUDGET_HANDLER 1000000L  /* per event, so a tick handler cannot eat the frame */
#define BUDGET_COMMAND 5000000L
#define MEMORY_LIMIT (96u << 20)

static lua_State *L;
static size_t g_mem;
static long g_instr, g_limit;
static bool g_expired;
static int g_errors;
static int g_mod_env_ref[128];
static char g_mod_env_id[128][32];
static int g_mod_env_count;

/* ---------------------------------------------------------------- state */

static void *limited_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud;
    if (!ptr) osize = 0;
    if (nsize == 0) { free(ptr); g_mem -= osize; return NULL; }
    if (nsize > osize && g_mem + (nsize - osize) > MEMORY_LIMIT) return NULL;
    void *p = realloc(ptr, nsize);
    if (p) g_mem = g_mem - osize + nsize;
    return p;
}

static void budget_hook(lua_State *state, lua_Debug *ar) {
    g_instr += g_expired ? 1 : HOOK_STEP;
    if (g_instr <= g_limit) return;
    if (!g_expired) {
        /* From now on check after every instruction, so a script cannot hide the overrun inside pcall. */
        g_expired = true;
        lua_sethook(state, budget_hook, LUA_MASKCOUNT, 1);
    }
    lua_getinfo(state, "Sl", ar);
    lua_pushfstring(state, "%s:%d: script used more than %d instructions in one call. Remove the endless loop, or spread the work over several ticks",
                    ar->short_src, ar->currentline, (int)g_limit);
    lua_error(state);
}

/* Splits "scripts/main.lua:12: message" into file, line and text. */
static void split_location(const char *msg, const char *fallback_file, char *file, size_t cap, int *line, const char **text) {
    snprintf(file, cap, "%s", fallback_file);
    *line = 0;
    *text = msg;
    const char *colon = strchr(msg, ':');
    int ln = 0, consumed = 0;
    if (colon && sscanf(colon + 1, "%d:%n", &ln, &consumed) >= 1 && consumed > 0) {
        size_t n = (size_t)(colon - msg);
        if (n > 0 && n < cap) { memcpy(file, msg, n); file[n] = '\0'; }
        *line = ln;
        *text = colon + 1 + consumed;
        while (**text == ' ') (*text)++;
    }
}

static bool lua_key_valid(const char *key) {
    if (!key || !key[0] || strlen(key) >= 64) return false;
    size_t n = strspn(key, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-. ");
    return n == strlen(key) && strchr(key, ' ') == NULL;
}

static void lua_push_json(lua_State *state, const Json *j) {
    if (!j) { lua_pushnil(state); return; }
    switch (j->type) {
    case JSON_NULL: lua_pushnil(state); break;
    case JSON_BOOL: lua_pushboolean(state, j->boolean); break;
    case JSON_NUMBER: lua_pushnumber(state, (lua_Number)j->num); break;
    case JSON_STRING: lua_pushstring(state, j->str ? j->str : ""); break;
    case JSON_ARRAY: {
        lua_createtable(state, j->count, 0);
        for (int i = 0; i < j->count; i++) {
            lua_push_json(state, j->items[i]);
            lua_rawseti(state, -2, i + 1);
        }
        break;
    }
    case JSON_OBJECT: {
        lua_createtable(state, 0, j->count);
        for (int i = 0; i < j->count; i++) {
            lua_pushstring(state, j->keys[i]);
            lua_push_json(state, j->items[i]);
            lua_rawset(state, -3);
        }
        break;
    }
    }
}

static Json *json_from_lua(lua_State *state, int index, char *err, size_t cap) {
    if (lua_isnil(state, index)) {
        Json *j = xcalloc(1, sizeof *j);
        j->type = JSON_NULL;
        return j;
    }
    if (lua_isboolean(state, index)) {
        Json *j = xcalloc(1, sizeof *j);
        j->type = JSON_BOOL;
        j->boolean = lua_toboolean(state, index);
        return j;
    }
    if (lua_isnumber(state, index)) {
        Json *j = xcalloc(1, sizeof *j);
        j->type = JSON_NUMBER;
        j->num = lua_tonumber(state, index);
        return j;
    }
    if (lua_isstring(state, index)) {
        Json *j = xcalloc(1, sizeof *j);
        j->type = JSON_STRING;
        j->str = xstrdup(lua_tostring(state, index));
        return j;
    }
    if (lua_istable(state, index)) {
        size_t len = (size_t)lua_objlen(state, index);
        bool is_array = true;
        for (size_t i = 1; i <= len; i++) {
            lua_rawgeti(state, index, (int)i);
            bool empty = lua_isnil(state, -1);
            lua_pop(state, 1);
            if (empty) { is_array = false; break; }
        }
        if (is_array) {
            Json *a = xcalloc(1, sizeof *a);
            a->type = JSON_ARRAY;
            a->count = (int)len;
            a->items = xcalloc(len ? len : 1, sizeof(Json *));
            for (size_t i = 1; i <= len; i++) {
                lua_rawgeti(state, index, (int)i);
                a->items[i - 1] = json_from_lua(state, -1, err, cap);
                lua_pop(state, 1);
                if (!a->items[i - 1]) {
                    json_free(a);
                    return NULL;
                }
            }
            return a;
        }
        Json *o = xcalloc(1, sizeof *o);
        o->type = JSON_OBJECT;
        lua_pushnil(state);
        while (lua_next(state, index)) {
            const char *key = NULL;
            if (lua_isstring(state, -2)) key = lua_tostring(state, -2);
            else if (lua_isnumber(state, -2)) {
                char tmp[32];
                snprintf(tmp, sizeof tmp, "%g", lua_tonumber(state, -2));
                key = xstrdup(tmp);
            } else {
                snprintf(err, cap, "table keys must be strings or numbers");
                lua_pop(state, 2);
                json_free(o);
                return NULL;
            }
            if (!lua_key_valid(key)) {
                snprintf(err, cap, "storage key \"%s\" is invalid; use letters, digits, underscores, '-' or '.' only", key);
                free((void *)key);
                lua_pop(state, 2);
                json_free(o);
                return NULL;
            }
            Json *v = json_from_lua(state, -1, err, cap);
            if (!v) {
                lua_pop(state, 2);
                json_free(o);
                return NULL;
            }
            o->keys = xrealloc(o->keys, (size_t)(o->count + 1) * sizeof(char *));
            o->items = xrealloc(o->items, (size_t)(o->count + 1) * sizeof(Json *));
            o->keys[o->count] = xstrdup(key);
            o->items[o->count++] = v;
            if (lua_isnumber(state, -2)) free((void *)key);
            lua_pop(state, 1);
        }
        return o;
    }
    snprintf(err, cap, "unsupported storage value type: %s", luaL_typename(state, index));
    return NULL;
}

static void report_error(const char *mod, const char *fallback_file, const char *msg) {
    char file[128];
    int line;
    const char *text;
    split_location(msg, fallback_file, file, sizeof file, &line, &text);
    data_error(mod, file, line, "%s", text);
    g_errors++;
}

/* Runs the function and its arguments on top of the stack under a fresh budget. On failure the message is
 * left on the stack and false is returned. Budgets nest, so a handler that fires another event keeps both. */
static bool call_limited(int nargs, int nresults, long budget) {
    long saved_instr = g_instr, saved_limit = g_limit;
    bool saved_expired = g_expired;
    g_instr = 0; g_limit = budget; g_expired = false;
    lua_sethook(L, budget_hook, LUA_MASKCOUNT, HOOK_STEP);
    int rc = lua_pcall(L, nargs, nresults, 0);
    g_instr = saved_instr; g_limit = saved_limit; g_expired = saved_expired;
    lua_sethook(L, budget_hook, LUA_MASKCOUNT, HOOK_STEP);
    return rc == 0;
}

/* ---------------------------------------------------------------- bindings */

static const char *mod_of(lua_State *state) { return lua_tostring(state, lua_upvalueindex(1)); }

static int l_log(lua_State *state) {
    static const char *const levels[] = {"debug", "info", "warn", "error", NULL};
    int level = luaL_checkoption(state, 1, "info", levels);
    api_get()->log((dfe_log_level)level, mod_of(state), luaL_checkstring(state, 2));
    return 0;
}

static int l_print(lua_State *state) {
    int n = lua_gettop(state);
    luaL_Buffer b;
    luaL_buffinit(state, &b);
    for (int i = 1; i <= n; i++) {
        size_t len;
        lua_getfield(state, LUA_REGISTRYINDEX, "dfe_tostring");
        lua_pushvalue(state, i);
        lua_call(state, 1, 1);
        const char *s = lua_tolstring(state, -1, &len);
        if (i > 1) luaL_addchar(&b, '\t');
        luaL_addlstring(&b, s ? s : "", s ? len : 0);
        lua_pop(state, 1);
    }
    luaL_pushresult(&b);
    api_get()->log(DFE_LOG_INFO, mod_of(state), lua_tostring(state, -1));
    return 0;
}

static int l_console(lua_State *state) { api_get()->console_print(luaL_checkstring(state, 1)); return 0; }

static int l_block_state(lua_State *state) {
    uint16_t s = api_get()->block_state(luaL_checkstring(state, 1));
    if (s == DFE_STATE_UNLOADED) lua_pushnil(state); else lua_pushinteger(state, s);
    return 1;
}

static int l_block_name(lua_State *state) {
    const char *n = api_get()->block_name((uint16_t)luaL_checkinteger(state, 1));
    if (n) lua_pushstring(state, n); else lua_pushnil(state);
    return 1;
}

static int l_state_name(lua_State *state) {
    char buf[128];
    if (api_get()->state_string((uint16_t)luaL_checkinteger(state, 1), buf, sizeof buf)) lua_pushstring(state, buf); else lua_pushnil(state);
    return 1;
}

static int l_get_state(lua_State *state) {
    uint16_t s = api_get()->get_state((int)floor(luaL_checknumber(state, 1)), (int)floor(luaL_checknumber(state, 2)), (int)floor(luaL_checknumber(state, 3)));
    if (s == DFE_STATE_UNLOADED) lua_pushnil(state); else lua_pushinteger(state, s);
    return 1;
}

static int l_get_block(lua_State *state) {
    uint16_t s = api_get()->get_state((int)floor(luaL_checknumber(state, 1)), (int)floor(luaL_checknumber(state, 2)), (int)floor(luaL_checknumber(state, 3)));
    const char *n = s == DFE_STATE_UNLOADED ? NULL : api_get()->block_name(s);
    if (n) lua_pushstring(state, n); else lua_pushnil(state);
    return 1;
}

static int l_set_block(lua_State *state) {
    uint16_t s;
    if (lua_type(state, 4) == LUA_TSTRING) {
        s = api_get()->block_state(lua_tostring(state, 4));
        if (s == DFE_STATE_UNLOADED) return luaL_error(state, "unknown block \"%s\". Use a registered name such as base:stone", lua_tostring(state, 4));
    } else {
        s = (uint16_t)luaL_checkinteger(state, 4);
    }
    lua_pushboolean(state, api_get()->set_state((int)floor(luaL_checknumber(state, 1)), (int)floor(luaL_checknumber(state, 2)), (int)floor(luaL_checknumber(state, 3)), s));
    return 1;
}

static int l_entity_spawn(lua_State *state) {
    int id = api_get()->entity_spawn(luaL_checkstring(state, 1), luaL_checknumber(state, 2), luaL_checknumber(state, 3), luaL_checknumber(state, 4));
    if (id) lua_pushinteger(state, id); else lua_pushnil(state);
    return 1;
}

static int l_entity_remove(lua_State *state) { lua_pushboolean(state, api_get()->entity_remove((int)luaL_checkinteger(state, 1))); return 1; }

static int l_entity_position(lua_State *state) {
    double p[3];
    if (!api_get()->entity_position((int)luaL_checkinteger(state, 1), p)) { lua_pushnil(state); return 1; }
    for (int i = 0; i < 3; i++) lua_pushnumber(state, p[i]);
    return 3;
}

static int l_entity_count(lua_State *state) { lua_pushinteger(state, api_get()->entity_count()); return 1; }

static int l_get_light(lua_State *state) {
    uint8_t out[4];
    if (!api_get()->get_light((int)floor(luaL_checknumber(state, 1)), (int)floor(luaL_checknumber(state, 2)), (int)floor(luaL_checknumber(state, 3)), out)) { lua_pushnil(state); return 1; }
    for (int i = 0; i < 4; i++) lua_pushinteger(state, out[i]);
    return 4;
}

static int l_seed(lua_State *state) { lua_pushnumber(state, (lua_Number)(api_get()->world_seed() & 0xFFFFFFFFu)); return 1; }
static int l_time(lua_State *state) { lua_pushnumber(state, api_get()->game_time()); return 1; }
static int l_player_info(lua_State *state) {
    (void)state; lua_newtable(state);
    lua_pushstring(state, g_player.id); lua_setfield(state, -2, "id");
    lua_pushstring(state, g_player.name); lua_setfield(state, -2, "name");
    lua_pushnumber(state, g_player.health); lua_setfield(state, -2, "health");
    lua_pushboolean(state, g_player.dead); lua_setfield(state, -2, "dead");
    lua_pushstring(state, g_player.mod_state); lua_setfield(state, -2, "state");
    return 1;
}
static int l_player_state(lua_State *state) {
    if (lua_gettop(state) >= 1) snprintf(g_player.mod_state, sizeof g_player.mod_state, "%s", luaL_checkstring(state, 1));
    lua_pushstring(state, g_player.mod_state); return 1;
}

static int l_item_count(lua_State *state) { lua_pushinteger(state, item_definition_count()); return 1; }
static int l_item_add(lua_State *state) {
    const char *id = luaL_checkstring(state, 1);
    int count = (int)luaL_optinteger(state, 2, 1);
    lua_pushinteger(state, inventory_add_item(&g_inv, id, count));
    return 1;
}
static int l_inventory_count(lua_State *state) {
    const char *id = luaL_checkstring(state, 1);
    const ItemDef *d = item_find(id);
    const BlockDef *b = block_find(id);
    if (!d && !b) { lua_pushinteger(state, 0); return 1; }
    u16 placed = d && d->place[0] ? block_parse_state(d->place) : b->default_state;
    lua_pushinteger(state, inventory_count(&g_inv, placed));
    return 1;
}
static int l_recipe_count(lua_State *state) { lua_pushinteger(state, recipe_count()); return 1; }
static int l_recipe_craft(lua_State *state) {
    const char *id = luaL_checkstring(state, 1);
    lua_pushboolean(state, recipe_craft(&g_inv, id));
    return 1;
}
static int l_recipe_process(lua_State *state) {
    const char *id = luaL_checkstring(state, 1); float duration = 0;
    bool ok = recipe_process(&g_inv, id, &duration);
    lua_pushboolean(state, ok); lua_pushnumber(state, duration); return 2;
}
static int l_loot_roll(lua_State *state) {
    const char *id = luaL_checkstring(state, 1);
    static Rng rng = {.s = 0xDFFEULL};
    if (!rng.s) rng.s = 0xDFFEULL;
    lua_pushinteger(state, loot_roll(id, &rng, &g_inv));
    return 1;
}
static int l_container_count(lua_State *state) {
    const char *key = luaL_checkstring(state, 1), *id = luaL_checkstring(state, 2);
    Container c; if (!container_open(mod_of(state), key, &c, 27)) { lua_pushinteger(state, 0); return 1; }
    int n = 0; for (int i = 0; i < c.slots; i++) if (c.slot[i].count && !strcmp(c.slot[i].item_id, id)) n += c.slot[i].count;
    container_close(mod_of(state), key, &c); lua_pushinteger(state, n); return 1;
}
static int l_container_add(lua_State *state) {
    const char *key = luaL_checkstring(state, 1), *id = luaL_checkstring(state, 2);
    int count = (int)luaL_optinteger(state, 3, 1); Container c;
    if (!container_open(mod_of(state), key, &c, 27) || count <= 0) { lua_pushinteger(state, count); return 1; }
    int left = container_add_item(&c, id, count);
    container_close(mod_of(state), key, &c); lua_pushinteger(state, left); return 1;
}

static int l_storage_get(lua_State *state) {
    const char *key = luaL_checkstring(state, 1);
    if (!lua_key_valid(key)) return luaL_error(state, "storage key \"%s\" is invalid; use letters, digits, underscores, '-' or '.' only", key);
    const Json *v = mod_storage_get(mod_of(state), key);
    if (!v) { lua_pushnil(state); return 1; }
    lua_push_json(state, v);
    return 1;
}

static int l_storage_set(lua_State *state) {
    const char *key = luaL_checkstring(state, 1);
    if (!lua_key_valid(key)) return luaL_error(state, "storage key \"%s\" is invalid; use letters, digits, underscores, '-' or '.' only", key);
    if (lua_isnil(state, 2)) {
        lua_pushboolean(state, mod_storage_remove(mod_of(state), key));
        return 1;
    }
    char err[160];
    Json *v = json_from_lua(state, 2, err, sizeof err);
    if (!v) return luaL_error(state, "%s", err[0] ? err : "unsupported storage value");
    bool ok = mod_storage_set(mod_of(state), key, v);
    json_free(v);
    lua_pushboolean(state, ok);
    return 1;
}

static int l_storage_remove(lua_State *state) {
    const char *key = luaL_checkstring(state, 1);
    if (!lua_key_valid(key)) return luaL_error(state, "storage key \"%s\" is invalid; use letters, digits, underscores, '-' or '.' only", key);
    lua_pushboolean(state, mod_storage_remove(mod_of(state), key));
    return 1;
}

/* ---------------------------------------------------------------- callbacks into Lua */

typedef struct LuaCallback {
    int ref;
    bool dead;
    char mod[32], what[32];
} LuaCallback;

static void disable_callback(LuaCallback *cb, const char *msg) {
    report_error(cb->mod, "scripts", msg);
    LOGE("[mod %s] %s was switched off after the error above. Fix the script and restart the game.", cb->mod, cb->what);
    cb->dead = true;
    luaL_unref(L, LUA_REGISTRYINDEX, cb->ref);
    cb->ref = LUA_NOREF;
}

static int event_trampoline(const dfe_event_t *ev, void *user) {
    LuaCallback *cb = user;
    if (cb->dead) return 0;
    lua_rawgeti(L, LUA_REGISTRYINDEX, cb->ref);
    lua_createtable(L, 0, 7);
    lua_pushstring(L, ev->name); lua_setfield(L, -2, "name");
    lua_pushinteger(L, ev->x); lua_setfield(L, -2, "x");
    lua_pushinteger(L, ev->y); lua_setfield(L, -2, "y");
    lua_pushinteger(L, ev->z); lua_setfield(L, -2, "z");
    lua_pushinteger(L, ev->state); lua_setfield(L, -2, "state");
    lua_pushnumber(L, ev->dt); lua_setfield(L, -2, "dt");
    if (ev->text) { lua_pushstring(L, ev->text); lua_setfield(L, -2, "text"); }
    if (ev->entity_id) { lua_pushinteger(L, ev->entity_id); lua_setfield(L, -2, "entity_id"); }
    if (ev->damage != 0.0f) { lua_pushnumber(L, ev->damage); lua_setfield(L, -2, "damage"); }
    if (!call_limited(1, 1, BUDGET_HANDLER)) {
        disable_callback(cb, lua_tostring(L, -1));
        lua_pop(L, 1);
        return 0;
    }
    int cancel = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return cancel;
}

static void command_trampoline(const char *args, void *user) {
    LuaCallback *cb = user;
    if (cb->dead) { console_print("command %s is switched off after an earlier error.", cb->what); return; }
    lua_rawgeti(L, LUA_REGISTRYINDEX, cb->ref);
    lua_pushstring(L, args);
    if (!call_limited(1, 0, BUDGET_COMMAND)) {
        console_print("error in %s: %s", cb->what, lua_tostring(L, -1));
        report_error(cb->mod, "scripts", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

static LuaCallback *make_callback(lua_State *state, int fn_index, const char *mod, const char *what) {
    LuaCallback *cb = xcalloc(1, sizeof *cb);
    lua_pushvalue(state, fn_index);
    cb->ref = luaL_ref(state, LUA_REGISTRYINDEX);
    snprintf(cb->mod, sizeof cb->mod, "%s", mod);
    snprintf(cb->what, sizeof cb->what, "%s", what);
    return cb;
}

static int l_on(lua_State *state) {
    const char *event = luaL_checkstring(state, 1);
    luaL_checktype(state, 2, LUA_TFUNCTION);
    char what[32];
    snprintf(what, sizeof what, "handler for %s", event);
    LuaCallback *cb = make_callback(state, 2, mod_of(state), what);
    int h = api_subscribe_owned(event, event_trampoline, cb, mod_of(state));
    if (!h) { luaL_unref(state, LUA_REGISTRYINDEX, cb->ref); free(cb); return luaL_error(state, "unknown event \"%s\". Valid events: tick, block_place, block_break, world_load, world_unload, command, random_tick, item_use, entity_spawn, entity_interact, entity_damage, entity_death, inventory_change, container_open, container_close, player_join, player_leave, player_damage, player_death, player_respawn, entity_tick, entity_despawn", event); }
    lua_pushinteger(state, h);
    return 1;
}

static int l_command(lua_State *state) {
    const char *name = luaL_checkstring(state, 1);
    const char *help = luaL_optstring(state, 2, "");
    luaL_checktype(state, 3, LUA_TFUNCTION);
    const char *syntax = luaL_optstring(state, 4, "");
    const char *permission = luaL_optstring(state, 5, "");
    char what[32];
    snprintf(what, sizeof what, "%s", name);
    LuaCallback *cb = make_callback(state, 3, mod_of(state), what);
    if (!api_register_command_owned(name, help, command_trampoline, cb, mod_of(state))) {
        luaL_unref(state, LUA_REGISTRYINDEX, cb->ref);
        free(cb);
        return luaL_error(state, "command \"%s\" could not be registered. Names need 1 to 23 characters without spaces and must be unused", name);
    }
    /* Metadata is accepted in the Lua signature and exposed through the native command table;
     * execution remains backward compatible with the original callback ABI. */
    (void)syntax; (void)permission;
    return 0;
}

/* ---------------------------------------------------------------- per-mod environment */

static bool read_script_file(const ModInfo *m, const char *rel, char **text, size_t *len) {
    char path[1300];
    snprintf(path, sizeof path, "%s/%s", m->dir, rel);
    *text = (char *)file_read(path, len);
    return *text != NULL;
}

static bool load_into_env(const ModInfo *m, const char *rel, int env_index) {
    char *text;
    size_t len;
    if (!read_script_file(m, rel, &text, &len)) {
        data_error(m->id, rel, 0, "script file not found. Create it, or fix \"script\" in mod.json.");
        g_errors++;
        return false;
    }
    char chunk[160];
    snprintf(chunk, sizeof chunk, "@%s", rel);
    int rc = luaL_loadbuffer(L, text, len, chunk);
    free(text);
    if (rc != 0) {
        report_error(m->id, rel, lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    lua_pushvalue(L, env_index);
    lua_setfenv(L, -2);
    return true;
}

static int l_require(lua_State *state) {
    const char *name = luaL_checkstring(state, 1);
    const ModInfo *m = mods_find(mod_of(state));
    if (!m || strstr(name, "..") || strspn(name, "abcdefghijklmnopqrstuvwxyz0123456789_./") != strlen(name))
        return luaL_error(state, "require \"%s\": module names use lowercase letters, digits, underscores and dots only", name);
    lua_pushvalue(state, lua_upvalueindex(2)); /* cache */
    lua_getfield(state, -1, name);
    if (!lua_isnil(state, -1)) return 1;
    lua_pop(state, 1);
    char rel[160];
    snprintf(rel, sizeof rel, "scripts/%s.lua", name);
    for (char *c = rel + 8; *c; c++) if (*c == '.' && strcmp(c, ".lua")) *c = '/';
    char *text;
    size_t len;
    if (!read_script_file(m, rel, &text, &len)) return luaL_error(state, "require \"%s\": %s not found in mod %s. Create the file", name, rel, m->id);
    char chunk[200];
    snprintf(chunk, sizeof chunk, "@%s", rel);
    int rc = luaL_loadbuffer(state, text, len, chunk);
    free(text);
    if (rc != 0) return lua_error(state);
    lua_pushvalue(state, lua_upvalueindex(3)); /* env */
    lua_setfenv(state, -2);
    lua_call(state, 0, 1);
    if (lua_isnil(state, -1)) { lua_pop(state, 1); lua_pushboolean(state, 1); }
    lua_pushvalue(state, -1);
    lua_setfield(state, -3, name);
    return 1;
}


/* ---------------------------------------------------------------- dfe.ui */

static int l_ui_get_status(lua_State *state) {
    const PlayerStatus *s = ui_status();
    lua_newtable(state);
#define F(k) lua_pushnumber(state, s->k); lua_setfield(state, -2, #k);
    F(health) F(max_health) F(absorption) F(hunger) F(max_hunger) F(saturation) F(stamina) F(max_stamina)
    F(magicka) F(max_magicka) F(xp) F(xp_next) F(armor) F(max_armor)
#undef F
    lua_pushinteger(state, s->level); lua_setfield(state, -2, "level");
    for (int i = 0; i < s->custom_count; i++) { lua_pushnumber(state, s->custom[i].value); lua_setfield(state, -2, s->custom[i].name); }
    return 1;
}
static int l_ui_set_status(lua_State *state) { /* dfe.ui.set_status(name, value, max) adds or updates a custom field */
    lua_pushboolean(state, ui_status_set_custom(luaL_checkstring(state, 1), (float)luaL_checknumber(state, 2), (float)luaL_optnumber(state, 3, 0)));
    return 1;
}
static int l_ui_set_element_visible(lua_State *state) { /* true/false force, nil follows the game mode */
    int st = lua_isnil(state, 2) ? -1 : lua_toboolean(state, 2);
    lua_pushboolean(state, ui_element_set_override(luaL_checkstring(state, 1), st));
    return 1;
}
static int l_ui_element_visible(lua_State *state) { lua_pushboolean(state, ui_element_visible(luaL_checkstring(state, 1))); return 1; }

static void screen_trampoline(const char *screen, const char *widget, int index, void *user) {
    LuaCallback *cb = user;
    if (!cb || cb->dead) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, cb->ref);
    lua_pushstring(L, screen);
    lua_pushstring(L, widget);
    lua_pushinteger(L, index + 1); /* Lua lists are 1-based */
    if (!call_limited(3, 0, BUDGET_HANDLER)) {
        disable_callback(cb, lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

static void callback_release(void *user) {
    LuaCallback *cb = user;
    if (!cb) return;
    if (cb->ref != LUA_NOREF && L) luaL_unref(L, LUA_REGISTRYINDEX, cb->ref);
    free(cb);
}

/* A screen's on_open and on_close callbacks, freed together with the screen. */
typedef struct ScreenCallbacks { LuaCallback *open, *close; } ScreenCallbacks;

static void screen_open_tramp(const char *screen, const char *widget, int index, void *user) { screen_trampoline(screen, widget, index, ((ScreenCallbacks *)user)->open); }
static void screen_close_tramp(const char *screen, const char *widget, int index, void *user) { screen_trampoline(screen, widget, index, ((ScreenCallbacks *)user)->close); }

static void screen_release_lua(void *user) {
    ScreenCallbacks *c = user;
    callback_release(c->open);
    callback_release(c->close);
    free(c);
}

/* Reads t[key] as a function into a callback, or NULL when absent. */
static LuaCallback *field_callback(lua_State *state, int t, const char *key, const char *mod, const char *what) {
    lua_getfield(state, t, key);
    LuaCallback *cb = NULL;
    if (lua_isfunction(state, -1)) cb = make_callback(state, lua_gettop(state), mod, what);
    lua_pop(state, 1);
    return cb;
}

static bool read_widget(lua_State *state, int t, const char *screen, const char *mod, Widget *w) {
    static const char *types[] = {"panel", "label", "button", "slot", "bar", "list", "scroll"};
    memset(w, 0, sizeof *w);
    w->enabled = true;
    const char *type = (lua_getfield(state, t, "type"), luaL_optstring(state, -1, "label"));
    int ti = -1;
    for (int i = 0; i < ARRAY_LEN(types); i++) if (!strcmp(types[i], type)) ti = i;
    lua_pop(state, 1);
    if (ti < 0) return false;
    w->type = (WidgetType)ti;
#define NUM(k, f, d) lua_getfield(state, t, k); w->f = (float)luaL_optnumber(state, -1, d); lua_pop(state, 1);
    NUM("x", x, 0) NUM("y", y, 0) NUM("w", w, 80) NUM("h", h, 18) NUM("value", value, 0) NUM("max", max, 1)
#undef NUM
    lua_getfield(state, t, "id"); snprintf(w->id, sizeof w->id, "%s", luaL_optstring(state, -1, "")); lua_pop(state, 1);
    lua_getfield(state, t, "text"); snprintf(w->text, sizeof w->text, "%s", luaL_optstring(state, -1, "")); lua_pop(state, 1);
    lua_getfield(state, t, "enabled"); if (!lua_isnil(state, -1)) w->enabled = lua_toboolean(state, -1); lua_pop(state, 1);
    lua_getfield(state, t, "items");
    if (lua_istable(state, -1)) {
        for (int i = 1; i <= WIDGET_MAX_ITEMS; i++) {
            lua_rawgeti(state, -1, i);
            if (!lua_isstring(state, -1)) { lua_pop(state, 1); break; }
            snprintf(w->items[w->item_count++], sizeof w->items[0], "%s", lua_tostring(state, -1));
            lua_pop(state, 1);
        }
    }
    lua_pop(state, 1);
    LuaCallback *cb = field_callback(state, t, "on_click", mod, "screen widget click");
    if (cb) { w->on_click = screen_trampoline; w->user = cb; w->release = callback_release; }
    (void)screen;
    return true;
}

static int l_ui_register_widget(lua_State *state) {
    const char *screen = luaL_checkstring(state, 1);
    luaL_checktype(state, 2, LUA_TTABLE);
    Widget w;
    if (!read_widget(state, 2, screen, mod_of(state), &w)) return luaL_error(state, "unknown widget type. Use panel, label, button, slot, bar, list or scroll");
    if (!screen_add_widget(screen, &w)) {
        callback_release(w.user);
        return luaL_error(state, "widget \"%s\" could not be added to screen \"%s\": the screen must exist, ids must be unique and a screen holds %d widgets", w.id, screen, SCREEN_MAX_WIDGETS);
    }
    return 0;
}

/* dfe.ui.register_screen(id, {title, w, h, modal, close_on_escape, on_open, on_close, widgets = {...}}) */
static int l_ui_register_screen(lua_State *state) {
    const char *id = luaL_checkstring(state, 1);
    luaL_checktype(state, 2, LUA_TTABLE);
    const char *mod = mod_of(state);
    ScreenDef d = {.scripted = true, .w = 240, .h = 160, .close_on_escape = true, .release = screen_release_lua};
    snprintf(d.id, sizeof d.id, "%s", id);
    lua_getfield(state, 2, "title"); snprintf(d.title, sizeof d.title, "%s", luaL_optstring(state, -1, "")); lua_pop(state, 1);
    lua_getfield(state, 2, "w"); d.w = (float)luaL_optnumber(state, -1, d.w); lua_pop(state, 1);
    lua_getfield(state, 2, "h"); d.h = (float)luaL_optnumber(state, -1, d.h); lua_pop(state, 1);
    lua_getfield(state, 2, "modal"); d.modal = lua_toboolean(state, -1); lua_pop(state, 1);
    lua_getfield(state, 2, "close_on_escape"); if (!lua_isnil(state, -1)) d.close_on_escape = lua_toboolean(state, -1); lua_pop(state, 1);
    LuaCallback *open_cb = field_callback(state, 2, "on_open", mod, "screen on_open");
    LuaCallback *close_cb = field_callback(state, 2, "on_close", mod, "screen on_close");
    ScreenCallbacks *cbs = xcalloc(1, sizeof *cbs);
    cbs->open = open_cb; cbs->close = close_cb;
    d.user = cbs;
    d.on_open = open_cb ? screen_open_tramp : NULL;
    d.on_close = close_cb ? screen_close_tramp : NULL;
    if (!screen_register(&d)) {
        screen_release_lua(cbs);
        return luaL_error(state, "screen \"%s\" could not be registered: the id must be unused and at most %d screens exist", id, SCREEN_MAX);
    }
    lua_getfield(state, 2, "widgets");
    if (lua_istable(state, -1))
        for (int i = 1;; i++) {
            lua_rawgeti(state, -1, i);
            if (!lua_istable(state, -1)) { lua_pop(state, 1); break; }
            Widget w;
            bool ok = read_widget(state, lua_gettop(state), id, mod, &w);
            if (ok && !screen_add_widget(id, &w)) { callback_release(w.user); ok = false; }
            if (!ok) { screen_unregister(id); return luaL_error(state, "screen \"%s\": widget %d is invalid (unknown type, missing or duplicate id, or too many widgets)", id, i); }
            lua_pop(state, 1);
        }
    lua_pop(state, 1);
    return 0;
}
static int l_ui_open_screen(lua_State *state) { lua_pushboolean(state, screen_open(luaL_checkstring(state, 1))); return 1; }
static int l_ui_close_screen(lua_State *state) { (void)state; lua_pushboolean(state, screen_close()); return 1; }
static int l_ui_is_screen_open(lua_State *state) { lua_pushboolean(state, screen_is_open(luaL_optstring(state, 1, NULL))); return 1; }
/* dfe.ui.set_widget(screen, id, {text, value, max, enabled}) updates a widget in place */
static int l_ui_set_widget(lua_State *state) {
    Widget *w = screen_widget(luaL_checkstring(state, 1), luaL_checkstring(state, 2));
    luaL_checktype(state, 3, LUA_TTABLE);
    if (!w) { lua_pushboolean(state, 0); return 1; }
    lua_getfield(state, 3, "text"); if (lua_isstring(state, -1)) snprintf(w->text, sizeof w->text, "%s", lua_tostring(state, -1)); lua_pop(state, 1);
    lua_getfield(state, 3, "value"); if (lua_isnumber(state, -1)) w->value = (float)lua_tonumber(state, -1); lua_pop(state, 1);
    lua_getfield(state, 3, "max"); if (lua_isnumber(state, -1)) w->max = (float)lua_tonumber(state, -1); lua_pop(state, 1);
    lua_getfield(state, 3, "enabled"); if (!lua_isnil(state, -1)) w->enabled = lua_toboolean(state, -1); lua_pop(state, 1);
    lua_pushboolean(state, 1);
    return 1;
}
static const luaL_Reg UI_FUNCS[] = {{"register_screen", l_ui_register_screen}, {"register_widget", l_ui_register_widget}, {"open_screen", l_ui_open_screen}, {"close_screen", l_ui_close_screen}, {"is_screen_open", l_ui_is_screen_open}, {"set_widget", l_ui_set_widget}, {"get_status", l_ui_get_status}, {"set_status", l_ui_set_status}, {"set_element_visible", l_ui_set_element_visible}, {"element_visible", l_ui_element_visible}, {NULL, NULL}};

/* ------------------------------------------------------------- dfe.entity */

#define LUA_ENTITY_MAX 256

static void read_vec3(lua_State *state, int idx, const char *what, double out[3]) {
    if (!lua_istable(state, idx)) luaL_error(state, "%s must be a table such as {x, y, z} or {x = 1, y = 2, z = 3}", what);
    static const char *const KEYS[3] = {"x", "y", "z"};
    for (int i = 0; i < 3; i++) {
        lua_getfield(state, idx, KEYS[i]);
        if (lua_isnil(state, -1)) { lua_pop(state, 1); lua_rawgeti(state, idx, i + 1); }
        if (!lua_isnumber(state, -1)) luaL_error(state, "%s needs three numbers (x, y, z)", what);
        out[i] = lua_tonumber(state, -1);
        lua_pop(state, 1);
    }
}

static void push_entity(lua_State *state, const dfe_entity_t *e) {
    lua_createtable(state, 0, 16);
    lua_pushinteger(state, e->id); lua_setfield(state, -2, "id");
    lua_pushstring(state, e->type); lua_setfield(state, -2, "type");
    lua_pushnumber(state, e->pos[0]); lua_setfield(state, -2, "x");
    lua_pushnumber(state, e->pos[1]); lua_setfield(state, -2, "y");
    lua_pushnumber(state, e->pos[2]); lua_setfield(state, -2, "z");
    lua_pushnumber(state, e->vel[0]); lua_setfield(state, -2, "vx");
    lua_pushnumber(state, e->vel[1]); lua_setfield(state, -2, "vy");
    lua_pushnumber(state, e->vel[2]); lua_setfield(state, -2, "vz");
    lua_pushnumber(state, e->yaw); lua_setfield(state, -2, "yaw");
    lua_pushnumber(state, e->health); lua_setfield(state, -2, "health");
    lua_pushnumber(state, e->max_health); lua_setfield(state, -2, "max_health");
    lua_pushnumber(state, e->age); lua_setfield(state, -2, "age");
    lua_pushstring(state, e->behaviour); lua_setfield(state, -2, "behaviour");
    lua_pushstring(state, e->data); lua_setfield(state, -2, "data");
}

/* Copies the fields present in the table at idx into e and returns the mask. Unknown or mistyped fields are errors,
 * so a misspelt key does not silently do nothing. */
static uint32_t read_entity_fields(lua_State *state, int idx, dfe_entity_t *e) {
    uint32_t mask = 0;
    luaL_checktype(state, idx, LUA_TTABLE);
    lua_pushnil(state);
    while (lua_next(state, idx)) {
        const char *k = lua_type(state, -2) == LUA_TSTRING ? lua_tostring(state, -2) : "";
        if (!strcmp(k, "x") || !strcmp(k, "y") || !strcmp(k, "z")) { e->pos[k[0] - 'x'] = luaL_checknumber(state, -1); mask |= DFE_ENTITY_POS; }
        else if (!strcmp(k, "vx") || !strcmp(k, "vy") || !strcmp(k, "vz")) { e->vel[k[1] - 'x'] = luaL_checknumber(state, -1); mask |= DFE_ENTITY_VEL; }
        else if (!strcmp(k, "yaw")) { e->yaw = (float)luaL_checknumber(state, -1); mask |= DFE_ENTITY_YAW; }
        else if (!strcmp(k, "health")) { e->health = (float)luaL_checknumber(state, -1); mask |= DFE_ENTITY_HEALTH; }
        else if (!strcmp(k, "max_health")) { e->max_health = (float)luaL_checknumber(state, -1); mask |= DFE_ENTITY_HEALTH; }
        else if (!strcmp(k, "behaviour")) { snprintf(e->behaviour, sizeof e->behaviour, "%s", luaL_checkstring(state, -1)); mask |= DFE_ENTITY_BEHAVIOUR; }
        else if (!strcmp(k, "data")) { snprintf(e->data, sizeof e->data, "%s", luaL_checkstring(state, -1)); mask |= DFE_ENTITY_DATA; }
        else if (strcmp(k, "id") && strcmp(k, "type") && strcmp(k, "age")) {
            return (uint32_t)luaL_error(state, "unknown entity field \"%s\". Settable fields: x, y, z, vx, vy, vz, yaw, health, max_health, behaviour, data", k);
        }
        lua_pop(state, 1);
    }
    return mask;
}

/* dfe.entity.spawn(type, pos, opts) returns the entity id, or nil and a reason. opts uses the field names of set();
 * it is read before anything spawns, so a mistake in it cannot leave a half-made entity behind. */
static int l_ent_spawn(lua_State *state) {
    const char *type = luaL_checkstring(state, 1);
    double p[3];
    read_vec3(state, 2, "position", p);
    dfe_entity_t e;
    memset(&e, 0, sizeof e);
    e.health = 1e30f; /* unnamed health means full health, set() clamps it to the maximum */
    uint32_t mask = lua_istable(state, 3) ? read_entity_fields(state, 3, &e) : 0;
    int id = api_get()->entity_spawn(type, p[0], p[1], p[2]); /* x, y, z in opts are ignored; pos is the position */
    if (!id) { lua_pushnil(state); lua_pushfstring(state, "could not spawn \"%s\": unknown type, entity limit reached or cancelled by an entity_spawn handler", type); return 2; }
    mask &= ~DFE_ENTITY_POS;
    if (mask) api_get()->entity_set(id, &e, mask);
    lua_pushinteger(state, id);
    return 1;
}

static int l_ent_despawn(lua_State *state) { lua_pushboolean(state, api_get()->entity_remove((int)luaL_checkinteger(state, 1))); return 1; }

static int l_ent_get(lua_State *state) {
    dfe_entity_t e;
    if (!api_get()->entity_get((int)luaL_checkinteger(state, 1), &e)) { lua_pushnil(state); return 1; }
    push_entity(state, &e);
    return 1;
}

static int l_ent_set(lua_State *state) {
    int id = (int)luaL_checkinteger(state, 1);
    dfe_entity_t e;
    if (!api_get()->entity_get(id, &e)) { lua_pushboolean(state, 0); return 1; }
    uint32_t mask = read_entity_fields(state, 2, &e); /* starts from the current state, so {x = 5} keeps y and z */
    lua_pushboolean(state, api_get()->entity_set(id, &e, mask));
    return 1;
}

static int l_ent_damage(lua_State *state) { lua_pushboolean(state, api_get()->entity_damage((int)luaL_checkinteger(state, 1), (float)luaL_checknumber(state, 2))); return 1; }
static int l_ent_heal(lua_State *state) { lua_pushboolean(state, api_get()->entity_heal((int)luaL_checkinteger(state, 1), (float)luaL_checknumber(state, 2))); return 1; }

/* The iterator state is a snapshot of the ids taken when iter() is called, so a loop body may remove or spawn
 * entities. Entities removed during the loop are skipped. */
static int ent_iter_next(lua_State *state) {
    int i = (int)lua_tointeger(state, lua_upvalueindex(2)) + 1;
    int n = (int)lua_objlen(state, lua_upvalueindex(1));
    for (; i <= n; i++) {
        lua_rawgeti(state, lua_upvalueindex(1), i);
        int id = (int)lua_tointeger(state, -1);
        lua_pop(state, 1);
        dfe_entity_t e;
        if (!api_get()->entity_get(id, &e)) continue;
        lua_pushinteger(state, i);
        lua_replace(state, lua_upvalueindex(2));
        lua_pushinteger(state, id);
        push_entity(state, &e);
        return 2;
    }
    return 0;
}

static int l_ent_iter(lua_State *state) {
    int ids[LUA_ENTITY_MAX];
    int n = MIN(api_get()->entity_list(ids, LUA_ENTITY_MAX), LUA_ENTITY_MAX);
    lua_createtable(state, n, 0);
    for (int i = 0; i < n; i++) { lua_pushinteger(state, ids[i]); lua_rawseti(state, -2, i + 1); }
    lua_pushinteger(state, 0);
    lua_pushcclosure(state, ent_iter_next, 2);
    return 1;
}

/* dfe.entity.near(pos, radius) returns an array of entity tables, nearest first, each with a distance field. */
static int l_ent_near(lua_State *state) {
    double p[3];
    read_vec3(state, 1, "position", p);
    double radius = luaL_checknumber(state, 2);
    int ids[LUA_ENTITY_MAX];
    int n = MIN(api_get()->entity_near(p[0], p[1], p[2], radius, ids, LUA_ENTITY_MAX), LUA_ENTITY_MAX);
    lua_createtable(state, n, 0);
    int out = 0;
    for (int i = 0; i < n; i++) {
        dfe_entity_t e;
        if (!api_get()->entity_get(ids[i], &e)) continue;
        push_entity(state, &e);
        lua_pushnumber(state, sqrt((e.pos[0] - p[0]) * (e.pos[0] - p[0]) + (e.pos[1] - p[1]) * (e.pos[1] - p[1]) + (e.pos[2] - p[2]) * (e.pos[2] - p[2])));
        lua_setfield(state, -2, "distance");
        lua_rawseti(state, -2, ++out);
    }
    return 1;
}

static int l_ent_on(lua_State *state) {
    const char *event = luaL_checkstring(state, 1);
    if (strncmp(event, "entity_", 7)) return luaL_error(state, "dfe.entity.on takes an entity event: entity_spawn, entity_tick, entity_damage, entity_death or entity_despawn (use dfe.on for others)");
    return l_on(state);
}

static const luaL_Reg ENTITY_FUNCS[] = {
    {"spawn", l_ent_spawn}, {"despawn", l_ent_despawn}, {"get", l_ent_get}, {"set", l_ent_set}, {"damage", l_ent_damage}, {"heal", l_ent_heal},
    {"iter", l_ent_iter}, {"near", l_ent_near}, {"on", l_ent_on}, {NULL, NULL}};

static const luaL_Reg DFE_FUNCS[] = {
    {"log", l_log}, {"console", l_console}, {"block_state", l_block_state}, {"block_name", l_block_name}, {"state_name", l_state_name},
    {"get_state", l_get_state}, {"get_block", l_get_block}, {"set_block", l_set_block}, {"get_light", l_get_light},
    {"entity_spawn", l_entity_spawn}, {"entity_remove", l_entity_remove}, {"entity_position", l_entity_position}, {"entity_count", l_entity_count},
    {"seed", l_seed}, {"time", l_time}, {"player_info", l_player_info}, {"player_state", l_player_state}, {"item_count", l_item_count}, {"item_add", l_item_add}, {"inventory_count", l_inventory_count}, {"recipe_count", l_recipe_count}, {"recipe_craft", l_recipe_craft}, {"recipe_process", l_recipe_process}, {"loot_roll", l_loot_roll}, {"container_count", l_container_count}, {"container_add", l_container_add}, {"on", l_on}, {"command", l_command}, {NULL, NULL}};

/* Gives the mod its own copy of a standard library by running the library's open function again. LuaJIT reuses a
 * library table that is already registered under the same name, so the registry entry and the global are cleared first
 * to force a fresh table. luaopen_string also replaces the state-wide string metatable, so the original is put back:
 * method calls on strings ("x"):upper() keep resolving through one table that no per-mod copy can redirect. */
static void install_private_lib(int env, lua_CFunction open_fn, const char *name) {
    lua_getfield(L, LUA_REGISTRYINDEX, "_LOADED");
    lua_pushnil(L);
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
    lua_pushnil(L);
    lua_setfield(L, LUA_GLOBALSINDEX, name);
    lua_pushcfunction(L, open_fn);
    lua_pushstring(L, name);
    lua_call(L, 1, 1);
    lua_setfield(L, env, name);
    lua_pushnil(L);
    lua_setfield(L, LUA_GLOBALSINDEX, name); /* the open function registers a global; the real globals stay empty */
    lua_getfield(L, LUA_REGISTRYINDEX, "_LOADED");
    lua_pushnil(L);
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
    lua_pushliteral(L, "");
    lua_getfield(L, LUA_REGISTRYINDEX, "dfe_string_meta");
    lua_setmetatable(L, -2);
    lua_pop(L, 1);
}

/* Pushes a new global table for a mod and returns its registry reference. */
static int make_mod_env(const char *mod_id) {
    lua_newtable(L);                     /* env */
    int env = lua_gettop(L);
    lua_createtable(L, 0, 2);            /* metatable */
    lua_getfield(L, LUA_REGISTRYINDEX, "dfe_safe_globals");
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, env);
    lua_newtable(L);                     /* dfe table */
    for (const luaL_Reg *r = DFE_FUNCS; r->name; r++) {
        lua_pushstring(L, mod_id);
        lua_pushcclosure(L, r->func, 1);
        lua_setfield(L, -2, r->name);
    }
    lua_newtable(L);                     /* dfe.entity */
    for (const luaL_Reg *r = ENTITY_FUNCS; r->name; r++) {
        lua_pushstring(L, mod_id);
        lua_pushcclosure(L, r->func, 1);
        lua_setfield(L, -2, r->name);
    }
    lua_setfield(L, -2, "entity");
    lua_newtable(L);                     /* dfe.ui */
    for (const luaL_Reg *r = UI_FUNCS; r->name; r++) { lua_pushstring(L, mod_id); lua_pushcclosure(L, r->func, 1); lua_setfield(L, -2, r->name); }
    lua_setfield(L, -2, "ui");
    lua_newtable(L);
    lua_pushstring(L, mod_id);
    lua_pushcclosure(L, l_storage_get, 1);
    lua_setfield(L, -2, "get");
    lua_pushstring(L, mod_id);
    lua_pushcclosure(L, l_storage_set, 1);
    lua_setfield(L, -2, "set");
    lua_pushstring(L, mod_id);
    lua_pushcclosure(L, l_storage_remove, 1);
    lua_setfield(L, -2, "remove");
    lua_setfield(L, -2, "storage");
    lua_pushstring(L, mod_id);
    lua_setfield(L, -2, "mod");
    lua_setfield(L, env, "dfe");
    lua_pushstring(L, mod_id);
    lua_pushcclosure(L, l_print, 1);
    lua_setfield(L, env, "print");
    lua_pushstring(L, mod_id);
    lua_newtable(L);                     /* require cache */
    lua_pushvalue(L, env);
    lua_pushcclosure(L, l_require, 3);
    lua_setfield(L, env, "require");
    lua_pushvalue(L, env);
    lua_setfield(L, env, "_G");
    /* A mod that edits string, table or math changes only its own copy. */
    install_private_lib(env, luaopen_string, LUA_STRLIBNAME);
    install_private_lib(env, luaopen_table, LUA_TABLIBNAME);
    install_private_lib(env, luaopen_math, LUA_MATHLIBNAME);
    return luaL_ref(L, LUA_REGISTRYINDEX);
}

static int env_for(const char *mod_id) {
    for (int i = 0; i < g_mod_env_count; i++) if (!strcmp(g_mod_env_id[i], mod_id)) return g_mod_env_ref[i];
    if (g_mod_env_count >= ARRAY_LEN(g_mod_env_ref)) return LUA_NOREF;
    int ref = make_mod_env(mod_id);
    snprintf(g_mod_env_id[g_mod_env_count], sizeof g_mod_env_id[0], "%s", mod_id);
    g_mod_env_ref[g_mod_env_count++] = ref;
    return ref;
}

static void copy_global(const char *name) {
    lua_getglobal(L, name);
    lua_setfield(L, -2, name);
}

bool script_init(void) {
    if (L) return true;
    L = lua_newstate(limited_alloc, NULL);
    if (!L) { LOGE("could not create the Lua state"); return false; }
    luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF); /* the budget hook needs the interpreter anyway */
    lua_pushcfunction(L, luaopen_base); lua_pushstring(L, ""); lua_call(L, 1, 0);
    lua_pushcfunction(L, luaopen_table); lua_pushstring(L, LUA_TABLIBNAME); lua_call(L, 1, 0);
    lua_pushcfunction(L, luaopen_string); lua_pushstring(L, LUA_STRLIBNAME); lua_call(L, 1, 0);
    lua_pushcfunction(L, luaopen_math); lua_pushstring(L, LUA_MATHLIBNAME); lua_call(L, 1, 0);
    lua_pushcfunction(L, luaopen_bit); lua_pushstring(L, LUA_BITLIBNAME); lua_call(L, 1, 0);
    /* The whitelist: everything a mod sees comes from this table. */
    lua_newtable(L);
    static const char *const globals[] = {"assert", "error", "ipairs", "pairs", "next", "pcall", "xpcall", "select", "tonumber", "tostring", "type",
                                          "unpack", "rawget", "rawset", "rawequal", "setmetatable", "getmetatable"};
    for (int i = 0; i < ARRAY_LEN(globals); i++) copy_global(globals[i]);
    /* string, table and math are installed per mod in make_mod_env; bit has no mutable state worth copying and stays shared. */
    copy_global("bit");
    lua_setfield(L, LUA_REGISTRYINDEX, "dfe_safe_globals");
    lua_pushliteral(L, "");
    lua_getmetatable(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, "dfe_string_meta");
    lua_pop(L, 1);
    lua_getglobal(L, "tostring");
    lua_setfield(L, LUA_REGISTRYINDEX, "dfe_tostring");
    /* Empty the real global table so nothing can reach the unsafe functions through it. */
    lua_pushvalue(L, LUA_GLOBALSINDEX);
    int g = lua_gettop(L);
    lua_pushnil(L);
    while (lua_next(L, g)) {
        lua_pop(L, 1);
        lua_pushvalue(L, -1);
        lua_pushnil(L);
        lua_rawset(L, g);
    }
    lua_pop(L, 1);
    return true;
}

void script_shutdown(void) {
    if (!L) return;
    screen_remove_scripted();
    events_clear_all();
    for (int i = 0; i < g_mod_env_count; i++) luaL_unref(L, LUA_REGISTRYINDEX, g_mod_env_ref[i]);
    lua_close(L);
    L = NULL;
    g_mem = 0;
    g_mod_env_count = 0;
    g_errors = 0;
}

int script_error_count(void) { return g_errors; }

int script_load_mods(void) {
    if (!L && !script_init()) return 1;
    int before = g_errors;
    for (int i = 0; i < mods_loaded_count(); i++) {
        const ModInfo *m = mods_loaded_at(i);
        if (!m->script[0]) continue;
        int ref = env_for(m->id);
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        int env = lua_gettop(L);
        int errors_here = g_errors;
        if (load_into_env(m, m->script, env) && !call_limited(0, 0, BUDGET_LOAD)) {
            report_error(m->id, m->script, lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_settop(L, env - 1);
        if (g_errors != errors_here) {
            events_clear(m->id);
            LOGW("[mod %s] its script failed, so its handlers and commands were removed. The rest of the mod still loads.", m->id);
        }
    }
    return g_errors - before;
}

bool script_run_mod_test(const char *mod_id, const char *relative_path) {
    if (!L && !script_init()) return false;
    const ModInfo *m = mods_find(mod_id);
    if (!m || !relative_path || strncmp(relative_path, "tests/", 6)) return false;
    int ref = env_for(m->id);
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    int env = lua_gettop(L);
    int before = g_errors;
    bool ok = load_into_env(m, relative_path, env);
    if (ok && !call_limited(0, 0, BUDGET_LOAD)) {
        report_error(m->id, relative_path, lua_tostring(L, -1));
        lua_pop(L, 1);
        ok = false;
    }
    lua_settop(L, env - 1);
    return ok && g_errors == before;
}

/* Evaluates a console line as an expression first, then as a statement, and prints the results. */
void script_eval(const char *code) {
    if (!L && !script_init()) return;
    int ref = env_for("console");
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    int env = lua_gettop(L);
    char *expr = xstrfmt("return %s", code);
    int rc = luaL_loadbuffer(L, expr, strlen(expr), "=console");
    free(expr);
    if (rc != 0) {
        lua_pop(L, 1);
        rc = luaL_loadbuffer(L, code, strlen(code), "=console");
    }
    if (rc != 0) { console_print("%s", lua_tostring(L, -1)); lua_settop(L, env - 1); return; }
    lua_pushvalue(L, env);
    lua_setfenv(L, -2);
    int base = lua_gettop(L) - 1;
    if (!call_limited(0, LUA_MULTRET, BUDGET_COMMAND)) { console_print("%s", lua_tostring(L, -1)); lua_settop(L, env - 1); return; }
    for (int i = base + 1; i <= lua_gettop(L); i++) {
        lua_getfield(L, LUA_REGISTRYINDEX, "dfe_tostring");
        lua_pushvalue(L, i);
        lua_pcall(L, 1, 1, 0);
        console_print("%s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_settop(L, env - 1);
}
