/* Menus: the title screen with the world list, the pause menu and the options screen.
 *
 * Everything is immediate mode on top of the 2D layer: each frame a widget is drawn and tells the caller whether it
 * was clicked, so there is no widget tree to keep in sync with the data. The look follows Minecraft's: bevelled grey
 * buttons, a tiled dirt backdrop, a blocky stone logo and white text with a dark drop shadow. Every size is written in
 * "GUI pixels" and multiplied by g_s, an integer scale picked from the window size the way Minecraft's auto scale is.
 * Settings are applied as they change so the player sees the effect behind the menu, and are written to disk when the
 * screen is left. */
#include "dfe.h"
#include "ui_widgets.h"
#include <GLFW/glfw3.h>
#include <time.h>

#define FIELD_CAP 40
#define SCREEN_FRAME_MS 16
#define DOUBLE_CLICK_S 0.4
#define WIDE_W 200.0f /* the standard Minecraft button width */
#define HALF_W 98.0f
#define BTN_H 20.0f
#define BTN_PITCH 24.0f
#define ROW_H 36.0f
#define LIST_W 220.0f

typedef enum MenuScreen { SCREEN_NONE, SCREEN_PAUSE, SCREEN_SETTINGS } MenuScreen;

static MenuScreen g_screen = SCREEN_NONE;
static bool g_quit_requested;
static bool g_return_to_title_requested;
static bool g_settings_live; /* true when the world exists, so gfx_apply may push values to the scene */
static int g_s = 1;          /* GUI scale */
static UIWState g_settings_ui;

#define U(n) ((float)(n) * (float)g_s)

/* Render distance choices; 0 follows the preset. */
static const int RD_CHOICES[] = {0, 4, 6, 8, 10, 12, 16, 20, 24, 32};
static const int DYN_CHOICES[] = {-1, 1, 0}; /* automatic, on, off */
#define FOV_MIN 50
#define FOV_MAX 110
#define FOV_STEP 5
#define SCALE_MIN 0.5f
#define SCALE_MAX 1.0f
#define SCALE_STEP 0.05f

#define COL_WHITE rgba(255, 255, 255, 255)
#define COL_GREY rgba(160, 160, 160, 255)
#define COL_YELLOW rgba(255, 255, 160, 255)
#define COL_RED rgba(255, 85, 85, 255)
#define COL_BLACK rgba(0, 0, 0, 255)

static void gui_scale(int width, int height) {
    int automatic = CLAMP(MIN(width / 320, height / 360), 1, 4);
    /* Automatic mode adapts to the viewport. An explicit player choice is authoritative:
     * oversized menus are intentional and must never be silently reduced to fit. */
    g_s = g_settings.ui_scale < 0 ? automatic : CLAMP(g_settings.ui_scale, 1, 4);
}

/* ----------------------------------------------------------- look and feel */

static u32 hash_u32(u32 x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}

#define TILE_TEXELS 16
static GLuint g_dirt_tex, g_grass_tex;

/* The two backdrop tiles are generated rather than shipped: a base colour with hashed speckles, point-sampled. */
static GLuint make_tile(int r, int g, int b, u32 salt, int spread) {
    u8 px[TILE_TEXELS * TILE_TEXELS * 4];
    for (int y = 0; y < TILE_TEXELS; y++)
        for (int x = 0; x < TILE_TEXELS; x++) {
            u32 h = hash_u32((u32)(x * 73856093) ^ (u32)(y * 19349663) ^ salt);
            int d = (int)(h % (u32)(spread * 2 + 1)) - spread;
            if ((h >> 8) % 9 == 0) d -= spread; /* darker pits */
            u8 *p = px + (y * TILE_TEXELS + x) * 4;
            p[0] = (u8)CLAMP(r + d, 0, 255);
            p[1] = (u8)CLAMP(g + d, 0, 255);
            p[2] = (u8)CLAMP(b + d * 3 / 4, 0, 255);
            p[3] = 255;
        }
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, TILE_TEXELS, TILE_TEXELS, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    return tex;
}

static void ensure_tiles(void) {
    if (g_dirt_tex) return;
    g_dirt_tex = make_tile(134, 96, 67, 0x1234u, 14);
    g_grass_tex = make_tile(92, 150, 52, 0x9876u, 12);
}

void menu_gl_shutdown(void) {
    if (g_dirt_tex) glDeleteTextures(1, &g_dirt_tex);
    if (g_grass_tex) glDeleteTextures(1, &g_grass_tex);
    g_dirt_tex = g_grass_tex = 0;
}

/* Fills a rectangle with a repeating 32 GUI px tile, tinted by `tint`. */
static void tile_fill(GLuint tex, float x, float y, float w, float h, u32 tint) {
    float tile = U(32);
    ui_image(tex, x, y, w, h, 0, 0, w / tile, h / tile, tint);
}

static void dirt_background(int width, int height, int shade) {
    ensure_tiles();
    tile_fill(g_dirt_tex, 0, 0, (float)width, (float)height, rgba(shade, shade, shade, 255));
}

static bool hovered(float x, float y, float w, float h) {
    return g_in.mouse_x >= x && g_in.mouse_x < x + w && g_in.mouse_y >= y && g_in.mouse_y < y + h;
}

static bool clicked(float x, float y, float w, float h, int button) { return g_in.mouse_pressed[button] && hovered(x, y, w, h); }

static float text_size(void) { return 8.0f * (float)g_s; }

/* Menu text relies on the engine-wide shadow rule in ui_text. */
static void mc_text(float x, float y, float size, u32 color, const char *text) { ui_text(x, y, size, color, text); }

static void mc_text_centered(float cx, float y, float size, u32 color, const char *text) {
    mc_text(cx - ui_text_width(size, text) * 0.5f, y, size, color, text);
}

static bool button(float x, float y, float w, const char *label, bool enabled) {
    return uiw_button(0, (UIWRect){x, y, w, U(BTN_H)}, label, enabled, text_size());
}

static int index_of_int(const int *list, int count, int value) {
    for (int i = 0; i < count; i++) if (list[i] == value) return i;
    return 0;
}

static int wrap(int i, int count) { return ((i % count) + count) % count; }

/* ------------------------------------------------------------------- logo */

#define GLYPH_W 5
#define GLYPH_H 7

static const char *glyph_rows(char c) {
    switch (c) {
    case 'U': return "#...#" "#...#" "#...#" "#...#" "#...#" "#...#" ".###.";
    case 'N': return "#...#" "##..#" "##..#" "#.#.#" "#..##" "#..##" "#...#";
    case 'B': return "####." "#...#" "#...#" "####." "#...#" "#...#" "####.";
    case 'O': return ".###." "#...#" "#...#" "#...#" "#...#" "#...#" ".###.";
    case 'D': return "####." "#...#" "#...#" "#...#" "#...#" "#...#" "####.";
    }
    return NULL;
}

static float logo_word_width(const char *word, float cell) {
    return ((float)strlen(word) * (GLYPH_W + 1) - 1) * cell;
}

/* Draws one word of the logo as bevelled stone blocks. The shadow pass goes first so it sits under every block. */
static void logo_word(float x, float y, const char *word, float cell) {
    for (int pass = 0; pass < 2; pass++) {
        float ox = x;
        for (const char *c = word; *c; c++, ox += (GLYPH_W + 1) * cell) {
            const char *g = glyph_rows(*c);
            if (!g) continue;
            for (int gy = 0; gy < GLYPH_H; gy++)
                for (int gx = 0; gx < GLYPH_W; gx++) {
                    if (g[gy * GLYPH_W + gx] != '#') continue;
                    float bx = ox + (float)gx * cell, by = y + (float)gy * cell;
                    if (pass == 0) { ui_rect(bx + cell * 0.35f, by + cell * 0.35f, cell, cell, rgba(0, 0, 0, 90)); continue; }
                    int shade = 118 + (int)(hash_u32((u32)(bx * 7.0f) ^ (u32)(by * 13.0f) * 31u) % 28u);
                    float b = MAX(1.0f, cell * 0.12f);
                    ui_rect(bx, by, cell, cell, rgba(34, 34, 34, 255));
                    ui_rect(bx + b, by + b, cell - 2 * b, cell - 2 * b, rgba(shade, shade, shade, 255));
                    ui_rect(bx + b, by + b, cell - 2 * b, b, rgba(shade + 50, shade + 50, shade + 50, 255));
                    ui_rect(bx + b, by + b, b, cell - 2 * b, rgba(shade + 36, shade + 36, shade + 36, 255));
                    ui_rect(bx + b, by + cell - 2 * b, cell - 2 * b, b, rgba(shade - 46, shade - 46, shade - 46, 255));
                    ui_rect(bx + cell - 2 * b, by + b, b, cell - 2 * b, rgba(shade - 36, shade - 36, shade - 36, 255));
                }
        }
    }
}

#define LOGO_WORD "UNBOUND"

/* Returns the y just below the logo. */
static float draw_logo(float cx, float y, const char *splash) {
    float cell = U(6);
    float w = logo_word_width(LOGO_WORD, cell);
    logo_word(cx - w * 0.5f, y, LOGO_WORD, cell);
    float pulse = 1.0f + 0.06f * sinf((float)time_now_s() * 6.0f);
    mc_text_centered(cx + w * 0.5f - U(30), y + GLYPH_H * cell - U(4), text_size() * 1.3f * pulse, rgba(255, 255, 0, 255), splash);
    return y + GLYPH_H * cell;
}

/* -------------------------------------------------------- text input field */

static void text_field(float cx, float y, int id, const char *label, char *value, bool active, bool names_only) {
    float w = U(WIDE_W), x = cx - w * 0.5f, size = text_size();
    uiw_text_field(id, (UIWRect){x, y, w, U(BTN_H)}, label, value, FIELD_CAP, active, names_only, size);
}

/* ---------------------------------------------------------- options screen */

static void settings_changed(void) {
    if (g_settings_live) gfx_apply();
    if (g_win.vsync != g_settings.vsync && !g_opt.no_vsync) window_set_vsync(g_settings.vsync);
}

static void step_preset(int step) {
    int n = preset_count();
    if (n <= 0) return;
    int cur = 0;
    for (int i = 0; i < n; i++) if (!strcmp(preset_at(i)->id, g_settings.preset)) cur = i;
    snprintf(g_settings.preset, sizeof g_settings.preset, "%s", preset_at(wrap(cur + step, n))->id);
}

static void step_render_distance(int step) {
    int n = ARRAY_LEN(RD_CHOICES);
    g_settings.render_distance = RD_CHOICES[wrap(index_of_int(RD_CHOICES, n, g_settings.render_distance) + step, n)];
}

static void step_dynamic(int step) {
    int n = ARRAY_LEN(DYN_CHOICES);
    g_settings.dynamic_resolution = DYN_CHOICES[wrap(index_of_int(DYN_CHOICES, n, g_settings.dynamic_resolution) + step, n)];
}

static void step_ui_scale(int step) {
    static const int choices[] = {-1, 1, 2, 3, 4};
    int cur = 0;
    for (int i = 0; i < ARRAY_LEN(choices); i++) if (g_settings.ui_scale == choices[i]) cur = i;
    g_settings.ui_scale = choices[wrap(cur + step, ARRAY_LEN(choices))];
}

static const char *ui_scale_label(void) {
    static char label[8];
    if (g_settings.ui_scale < 0) return "Auto";
    snprintf(label, sizeof label, "%dx", g_settings.ui_scale);
    return label;
}

/* Position 0 follows the preset; 1..n are the shadow levels, low to high. */
static void step_shadow_quality(int step) {
    int n = shadow_level_count() + 1, cur = 0;
    for (int i = 0; i < shadow_level_count(); i++) if (!strcmp(shadow_level_at(i)->id, g_settings.shadow_quality)) cur = i + 1;
    cur = wrap(cur + step, n);
    snprintf(g_settings.shadow_quality, sizeof g_settings.shadow_quality, "%s", cur ? shadow_level_at(cur - 1)->id : "");
}

static void step_godray_quality(int step) {
    int n = godray_level_count() + 1, cur = 0;
    for (int i = 0; i < godray_level_count(); i++) if (!strcmp(godray_level_at(i)->id, g_settings.godray_quality)) cur = i + 1;
    cur = wrap(cur + step, n);
    snprintf(g_settings.godray_quality, sizeof g_settings.godray_quality, "%s", cur ? godray_level_at(cur - 1)->id : "");
}

static void step_fog_quality(int step) {
    int n = fog_level_count() + 1, cur = 0;
    for (int i = 0; i < fog_level_count(); i++) if (!strcmp(fog_level_at(i)->id, g_settings.fog_quality)) cur = i + 1;
    cur = wrap(cur + step, n);
    snprintf(g_settings.fog_quality, sizeof g_settings.fog_quality, "%s", cur ? fog_level_at(cur - 1)->id : "");
}

static void step_fov(int step) { g_settings.fov_deg = (float)CLAMP((int)g_settings.fov_deg + step * FOV_STEP, FOV_MIN, FOV_MAX); }

/* One "Label: value" button. Left click advances, right click goes back. */
static int option_button(int id, float x, float y, const char *label, const char *value) {
    return uiw_cycle_row(id, (UIWRect){x, y, U(150), U(BTN_H)}, label, value, true, text_size());
}

static void screen_title(float cx, float y, const char *title) { mc_text_centered(cx, y, text_size() * 1.2f, COL_WHITE, title); }

/* These pages are sub-screens of Options, so the title menu and pause menu share the same flow. */
static bool g_in_controls;
static bool g_in_graphics;

/* Data page: wiping every world needs two separate confirmations. Stage 0 is the page, 1 the first warning, 2 the last. */
static bool g_in_data;
static int g_wipe_stage;
static bool g_worlds_stale; /* set when worlds were deleted outside the world list, so it reloads from disk */
static char g_data_message[96];

static int count_worlds(void) {
    StrList all = {0};
    dir_list("saves", &all);
    int n = 0;
    for (int i = 0; i < all.n; i++) {
        char path[300];
        snprintf(path, sizeof path, "saves/%s", all.d[i]);
        if (all.d[i][0] != '.' && path_is_dir(path)) n++;
    }
    strlist_free(&all);
    return n;
}

static void delete_all_worlds(void) {
    StrList all = {0};
    dir_list("saves", &all);
    int failed = 0, removed = 0;
    for (int i = 0; i < all.n; i++) {
        char path[300];
        if (all.d[i][0] == '.' || strchr(all.d[i], '/') || strchr(all.d[i], '\\')) continue;
        snprintf(path, sizeof path, "saves/%s", all.d[i]);
        if (!path_is_dir(path)) continue;
        if (dir_remove_all(path)) removed++; else failed++;
    }
    strlist_free(&all);
    g_worlds_stale = true;
    if (failed) snprintf(g_data_message, sizeof g_data_message, "Deleted %d worlds; %d could not be fully removed.", removed, failed);
    else snprintf(g_data_message, sizeof g_data_message, "Deleted %d worlds.", removed);
}

static bool data_rows(float cx, int height) {
    float bx = cx - U(WIDE_W) * 0.5f;
    int n = count_worlds();
    if (g_wipe_stage == 1 || g_wipe_stage == 2) {
        char line[128];
        screen_title(cx, U(40), g_wipe_stage == 1 ? "Delete ALL worlds?" : "Are you absolutely sure?");
        snprintf(line, sizeof line, g_wipe_stage == 1 ? "This will remove all %d saved worlds." : "Last chance: %d worlds will be lost forever. This cannot be undone.", n);
        mc_text_centered(cx, U(62), text_size(), COL_RED, line);
        float y = U(100);
        if (uiw_button_action(g_wipe_stage == 1 ? 14 : 15, (UIWRect){bx, y, U(WIDE_W), U(BTN_H)},
                              g_wipe_stage == 1 ? "Yes, continue" : "Delete Everything", true, text_size(), COL_RED) == 1) {
            if (g_wipe_stage == 1) g_wipe_stage = 2;
            else { delete_all_worlds(); g_wipe_stage = 0; }
            return false;
        }
        y += U(BTN_PITCH);
        if (button(bx, y, U(WIDE_W), "Cancel", true)) g_wipe_stage = 0;
        return false;
    }
    screen_title(cx, U(15), "Data");
    float y = U(50);
    if (g_settings_live) {
        mc_text_centered(cx, y, text_size() * 0.9f, COL_GREY, "Available from the title screen only, while no world is open.");
    } else {
        char label[64];
        snprintf(label, sizeof label, "Delete All Worlds (%d)", n);
        if (button(bx, y, U(WIDE_W), label, n > 0)) { g_wipe_stage = 1; g_data_message[0] = 0; }
        if (g_data_message[0]) mc_text_centered(cx, y + U(BTN_PITCH), text_size() * 0.9f, COL_GREY, g_data_message);
    }
    float done_y = MAX(y + U(BTN_PITCH) + U(30), (float)height - U(34));
    if (button(bx, done_y, U(WIDE_W), "Done", true)) { g_in_data = false; g_data_message[0] = 0; }
    return false;
}

static bool controls_rows(float cx, int height) {
    screen_title(cx, U(15), "Controls");
    UIWStack rows = uiw_vstack((UIWRect){cx - U(155), U(40), U(310), 0}, U(4));
    UIWRect row = uiw_stack_next(&rows, U(BTN_H));
    UIWStack cells = uiw_hstack(row, U(10));
    UIWRect left = uiw_stack_next(&cells, (row.w - U(10)) * 0.5f);
    UIWRect right = uiw_stack_next(&cells, (row.w - U(10)) * 0.5f);
    if (option_button(7, left.x, left.y, "Auto-Jump", g_settings.auto_jump_off ? "Off" : "On")) g_settings.auto_jump_off = !g_settings.auto_jump_off;
    if (option_button(8, right.x, right.y, "View Bobbing", g_settings.view_bob_off ? "Off" : "On")) g_settings.view_bob_off = !g_settings.view_bob_off;
    row = uiw_stack_next(&rows, U(BTN_H));
    cells = uiw_hstack(row, U(10));
    left = uiw_stack_next(&cells, (row.w - U(10)) * 0.5f);
    if (option_button(9, left.x, left.y, "Motion Effects", g_settings.motion_fx_off ? "Off" : "On")) g_settings.motion_fx_off = !g_settings.motion_fx_off;
    float y = rows.cursor - rows.gap + U(2);
    mc_text_centered(cx, y, text_size() * 0.85f, COL_GREY, "Motion Effects: landing dip, sprint view widening and smooth stepping.");
    float done_y = MAX(y + U(30), (float)height - U(34));
    if (button(cx - U(WIDE_W) * 0.5f, done_y, U(WIDE_W), "Done", true)) g_in_controls = false;
    return false;
}

/* Draws the graphics screen centred on cx and returns true when Done was pressed. */
static bool graphics_rows(float cx, int height) {
    if (g_in_controls) return controls_rows(cx, height);
    char v[64];
    bool changed = false;
    const Preset *p = preset_find(g_settings.preset);
    int step;
    screen_title(cx, U(15), "Graphics Settings");
    UIWStack rows = uiw_vstack((UIWRect){cx - U(155), U(40), U(310), 0}, U(4));
    UIWRect row = uiw_stack_next(&rows, U(BTN_H));
    UIWStack cells = uiw_hstack(row, U(10));
    UIWRect left = uiw_stack_next(&cells, (row.w - U(10)) * 0.5f);
    UIWRect right = uiw_stack_next(&cells, (row.w - U(10)) * 0.5f);
    if ((step = option_button(1, left.x, left.y, "Preset", p ? p->name : g_settings.preset))) { step_preset(step); changed = true; }
    if (g_settings.render_distance > 0) snprintf(v, sizeof v, "%d chunks", g_settings.render_distance);
    else snprintf(v, sizeof v, "preset (%d)", p ? p->render_distance : 0);
    if ((step = option_button(2, right.x, right.y, "Render Distance", v))) { step_render_distance(step); changed = true; }

    row = uiw_stack_next(&rows, U(30));
    cells = uiw_hstack(row, U(10));
    left = uiw_stack_next(&cells, (row.w - U(10)) * 0.5f);
    right = uiw_stack_next(&cells, (row.w - U(10)) * 0.5f);
    snprintf(v, sizeof v, "%s", g_settings.dynamic_resolution < 0 ? "Preset" : g_settings.dynamic_resolution ? "On" : "Off");
    if ((step = option_button(3, left.x, left.y, "Dynamic Res", v))) { step_dynamic(step); changed = true; }
    snprintf(v, sizeof v, "%d%%", (int)(g_settings.render_scale * 100.0f + 0.5f));
    mc_text(right.x, right.y, text_size() * 0.9f, COL_WHITE, "Render Scale");
    mc_text(right.x + right.w - ui_text_width(text_size() * 0.9f, v), right.y, text_size() * 0.9f, COL_WHITE, v);
    UIWRect slider = {right.x, right.y + text_size(), right.w, right.h - text_size()};
    if (uiw_slider(&g_settings_ui, 4, slider, &g_settings.render_scale, SCALE_MIN, SCALE_MAX, SCALE_STEP)) changed = true;
    uiw_tooltip(right, "Render scale (0.50 to 1.00)", text_size());

    row = uiw_stack_next(&rows, U(BTN_H));
    cells = uiw_hstack(row, U(10));
    left = uiw_stack_next(&cells, (row.w - U(10)) * 0.5f);
    right = uiw_stack_next(&cells, (row.w - U(10)) * 0.5f);
    snprintf(v, sizeof v, "%d", (int)g_settings.fov_deg);
    if ((step = option_button(5, left.x, left.y, "FOV", v))) { step_fov(step); changed = true; }
    if ((step = option_button(6, right.x, right.y, "VSync", g_settings.vsync ? "On" : "Off"))) { g_settings.vsync = !g_settings.vsync; changed = true; }
    row = uiw_stack_next(&rows, U(BTN_H));
    cells = uiw_hstack(row, U(10));
    left = uiw_stack_next(&cells, (row.w - U(10)) * 0.5f);
    right = uiw_stack_next(&cells, (row.w - U(10)) * 0.5f);
    if (option_button(10, left.x, left.y, "Shadows", g_settings.shadows_off ? "Off" : "On")) { g_settings.shadows_off = !g_settings.shadows_off; changed = true; }
    const ShadowLevel *sl = g_settings.shadow_quality[0] ? shadow_level_find(g_settings.shadow_quality) : NULL;
    const Preset *cur_preset = preset_find(g_settings.preset);
    const ShadowLevel *auto_level = cur_preset ? shadow_level_find(cur_preset->shadows) : NULL;
    if (sl) snprintf(v, sizeof v, "%s", sl->name);
    else snprintf(v, sizeof v, "Preset (%s)", auto_level ? auto_level->name : "none");
    if ((step = option_button(11, right.x, right.y, "Shadow Quality", v))) { step_shadow_quality(step); changed = true; }
    row = uiw_stack_next(&rows, U(BTN_H));
    cells = uiw_hstack(row, U(10));
    left = uiw_stack_next(&cells, (row.w - U(10)) * 0.5f);
    right = uiw_stack_next(&cells, (row.w - U(10)) * 0.5f);
    if (option_button(20, left.x, left.y, "Godrays", g_settings.godrays_off ? "Off" : "On")) {
        g_settings.godrays_off = !g_settings.godrays_off;
        /* Turning them on under a preset that has none picks the lowest level, so the button always does something. */
        const Preset *pr = preset_find(g_settings.preset);
        if (!g_settings.godrays_off && !g_settings.godray_quality[0] && godray_level_count() && pr && !pr->light_shafts && !pr->godrays[0])
            snprintf(g_settings.godray_quality, sizeof g_settings.godray_quality, "%s", godray_level_at(0)->id);
        changed = true;
    }
    const GodrayLevel *gq = g_settings.godray_quality[0] ? godray_level_find(g_settings.godray_quality) : NULL;
    const GodrayLevel *auto_gl = cur_preset ? godray_level_find(cur_preset->godrays) : NULL;
    if (!auto_gl && cur_preset && cur_preset->light_shafts && godray_level_count()) auto_gl = godray_level_at(godray_level_count() / 2);
    if (gq) snprintf(v, sizeof v, "%s", gq->name);
    else snprintf(v, sizeof v, "Preset (%s)", auto_gl ? auto_gl->name : "none");
    if ((step = option_button(21, right.x, right.y, "Godray Quality", v))) { step_godray_quality(step); changed = true; }
    row = uiw_stack_next(&rows, U(BTN_H));
    cells = uiw_hstack(row, U(10));
    left = uiw_stack_next(&cells, (row.w - U(10)) * 0.5f);
    right = uiw_stack_next(&cells, (row.w - U(10)) * 0.5f);
    if (option_button(22, left.x, left.y, "Fog", g_settings.fog_off ? "Off" : "On")) {
        g_settings.fog_off = !g_settings.fog_off;
        const Preset *pr = preset_find(g_settings.preset);
        if (!g_settings.fog_off && !g_settings.fog_quality[0] && fog_level_count() && pr && (!pr->fog || !pr->fog_quality[0]))
            snprintf(g_settings.fog_quality, sizeof g_settings.fog_quality, "%s", fog_level_at(fog_level_count() / 2)->id);
        changed = true;
    }
    const FogLevel *fq = g_settings.fog_quality[0] ? fog_level_find(g_settings.fog_quality) : NULL;
    const FogLevel *auto_fq = cur_preset ? fog_level_find(cur_preset->fog_quality) : NULL;
    if (!auto_fq && cur_preset && cur_preset->fog && fog_level_count()) auto_fq = fog_level_at(fog_level_count() / 2);
    if (fq) snprintf(v, sizeof v, "%s", fq->name);
    else snprintf(v, sizeof v, "Preset (%s)", auto_fq ? auto_fq->name : "none");
    if ((step = option_button(23, right.x, right.y, "Fog Quality", v))) { step_fog_quality(step); changed = true; }
    float y = rows.cursor - rows.gap + U(2);
    mc_text_centered(cx, y, text_size() * 0.85f, COL_GREY, "Right click steps backwards. Render scale applies while dynamic resolution is off.");
    if (changed) settings_changed();
    float done_y = MAX(y + U(22), (float)height - U(34));
    return button(cx - U(WIDE_W) * 0.5f, done_y, U(WIDE_W), "Done", true);
}

/* The options hub keeps navigation and player-facing preferences separate from rendering controls. */
static bool settings_rows(float cx, int height) {
    if (g_in_controls) return controls_rows(cx, height);
    if (g_in_graphics) return graphics_rows(cx, height);
    if (g_in_data) return data_rows(cx, height);
    screen_title(cx, U(15), "Options");
    UIWStack rows = uiw_vstack((UIWRect){cx - U(155), U(40), U(310), 0}, U(4));
    UIWRect row = uiw_stack_next(&rows, U(BTN_H));
    UIWStack cells = uiw_hstack(row, U(10));
    UIWRect left = uiw_stack_next(&cells, (row.w - U(10)) * 0.5f);
    UIWRect right = uiw_stack_next(&cells, (row.w - U(10)) * 0.5f);
    if (option_button(24, left.x, left.y, "UI Scale", ui_scale_label())) {
        step_ui_scale(1);
        settings_changed();
        gui_scale(g_win.width, g_win.height);
    }
    if (button(right.x, right.y, right.w, "Graphics Settings...", true)) g_in_graphics = true;
    row = uiw_stack_next(&rows, U(BTN_H));
    cells = uiw_hstack(row, U(10));
    left = uiw_stack_next(&cells, (row.w - U(10)) * 0.5f);
    right = uiw_stack_next(&cells, (row.w - U(10)) * 0.5f);
    if (button(left.x, left.y, left.w, "Controls...", true)) g_in_controls = true;
    if (button(right.x, right.y, right.w, "Data...", true)) g_in_data = true;
    mc_text_centered(cx, rows.cursor + U(5), text_size() * 0.85f, COL_GREY, "UI scale is applied immediately and saved when leaving Options.");
    float done_y = MAX(rows.cursor + U(24), (float)height - U(34));
    return button(cx - U(WIDE_W) * 0.5f, done_y, U(WIDE_W), "Done", true);
}

static void leave_settings(void) {
    g_in_controls = false;
    g_in_graphics = false;
    g_in_data = false;
    g_wipe_stage = 0;
    /* Benchmarks never reach a menu, but the guard keeps a script from overwriting a player's file by accident. */
    if (!g_opt.benchmark) settings_save();
}

/* ------------------------------------------------------------- pause menu */

bool menu_is_open(void) { return g_screen != SCREEN_NONE; }

bool menu_quit_requested(void) { return g_quit_requested; }
bool menu_return_to_title_requested(void) { return g_return_to_title_requested; }

void menu_set_open(bool open) {
    if (open == menu_is_open()) return;
    g_screen = open ? SCREEN_PAUSE : SCREEN_NONE;
    g_settings_live = true;
    window_set_cursor_captured(!open);
}

/* Escape steps back one level: options to pause, pause to the game. */
static bool settings_step_back(void) {
    if (g_in_data && g_wipe_stage) { g_wipe_stage = 0; return true; }
    if (g_in_data) { g_in_data = false; return true; }
    if (g_in_controls) { g_in_controls = false; return true; }
    if (g_in_graphics) { g_in_graphics = false; return true; }
    return false;
}

void menu_back(void) {
    if (g_screen == SCREEN_SETTINGS && settings_step_back()) return;
    else if (g_screen == SCREEN_SETTINGS) { leave_settings(); g_screen = SCREEN_PAUSE; }
    else menu_set_open(false);
}

void menu_draw(int width, int height) {
    if (!menu_is_open()) return;
    gui_scale(width, height);
    float cx = (float)width * 0.5f, bx = cx - U(WIDE_W) * 0.5f;
    ui_rect_gradient(0, 0, (float)width, (float)height, rgba(16, 16, 16, 192), rgba(16, 16, 16, 208));
    if (g_screen == SCREEN_PAUSE) {
        float y = (float)height * 0.25f + U(24);
        screen_title(cx, y - U(34), "Game Menu");
        if (button(bx, y, U(WIDE_W), "Back to Game", true)) menu_set_open(false);
        y += U(BTN_PITCH);
        if (button(bx, y, U(WIDE_W), "Options...", true)) g_screen = SCREEN_SETTINGS;
        y += U(BTN_PITCH) + U(8);
        if (button(bx, y, U(WIDE_W), "Save and Exit to Menu", true)) {
            g_return_to_title_requested = true;
            window_request_close();
        }
        y += U(BTN_PITCH);
        if (button(bx, y, U(WIDE_W), "Save and Quit Game", true)) { g_quit_requested = true; window_request_close(); }
    } else if (settings_rows(cx, height)) {
        leave_settings();
        g_screen = SCREEN_PAUSE;
    }
}

/* -------------------------------------------------------------- title menu */

typedef enum TitleScreen { TS_MAIN, TS_WORLDS, TS_CREATE, TS_EDIT, TS_DELETE, TS_OPTIONS } TitleScreen;

typedef struct WorldInfo {
    char seed[32];
    char date[24];
} WorldInfo;

typedef struct TitleState {
    TitleScreen screen;
    StrList worlds;
    VEC(WorldInfo) info;
    int selected; /* index into worlds, -1 for none */
    int scroll;   /* first visible row */
    int last_click;
    double last_click_time;
    char name[FIELD_CAP], seed[FIELD_CAP];
    int field; /* 0 name, 1 seed */
    char message[160];
    const char *splash;
} TitleState;

static const char *SPLASHES[] = {"Block by block!", "Now with voxels!", "Snow friction!", "Moddable!", "Written in C!", "Chunky!"};

static void read_world_info(const char *name, WorldInfo *out) {
    memset(out, 0, sizeof *out);
    char path[320];
    snprintf(path, sizeof path, "saves/%s/world.json", name);
    snprintf(out->seed, sizeof out->seed, "unknown");
    size_t size = 0;
    u8 *text = file_read(path, &size);
    if (text) {
        char err[64];
        Json *j = json_parse((const char *)text, size, err, sizeof err, NULL);
        if (j) {
            snprintf(out->seed, sizeof out->seed, "%.0f", json_num(j, "seed", 0));
            json_free(j);
        }
        free(text);
    }
    i64 mt = path_mtime(path);
    if (mt >= 0) {
        time_t t = (time_t)mt;
        struct tm *tm = localtime(&t);
        if (tm) strftime(out->date, sizeof out->date, "%Y-%m-%d %H:%M", tm);
    }
}

static void refresh_worlds(TitleState *t, const char *select) {
    strlist_free(&t->worlds);
    vec_free(t->info);
    StrList all = {0};
    dir_list("saves", &all);
    for (int i = 0; i < all.n; i++) {
        char path[300];
        snprintf(path, sizeof path, "saves/%s", all.d[i]);
        if (all.d[i][0] != '.' && path_is_dir(path)) {
            WorldInfo wi;
            read_world_info(all.d[i], &wi);
            vec_push(t->info, wi);
            vec_push(t->worlds, xstrdup(all.d[i]));
        }
    }
    strlist_free(&all);
    t->selected = -1;
    for (int i = 0; select && i < t->worlds.n; i++) if (!strcmp(t->worlds.d[i], select)) t->selected = i;
    t->scroll = CLAMP(t->scroll, 0, MAX(0, t->worlds.n - 1));
}

/* A numeric seed is used as is; any other text is hashed, so a word works as a seed and always gives the same world. */
static u64 seed_from_text(const char *s) {
    char *end = NULL;
    u64 n = strtoull(s, &end, 10);
    if (*s && end && !*end) return n;
    u64 h = 1469598103934665603ull;
    for (; *s; s++) { h ^= (u8)*s; h *= 1099511628211ull; }
    return h;
}

static void go(TitleState *t, TitleScreen screen) {
    t->screen = screen;
    t->message[0] = 0;
    if (g_worlds_stale) { g_worlds_stale = false; refresh_worlds(t, NULL); }
}

static const char *selected_name(const TitleState *t) { return t->selected >= 0 && t->selected < t->worlds.n ? t->worlds.d[t->selected] : NULL; }

/* The world list. Returns true when a world was chosen to play. */
static bool title_worlds(TitleState *t, int width, int height, char *world_out, size_t cap) {
    float cx = (float)width * 0.5f;
    float top = U(36), bottom = (float)height - U(64);
    int rows = MAX(1, (int)((bottom - top) / U(ROW_H)));
    int max_scroll = MAX(0, t->worlds.n - rows);
    if (g_in.scroll != 0) t->scroll += g_in.scroll > 0 ? -1 : 1;
    if (key_pressed(GLFW_KEY_DOWN) && t->worlds.n) t->selected = MIN(t->selected + 1, t->worlds.n - 1);
    if (key_pressed(GLFW_KEY_UP) && t->worlds.n) t->selected = MAX(t->selected - 1, 0);
    if (t->selected >= 0) {
        if (t->selected < t->scroll) t->scroll = t->selected;
        if (t->selected >= t->scroll + rows) t->scroll = t->selected - rows + 1;
    }
    t->scroll = CLAMP(t->scroll, 0, max_scroll);

    dirt_background(width, height, 64);
    ui_rect(0, top, (float)width, bottom - top, rgba(0, 0, 0, 110));
    ui_rect_gradient(0, top, (float)width, U(4), rgba(0, 0, 0, 200), rgba(0, 0, 0, 0));
    ui_rect_gradient(0, bottom - U(4), (float)width, U(4), rgba(0, 0, 0, 0), rgba(0, 0, 0, 200));
    ensure_tiles();

    float x = cx - U(LIST_W) * 0.5f, size = text_size();
    bool play = false;
    ui_clip(0, (int)top, width, (int)(bottom - top));
    for (int i = 0; i < rows && t->scroll + i < t->worlds.n; i++) {
        int idx = t->scroll + i;
        float y = top + U(4) + (float)i * U(ROW_H);
        float h = U(ROW_H) - U(4);
        if (idx == t->selected) {
            ui_rect(x - U(1), y - U(1), U(LIST_W) + U(2), h + U(2), COL_WHITE);
            ui_rect(x, y, U(LIST_W), h, COL_BLACK);
        }
        ui_image(g_grass_tex, x + U(1), y + U(1), h - U(2), h - U(2), 0, 0, 1, 1, COL_WHITE);
        mc_text(x + h + U(3), y + U(2), size, COL_WHITE, t->worlds.d[idx]);
        char line[96];
        snprintf(line, sizeof line, "Seed: %s", t->info.d[idx].seed);
        mc_text(x + h + U(3), y + U(2) + size + U(1), size * 0.9f, COL_GREY, line);
        mc_text(x + h + U(3), y + U(2) + size * 1.9f + U(2), size * 0.9f, COL_GREY, t->info.d[idx].date);
        if (clicked(x, y, U(LIST_W), h, GLFW_MOUSE_BUTTON_LEFT) && hovered(0, top, (float)width, bottom - top)) {
            double now = time_now_s();
            if (t->last_click == idx && now - t->last_click_time < DOUBLE_CLICK_S) play = true;
            t->selected = idx;
            t->last_click = idx;
            t->last_click_time = now;
        }
    }
    ui_clip(0, 0, 0, 0);
    if (!t->worlds.n) mc_text_centered(cx, top + U(20), size, COL_GREY, "No worlds yet. Create one to begin.");
    if (t->worlds.n > rows) {
        float track = bottom - top - U(8), bar = MAX(U(8), track * (float)rows / (float)t->worlds.n);
        float by = top + U(4) + (track - bar) * (max_scroll ? (float)t->scroll / (float)max_scroll : 0.0f);
        ui_rect(x + U(LIST_W) + U(6), by, U(3), bar, rgba(180, 180, 180, 255));
    }
    screen_title(cx, U(14), "Select World");

    float row1 = (float)height - U(52), row2 = (float)height - U(28);
    float bx = cx - U(152);
    bool has = selected_name(t) != NULL;
    if (button(bx, row1, U(150), "Play Selected World", has) || (has && (key_pressed(GLFW_KEY_ENTER) || key_pressed(GLFW_KEY_KP_ENTER)))) play = true;
    if (button(bx + U(154), row1, U(150), "Create New World", true)) {
        go(t, TS_CREATE);
        t->field = 0;
        t->name[0] = t->seed[0] = 0;
    }
    if (button(bx, row2, U(98), "Edit", has)) {
        go(t, TS_EDIT);
        snprintf(t->name, sizeof t->name, "%s", selected_name(t));
    }
    if (button(bx + U(103), row2, U(98), "Delete", has)) go(t, TS_DELETE);
    if (button(bx + U(206), row2, U(98), "Cancel", true) || key_pressed(GLFW_KEY_ESCAPE)) go(t, TS_MAIN);

    if (play && has) {
        snprintf(world_out, cap, "%s", selected_name(t));
        return true;
    }
    return false;
}

/* Returns true when the player asked for a new world; the choice is written to world_out and seed_out. */
static bool title_create(TitleState *t, int width, int height, char *world_out, size_t cap, u64 *seed_out, bool *seed_set) {
    float cx = (float)width * 0.5f, bx = cx - U(WIDE_W) * 0.5f;
    dirt_background(width, height, 64);
    screen_title(cx, U(20), "Create New World");
    float y = U(60);
    if (clicked(bx, y, U(WIDE_W), U(BTN_H), GLFW_MOUSE_BUTTON_LEFT)) t->field = 0;
    y += U(BTN_H) + U(26);
    if (clicked(bx, y, U(WIDE_W), U(BTN_H), GLFW_MOUSE_BUTTON_LEFT)) t->field = 1;
    y += U(BTN_H) + U(16);
    if (key_pressed(GLFW_KEY_TAB)) t->field ^= 1;
    float field_y = U(60);
    text_field(cx, field_y, 10, "World Name", t->name, t->field == 0, true);
    field_y += U(BTN_H) + U(26);
    text_field(cx, field_y, 11, "Seed (empty for random, any text works)", t->seed, t->field == 1, false);
    bool create = button(bx, y, U(WIDE_W), "Create New World", true) || key_pressed(GLFW_KEY_ENTER) || key_pressed(GLFW_KEY_KP_ENTER);
    y += U(BTN_PITCH);
    if (button(bx, y, U(WIDE_W), "Cancel", true) || key_pressed(GLFW_KEY_ESCAPE)) go(t, TS_WORLDS);
    if (t->message[0]) mc_text_centered(cx, y + U(BTN_PITCH), text_size() * 0.9f, COL_RED, t->message);
    (void)height;
    if (!create) return false;
    if (!t->name[0]) { snprintf(t->message, sizeof t->message, "Enter a name for the world."); return false; }
    char path[300];
    snprintf(path, sizeof path, "saves/%s", t->name);
    if (path_exists(path)) { snprintf(t->message, sizeof t->message, "A world named '%s' already exists.", t->name); return false; }
    snprintf(world_out, cap, "%s", t->name);
    *seed_set = true;
    *seed_out = t->seed[0] ? seed_from_text(t->seed) : (u64)(time_now_s() * 1e6) ^ 0x9E3779B97F4A7C15ull;
    return true;
}

/* Renames the selected world's folder. */
static void title_edit(TitleState *t, int width, int height) {
    float cx = (float)width * 0.5f, bx = cx - U(WIDE_W) * 0.5f;
    const char *old = selected_name(t);
    if (!old) { go(t, TS_WORLDS); return; }
    dirt_background(width, height, 64);
    screen_title(cx, U(20), "Edit World");
    float y = U(60);
    text_field(cx, y, 12, "World Name", t->name, true, true);
    y += U(BTN_H) + U(8);
    char info[96];
    snprintf(info, sizeof info, "Seed: %s   Last played: %s", t->info.d[t->selected].seed, t->info.d[t->selected].date);
    mc_text_centered(cx, y, text_size() * 0.85f, COL_GREY, info);
    y += U(24);
    bool save = button(bx, y, U(WIDE_W), "Save Changes", t->name[0] != 0) || key_pressed(GLFW_KEY_ENTER) || key_pressed(GLFW_KEY_KP_ENTER);
    y += U(BTN_PITCH);
    if (button(bx, y, U(WIDE_W), "Delete World", true)) go(t, TS_DELETE);
    y += U(BTN_PITCH) + U(8);
    if (button(bx, y, U(WIDE_W), "Cancel", true) || key_pressed(GLFW_KEY_ESCAPE)) go(t, TS_WORLDS);
    if (t->message[0]) mc_text_centered(cx, y + U(BTN_PITCH), text_size() * 0.9f, COL_RED, t->message);
    if (!save || !t->name[0]) return;
    if (!strcmp(t->name, old)) { go(t, TS_WORLDS); return; }
    char from[300], to[300];
    snprintf(from, sizeof from, "saves/%s", old);
    snprintf(to, sizeof to, "saves/%s", t->name);
    if (path_exists(to)) { snprintf(t->message, sizeof t->message, "A world named '%s' already exists.", t->name); return; }
    if (!path_rename(from, to)) { snprintf(t->message, sizeof t->message, "Could not rename the world folder."); return; }
    char renamed[FIELD_CAP];
    snprintf(renamed, sizeof renamed, "%s", t->name);
    refresh_worlds(t, renamed);
    go(t, TS_WORLDS);
}

static void title_delete(TitleState *t, int width, int height) {
    float cx = (float)width * 0.5f, bx = cx - U(WIDE_W) * 0.5f;
    const char *name = selected_name(t);
    if (!name) { go(t, TS_WORLDS); return; }
    dirt_background(width, height, 64);
    float size = text_size();
    screen_title(cx, U(70), "Are you sure you want to delete this world?");
    char line[160];
    snprintf(line, sizeof line, "'%s' will be lost forever. This cannot be undone.", name);
    mc_text_centered(cx, U(90), size, COL_RED, line);
    float y = U(130);
    bool confirm = uiw_button_action(13, (UIWRect){bx, y, U(WIDE_W), U(BTN_H)},
                                     "Delete", true, text_size(), COL_RED) == 1;
    y += U(BTN_PITCH);
    if (button(bx, y, U(WIDE_W), "Cancel", true) || key_pressed(GLFW_KEY_ESCAPE)) { go(t, TS_WORLDS); return; }
    (void)height;
    if (!confirm) return;
    /* Names come from the folder listing, but a path separator or dot prefix must never reach the delete. */
    if (name[0] == '.' || strchr(name, '/') || strchr(name, '\\')) { go(t, TS_WORLDS); return; }
    char path[300];
    snprintf(path, sizeof path, "saves/%s", name);
    bool ok = dir_remove_all(path);
    refresh_worlds(t, NULL);
    go(t, TS_WORLDS);
    if (!ok) snprintf(t->message, sizeof t->message, "Some files of that world could not be removed.");
}

static void title_main(TitleState *t, int width, int height) {
    float cx = (float)width * 0.5f, bx = cx - U(WIDE_W) * 0.5f;
    ensure_tiles();
    /* Sky above, then a grass edge and dirt below, as if standing at the surface. */
    float ground = floorf((float)height * 0.78f / U(32)) * U(32);
    ui_rect_gradient(0, 0, (float)width, ground, rgba(98, 140, 224, 255), rgba(190, 214, 255, 255));
    tile_fill(g_grass_tex, 0, ground, (float)width, U(32), rgba(255, 255, 255, 255));
    tile_fill(g_dirt_tex, 0, ground + U(32), (float)width, (float)height - ground - U(32), rgba(150, 150, 150, 255));
    ui_rect_gradient(0, ground + U(32), (float)width, U(6), rgba(0, 0, 0, 90), rgba(0, 0, 0, 0));
    draw_logo(cx, U(36), t->splash);

    float y = (float)height * 0.25f + U(48);
    if (button(bx, y, U(WIDE_W), "Singleplayer", true)) go(t, TS_WORLDS);
    y += U(BTN_PITCH) + U(12);
    if (button(bx, y, U(HALF_W), "Options...", true)) go(t, TS_OPTIONS);
    if (button(bx + U(102), y, U(HALF_W), "Quit Game", true)) window_request_close();
    if (t->message[0]) mc_text_centered(cx, y + U(BTN_PITCH) + U(6), text_size() * 0.9f, COL_RED, t->message);
}

bool menu_title(char *world_out, size_t cap, u64 *seed_out, bool *seed_set) {
    g_return_to_title_requested = false;
    TitleState t = {0};
    t.last_click = -1;
    t.splash = SPLASHES[(u64)(time_now_s() * 1000.0) % ARRAY_LEN(SPLASHES)];
    refresh_worlds(&t, NULL);
    window_set_cursor_captured(false);
    g_settings_live = false;
    bool chosen = false;
    while (!chosen) {
        window_poll();
        if (g_win.should_close) break;
        gui_scale(g_win.width, g_win.height);
        glViewport(0, 0, g_win.fb_width, g_win.fb_height);
        glClearColor(0.07f, 0.09f, 0.13f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        ui_begin(g_win.width, g_win.height);
        switch (t.screen) {
        case TS_MAIN: title_main(&t, g_win.width, g_win.height); break;
        case TS_WORLDS: chosen = title_worlds(&t, g_win.width, g_win.height, world_out, cap); break;
        case TS_CREATE: chosen = title_create(&t, g_win.width, g_win.height, world_out, cap, seed_out, seed_set); break;
        case TS_EDIT: title_edit(&t, g_win.width, g_win.height); break;
        case TS_DELETE: title_delete(&t, g_win.width, g_win.height); break;
        case TS_OPTIONS:
            dirt_background(g_win.width, g_win.height, 64);
            bool done = settings_rows((float)g_win.width * 0.5f, g_win.height);
            if (!done && key_pressed(GLFW_KEY_ESCAPE) && !settings_step_back()) done = true;
            if (done) { leave_settings(); go(&t, TS_MAIN); }
            break;
        }
        ui_end();
        window_swap();
        sleep_ms(SCREEN_FRAME_MS);
    }
    strlist_free(&t.worlds);
    vec_free(t.info);
    return chosen;
}
