/* Defective Engine native plugin API, version 1.
 *
 * This header is the only stable surface a native plugin may rely on. Lua scripts share
 * these core world/event calls and also have additional Lua-only helpers.
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
#include <stddef.h>
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
/* One entity as seen by plugins (API 1.3). type is a namespaced id, behaviour one of "wander", "static", "hostile",
 * "passive". data is free text the plugin keeps with the entity; it is saved with the world. */
#define DFE_ENTITY_POS 1u
#define DFE_ENTITY_VEL 2u
#define DFE_ENTITY_YAW 4u
#define DFE_ENTITY_HEALTH 8u
#define DFE_ENTITY_BEHAVIOUR 16u
#define DFE_ENTITY_DATA 32u
typedef struct dfe_entity_t {
    int id;
    char type[64];
    double pos[3], vel[3];
    float yaw, health, max_health, age;
    char behaviour[16];
    char data[192];
} dfe_entity_t;

typedef struct dfe_event_t {
    const char *name;
    int x, y, z;
    uint16_t state;
    double dt;
    const char *text;
    int entity_id;
    float damage;
} dfe_event_t;

/* Returns nonzero to cancel a cancellable event or to mark a command as handled. */
typedef int (*dfe_event_fn)(const dfe_event_t *ev, void *user);
/* args is the text after the command name, with leading blanks removed. The reply goes to the console. */
typedef void (*dfe_command_fn)(const char *args, void *user);

typedef struct dfe_api_t {
    uint32_t abi_version;
    uint32_t struct_size;

    void (*log)(dfe_log_level level, const char *mod_id, const char *message);

    /* Block lookup. block_state takes a namespaced name such as "base:stone" and returns its default state, or
     * DFE_STATE_UNLOADED when no loaded mod defines it. A state with properties is selected with a bracket
     * suffix: "mymod:lamp[lit=on]" or "mymod:door[half=upper,open=false]". Unknown properties and values fail. */
    uint16_t (*block_state)(const char *name);
    const char *(*block_name)(uint16_t state);

    /* World access in block coordinates. get_state returns DFE_STATE_UNLOADED outside loaded terrain. */
    uint16_t (*get_state)(int x, int y, int z);
    bool (*set_state)(int x, int y, int z, uint16_t state);
    /* Writes sky, red, green, blue light (0..15 each) to out[4]. Returns false if unloaded. */
    bool (*get_light)(int x, int y, int z, uint8_t out[4]);
    uint64_t (*world_seed)(void);
    double (*game_time)(void); /* seconds of simulated time since the world was created */

    /* Returns a handle (>0) or 0 when the event name is unknown. The plugin owns
     * user and must keep it valid until removal; the engine never frees it. */
    int (*subscribe)(const char *event, dfe_event_fn fn, void *user, const char *mod_id);
    /* The plugin owns user and must free it after the command is removed or
     * during plugin shutdown; the engine never frees it. */
    int (*register_command)(const char *name, const char *help, dfe_command_fn fn, void *user, const char *mod_id);
    /* Writes a line to the in-game console. */
    void (*console_print)(const char *message);

    /* Added in API 1.1 (struct_size covers it when >= offsetof(dfe_api_t, state_string) + sizeof(void *)).
     * Writes the canonical text of a state, such as "base:stone" or "mymod:lamp[lit=on]", into out. Returns false
     * for an invalid state or when size is 0; the text is truncated to fit size. */
    bool (*state_string)(uint16_t state, char *out, size_t size);

    /* Added in API 1.2 (struct_size covers it when >= offsetof(dfe_api_t, entity_count) + sizeof(void *)).
     * entity_spawn takes a type id such as "base:hopper" and a feet position in blocks. It returns a handle (>0),
     * or 0 when the type is unknown or the entity limit is reached; the reason is logged. Handles are never reused. */
    int (*entity_spawn)(const char *type, double x, double y, double z);
    bool (*entity_remove)(int handle);
    /* Writes the feet position to out[3]. Returns false for a handle that no longer exists. */
    bool (*entity_position)(int handle, double out[3]);
    int (*entity_count)(void);

    /* Added as an optional world-save-backed mod storage facility. This is JSON state under the active world save,
     * namespaced by mod id. The JSON is stored alongside the world, not in a global registry. */
    bool (*mod_storage_get)(const char *mod_id, const char *key, char *out, size_t size);
    bool (*mod_storage_set)(const char *mod_id, const char *key, const char *json_value);
    bool (*mod_storage_remove)(const char *mod_id, const char *key);

    /* Added in API 1.3 (struct_size covers it when >= offsetof(dfe_api_t, entity_near) + sizeof(void *)). All of
     * these run on the main thread only. Handles are the ones entity_spawn returns and stay valid until the entity
     * is removed; they are never reused. Entity events (entity_spawn, entity_tick, entity_damage, entity_death,
     * entity_despawn) set entity_id and, for entity_damage, damage; returning nonzero cancels them.
     * entity_get fills out and returns false for an unknown handle. entity_set writes only the fields named in mask
     * (DFE_ENTITY_*); position is a teleport that fails inside blocks, a health of 0 or less kills the entity. A
     * type without health ignores health, damage and heal. entity_damage honours the invulnerability timer and
     * entity_death cancellation and returns true only when health dropped. entity_list writes up to cap handles in
     * spawn order and returns the total. entity_near writes up to cap handles within radius blocks, nearest first,
     * and returns how many matched. The engine never frees or reads plugin user pointers passed to subscribe. */
    bool (*entity_get)(int handle, dfe_entity_t *out);
    bool (*entity_set)(int handle, const dfe_entity_t *in, uint32_t mask);
    bool (*entity_damage)(int handle, float amount);
    bool (*entity_heal)(int handle, float amount);
    int (*entity_list)(int *out, int cap);
    int (*entity_near)(double x, double y, double z, double radius, int *out, int cap);
} dfe_api_t;

#ifdef __cplusplus
}
#endif
#endif
