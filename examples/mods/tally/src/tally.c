/* Block Tally: a native plugin built against include/dfe_api.h only.
 *
 *   tally x1 y1 z1 x2 y2 z2     count the blocks in a box by name, most common first
 *   tally                       show how many player edits happened since the world loaded
 *
 * A native plugin runs with the full privileges of the game and is loaded only when the game is started
 * with --allow-native. Prefer Lua unless the work needs native speed or a library; this mod exists to show
 * the shape of the C API: an init function, a command and event subscriptions. */
#include "dfe_api.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_VOLUME 262144 /* blocks scanned by one command, a fraction of a second of work */
#define MAX_KINDS 256     /* distinct block states tracked in one scan */
#define REPORT_ROWS 8
#define LINE_CAPACITY 256

static const dfe_api_t *g_api;
static long g_placed, g_broken;

typedef struct Kind {
    uint16_t state;
    long count;
} Kind;

static void say(const char *format, ...) {
    char line[LINE_CAPACITY];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    g_api->console_print(line);
}

static int by_count_descending(const void *a, const void *b) {
    long ca = ((const Kind *)a)->count, cb = ((const Kind *)b)->count;
    return (cb > ca) - (cb < ca);
}

static int swap_if_greater(int *lo, int *hi) {
    if (*lo > *hi) {
        int t = *lo;
        *lo = *hi;
        *hi = t;
    }
    return *hi - *lo + 1;
}

/* Adds one block to the tally, growing the table of distinct kinds. Returns 0 when the table is full. */
static int count_state(Kind *kinds, int *n, uint16_t state) {
    for (int i = 0; i < *n; i++) {
        if (kinds[i].state == state) {
            kinds[i].count++;
            return 1;
        }
    }
    if (*n >= MAX_KINDS) return 0;
    kinds[*n].state = state;
    kinds[*n].count = 1;
    (*n)++;
    return 1;
}

static void report(const Kind *kinds, int n, long unloaded, long total) {
    say("%ld blocks scanned, %ld unloaded, %d kinds", total, unloaded, n);
    for (int i = 0; i < n && i < REPORT_ROWS; i++) {
        const char *name = g_api->block_name(kinds[i].state);
        say("  %-24s %ld", name ? name : "unknown", kinds[i].count);
    }
}

static void command_tally(const char *args, void *user) {
    (void)user;
    int x1, y1, z1, x2, y2, z2;
    if (!*args) {
        say("player edits since the world loaded: %ld placed, %ld broken", g_placed, g_broken);
        return;
    }
    if (sscanf(args, "%d %d %d %d %d %d", &x1, &y1, &z1, &x2, &y2, &z2) != 6) {
        say("usage: tally <x1> <y1> <z1> <x2> <y2> <z2>");
        return;
    }
    long volume = (long)swap_if_greater(&x1, &x2) * swap_if_greater(&y1, &y2) * swap_if_greater(&z1, &z2);
    if (volume > MAX_VOLUME) {
        say("that box holds %ld blocks; the limit is %d. Use a smaller box", volume, MAX_VOLUME);
        return;
    }
    Kind *kinds = malloc(sizeof(Kind) * MAX_KINDS);
    if (!kinds) {
        say("out of memory");
        return;
    }
    int n = 0;
    long unloaded = 0;
    for (int x = x1; x <= x2; x++)
        for (int y = y1; y <= y2; y++)
            for (int z = z1; z <= z2; z++) {
                uint16_t state = g_api->get_state(x, y, z);
                if (state == DFE_STATE_UNLOADED) unloaded++;
                else if (!count_state(kinds, &n, state)) {
                    say("more than %d distinct block states in that box; use a smaller box", MAX_KINDS);
                    free(kinds);
                    return;
                }
            }
    qsort(kinds, (size_t)n, sizeof(Kind), by_count_descending);
    report(kinds, n, unloaded, volume);
    free(kinds);
}

static int on_place(const dfe_event_t *ev, void *user) {
    (void)ev; (void)user;
    g_placed++;
    return 0; /* never cancel: this plugin only observes */
}

static int on_break(const dfe_event_t *ev, void *user) {
    (void)ev; (void)user;
    g_broken++;
    return 0;
}

static int on_world_load(const dfe_event_t *ev, void *user) {
    (void)ev; (void)user;
    g_placed = g_broken = 0;
    return 0;
}

DFE_PLUGIN_EXPORT int dfe_plugin_init(const dfe_api_t *api, const char *mod_id) {
    if (api->abi_version != DFE_API_VERSION) return 1;
    g_api = api;
    api->subscribe("block_place", on_place, NULL, mod_id);
    api->subscribe("block_break", on_break, NULL, mod_id);
    api->subscribe("world_load", on_world_load, NULL, mod_id);
    if (!api->register_command("tally", "tally [x1 y1 z1 x2 y2 z2]", command_tally, NULL, mod_id)) return 1;
    api->log(DFE_LOG_INFO, mod_id, "block tally ready");
    return 0;
}

DFE_PLUGIN_EXPORT void dfe_plugin_shutdown(void) {
    g_api = NULL;
}
