/* Defective Engine native plugin API, version 1.
 *
 * This header is the only stable surface a native plugin may rely on. The Lua bindings
 * are written against the same dfe_api_t table, so anything Lua can do a plugin can do.
 *
 * Compatibility rules:
 *  - Fields are only ever appended. Existing fields never change type or meaning.
 *  - struct_size tells a plugin which fields exist. Check it before using a field that
 *    was added after the API version the plugin was built against.
 *  - abi_version changes only on a breaking change, which also bumps the major number
 *    that mod manifests declare in their "api" field.
 *
 * A plugin is a shared library exporting:
 *     int  dfe_plugin_init(const dfe_api_t *api, const char *mod_id);   returns 0 on success
 *     void dfe_plugin_shutdown(void);                                  optional
 *
 * Plugins run unsandboxed with the privileges of the game, so the engine loads them only
 * when started with --allow-native and the mod manifest declares them. */
#ifndef DFE_API_H
#define DFE_API_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DFE_API_VERSION 1
#define DFE_STATE_UNLOADED 0xFFFF

#if defined(_WIN32)
#define DFE_PLUGIN_EXPORT __declspec(dllexport)
#else
#define DFE_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

typedef enum { DFE_LOG_DEBUG, DFE_LOG_INFO, DFE_LOG_WARN, DFE_LOG_ERROR } dfe_log_level;

/* Payload shared by every event. Which fields are meaningful depends on the event:
 *   "tick"          dt (seconds, always 0.05)
 *   "block_place"   x y z state (the state about to be placed). Return 1 to cancel.
 *   "block_break"   x y z state (the state about to be removed). Return 1 to cancel.
 *   "world_load"    nothing
 *   "world_unload"  nothing
 *   "command"       text (the full line) after no command matched. Return 1 if handled. */
typedef struct dfe_event_t {
    const char *name;
    int x, y, z;
    uint16_t state;
    double dt;
    const char *text;
} dfe_event_t;

/* Returns nonzero to cancel a cancellable event or to mark a command as handled. */
typedef int (*dfe_event_fn)(const dfe_event_t *ev, void *user);
/* args is the text after the command name, with leading blanks removed. The reply goes to the console. */
typedef void (*dfe_command_fn)(const char *args, void *user);

typedef struct dfe_api_t {
    uint32_t abi_version;
    uint32_t struct_size;

    void (*log)(dfe_log_level level, const char *mod_id, const char *message);

    /* Block lookup. block_state returns the default state of a namespaced name such as
     * "base:stone", or DFE_STATE_UNLOADED when no loaded mod defines it. */
    uint16_t (*block_state)(const char *name);
    const char *(*block_name)(uint16_t state);

    /* World access in block coordinates. get_state returns DFE_STATE_UNLOADED outside loaded terrain. */
    uint16_t (*get_state)(int x, int y, int z);
    bool (*set_state)(int x, int y, int z, uint16_t state);
    /* Writes sky, red, green, blue light (0..15 each) to out[4]. Returns false if unloaded. */
    bool (*get_light)(int x, int y, int z, uint8_t out[4]);
    uint64_t (*world_seed)(void);
    double (*game_time)(void); /* seconds of simulated time since the world was created */

    /* Returns a handle (>0) or 0 when the event name is unknown. */
    int (*subscribe)(const char *event, dfe_event_fn fn, void *user, const char *mod_id);
    int (*register_command)(const char *name, const char *help, dfe_command_fn fn, void *user, const char *mod_id);
    /* Writes a line to the in-game console. */
    void (*console_print)(const char *message);
} dfe_api_t;

#ifdef __cplusplus
}
#endif
#endif
