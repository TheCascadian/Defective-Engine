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
    int h = api_get()->subscribe(event, event_trampoline, cb, mod_of(state));
    if (!h) { luaL_unref(state, LUA_REGISTRYINDEX, cb->ref); free(cb); return luaL_error(state, "unknown event \"%s\". Valid events: tick, block_place, block_break, world_load, world_unload, command, random_tick", event); }
    lua_pushinteger(state, h);
    return 1;
}

static int l_command(lua_State *state) {
    const char *name = luaL_checkstring(state, 1);
    const char *help = luaL_optstring(state, 2, "");
    luaL_checktype(state, 3, LUA_TFUNCTION);
    char what[32];
    snprintf(what, sizeof what, "%s", name);
    LuaCallback *cb = make_callback(state, 3, mod_of(state), what);
    if (!api_get()->register_command(name, help, command_trampoline, cb, mod_of(state))) {
        luaL_unref(state, LUA_REGISTRYINDEX, cb->ref);
        free(cb);
        return luaL_error(state, "command \"%s\" could not be registered. Names need 1 to 23 characters without spaces and must be unused", name);
    }
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

static const luaL_Reg DFE_FUNCS[] = {
    {"log", l_log}, {"console", l_console}, {"block_state", l_block_state}, {"block_name", l_block_name}, {"state_name", l_state_name},
    {"get_state", l_get_state}, {"get_block", l_get_block}, {"set_block", l_set_block}, {"get_light", l_get_light},
    {"entity_spawn", l_entity_spawn}, {"entity_remove", l_entity_remove}, {"entity_position", l_entity_position}, {"entity_count", l_entity_count},
    {"seed", l_seed}, {"time", l_time}, {"on", l_on}, {"command", l_command}, {NULL, NULL}};

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
