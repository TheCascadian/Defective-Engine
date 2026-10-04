/* Hot reload for the things that are safe to swap while the game runs: shaders, the atmosphere file and presets.
 *
 * Content registries and scripts are reloaded transactionally. Block ids, textures and compiled renderer assets
 * remain stable because they are baked into live chunks and meshes; changing those live would corrupt the world.
 * Detection polls file times through the virtual filesystem, so it also sees edits inside mod folders. */
#include "dfe.h"
#include "ui.h"
#include "icons.h"

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
        bool ok = scene_reload_shaders() && post_reload_shaders() && entity_reload_shaders() && atmosphere_gl_reload_shaders();
        all_ok &= report("shaders", ok, data_error_count());
    }
    int before = data_error_count();
    all_ok &= report("atmosphere", atmosphere_reload_data() == 0, before);
    before = data_error_count();
    all_ok &= report("presets", registry_load_presets() == 0, before);
    before = data_error_count();
    all_ok &= report("entity types", registry_load_entities() == 0, before);
    all_ok &= report("hud layout", ui_reload(), data_error_count());
    all_ok &= report("ui icons", icons_reload(), data_error_count());
    before = data_error_count();
    all_ok &= report("content", content_reload() == 0, before);
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
