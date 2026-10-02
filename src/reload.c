/* Hot reload for the things that are safe to swap while the game runs: shaders, the atmosphere file and presets.
 *
 * Blocks, textures and scripts are not reloaded. Block ids are baked into every loaded chunk and the texture array
 * is built once, so changing either live would corrupt the world; a restart is the honest answer and takes seconds.
 * Detection polls file times through the virtual filesystem, so it also sees edits inside mod folders. */
#include "dfe.h"

#define POLL_INTERVAL_S 0.5

static bool report(const char *what, bool ok, int first_error) {
    if (ok) { LOGI("reloaded %s", what); return true; }
    LOGE("reload of %s failed, keeping the previous version", what);
    for (int i = first_error; i < data_error_count() && i < first_error + 3; i++) LOGE("  %s", data_error_text(i));
    return false;
}

bool hot_reload_now(void) {
    bool all_ok = true;
    bool gl = g_win.handle != NULL;
    if (gl) {
        bool ok = scene_reload_shaders() && post_reload_shaders() && atmosphere_gl_reload_shaders();
        all_ok &= report("shaders", ok, data_error_count());
    }
    int before = data_error_count();
    all_ok &= report("atmosphere", atmosphere_reload_data() == 0, before);
    before = data_error_count();
    all_ok &= report("presets", registry_load_presets() == 0, before);
    gfx_apply();
    return all_ok;
}

void hot_reload_poll(double now_s, bool enabled) {
    static double next_poll;
    static u64 stamp;
    if (!enabled || now_s < next_poll) return;
    next_poll = now_s + POLL_INTERVAL_S;
    u64 current = vfs_stamp();
    if (!stamp) { stamp = current; return; }
    if (current == stamp) return;
    stamp = current;
    hot_reload_now();
    stamp = vfs_stamp(); /* files read during the reload join the watch list */
}
