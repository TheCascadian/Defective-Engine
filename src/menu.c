/* Menus: the title screen with the world list, the pause menu and the settings screen.
 *
 * Everything is immediate mode on top of the 2D layer: each frame a widget is drawn and tells the caller whether it
 * was clicked, so there is no widget tree to keep in sync with the data. Settings are applied as they change so the
 * player sees the effect behind the menu, and are written to disk when the screen is left. */
#include "dfe.h"
#include <GLFW/glfw3.h>

#define PANEL_W 420.0f
#define ROW_H 36.0f
#define ROW_GAP 8.0f
#define TEXT_SIZE 18.0f
#define TITLE_SIZE 34.0f
#define ARROW_W 40.0f
#define LIST_ROWS 6
#define FIELD_CAP 40
#define WORLD_NAME_CAP 40
#define SCREEN_FRAME_MS 16

#define COL_PANEL rgba(14, 17, 24, 235)
#define COL_BTN rgba(40, 48, 66, 255)
#define COL_BTN_HOT rgba(66, 94, 150, 255)
#define COL_TEXT rgba(232, 235, 242, 255)
#define COL_DIM rgba(150, 158, 175, 255)
#define COL_WARN rgba(255, 170, 140, 255)
#define COL_FIELD rgba(22, 26, 36, 255)
#define COL_FIELD_ACTIVE rgba(34, 44, 66, 255)

typedef enum MenuScreen { SCREEN_NONE, SCREEN_PAUSE, SCREEN_SETTINGS } MenuScreen;

static MenuScreen g_screen = SCREEN_NONE;
static bool g_quit_requested;
static bool g_settings_live; /* true when the world exists, so gfx_apply may push values to the scene */

/* Render distance choices; 0 follows the preset. */
static const int RD_CHOICES[] = {0, 4, 6, 8, 10, 12, 16, 20, 24, 32};
static const int DYN_CHOICES[] = {-1, 1, 0}; /* automatic, on, off */
#define FOV_MIN 50
#define FOV_MAX 110
#define FOV_STEP 5
#define SCALE_MIN 0.5f
#define SCALE_MAX 1.0f
#define SCALE_STEP 0.05f

/* ---------------------------------------------------------------- widgets */

static bool hovered(float x, float y, float w, float h) {
    return g_in.mouse_x >= x && g_in.mouse_x < x + w && g_in.mouse_y >= y && g_in.mouse_y < y + h;
}

static bool clicked(float x, float y, float w, float h, int button) { return g_in.mouse_pressed[button] && hovered(x, y, w, h); }

static void draw_centered(float x, float y, float w, float h, float size, u32 color, const char *text) {
    ui_text(x + (w - ui_text_width(size, text)) * 0.5f, y + (h - size) * 0.5f - 2.0f, size, color, text);
}

static bool button(float x, float y, float w, const char *label) {
    ui_rect(x, y, w, ROW_H, hovered(x, y, w, ROW_H) ? COL_BTN_HOT : COL_BTN);
    draw_centered(x, y, w, ROW_H, TEXT_SIZE, COL_TEXT, label);
    return clicked(x, y, w, ROW_H, GLFW_MOUSE_BUTTON_LEFT);
}

/* A row with arrows on both sides. Returns -1 or +1 for the arrow that was clicked, 0 otherwise. The whole row
 * also advances on a left click in the middle, so a mouse-only player can cycle with one hand. */
static int cycle_row(float x, float y, float w, const char *label, const char *value) {
    int step = 0;
    char text[96];
    snprintf(text, sizeof text, "%s: %s", label, value);
    ui_rect(x, y, w, ROW_H, COL_BTN);
    if (hovered(x, y, ARROW_W, ROW_H)) ui_rect(x, y, ARROW_W, ROW_H, COL_BTN_HOT);
    if (hovered(x + w - ARROW_W, y, ARROW_W, ROW_H)) ui_rect(x + w - ARROW_W, y, ARROW_W, ROW_H, COL_BTN_HOT);
    draw_centered(x, y, ARROW_W, ROW_H, TEXT_SIZE, COL_TEXT, "<");
    draw_centered(x + w - ARROW_W, y, ARROW_W, ROW_H, TEXT_SIZE, COL_TEXT, ">");
    draw_centered(x + ARROW_W, y, w - 2 * ARROW_W, ROW_H, TEXT_SIZE, COL_TEXT, text);
    if (clicked(x, y, ARROW_W, ROW_H, GLFW_MOUSE_BUTTON_LEFT)) step = -1;
    else if (clicked(x + w - ARROW_W, y, ARROW_W, ROW_H, GLFW_MOUSE_BUTTON_LEFT)) step = 1;
    else if (clicked(x + ARROW_W, y, w - 2 * ARROW_W, ROW_H, GLFW_MOUSE_BUTTON_LEFT)) step = 1;
    else if (clicked(x + ARROW_W, y, w - 2 * ARROW_W, ROW_H, GLFW_MOUSE_BUTTON_RIGHT)) step = -1;
    return step;
}

static int index_of_int(const int *list, int count, int value) {
    for (int i = 0; i < count; i++) if (list[i] == value) return i;
    return 0;
}

static int wrap(int i, int count) { return ((i % count) + count) % count; }

/* ---------------------------------------------------------- settings screen */

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

static void step_scale(int step) {
    g_settings.render_scale = CLAMP(roundf((g_settings.render_scale + (float)step * SCALE_STEP) / SCALE_STEP) * SCALE_STEP, SCALE_MIN, SCALE_MAX);
}

static void step_fov(int step) { g_settings.fov_deg = (float)CLAMP((int)g_settings.fov_deg + step * FOV_STEP, FOV_MIN, FOV_MAX); }

/* Draws the settings rows at the panel's top-left corner and returns true when Done was pressed. */
static bool settings_rows(float x, float y) {
    char v[64];
    bool changed = false;
    const Preset *p = preset_find(g_settings.preset);
    int step;
    if ((step = cycle_row(x, y, PANEL_W, "Preset", p ? p->name : g_settings.preset))) { step_preset(step); changed = true; }
    y += ROW_H + ROW_GAP;
    if (g_settings.render_distance > 0) snprintf(v, sizeof v, "%d chunks", g_settings.render_distance);
    else snprintf(v, sizeof v, "preset (%d)", p ? p->render_distance : 0);
    if ((step = cycle_row(x, y, PANEL_W, "Render distance", v))) { step_render_distance(step); changed = true; }
    y += ROW_H + ROW_GAP;
    snprintf(v, sizeof v, "%s", g_settings.dynamic_resolution < 0 ? "preset" : g_settings.dynamic_resolution ? "on" : "off");
    if ((step = cycle_row(x, y, PANEL_W, "Dynamic resolution", v))) { step_dynamic(step); changed = true; }
    y += ROW_H + ROW_GAP;
    snprintf(v, sizeof v, "%d%%", (int)(g_settings.render_scale * 100.0f + 0.5f));
    if ((step = cycle_row(x, y, PANEL_W, "Render scale", v))) { step_scale(step); changed = true; }
    y += ROW_H + ROW_GAP;
    snprintf(v, sizeof v, "%d", (int)g_settings.fov_deg);
    if ((step = cycle_row(x, y, PANEL_W, "Field of view", v))) { step_fov(step); changed = true; }
    y += ROW_H + ROW_GAP;
    if ((step = cycle_row(x, y, PANEL_W, "Vertical sync", g_settings.vsync ? "on" : "off"))) { g_settings.vsync = !g_settings.vsync; changed = true; }
    y += ROW_H + ROW_GAP;
    ui_text(x, y, 14, COL_DIM, "Render scale applies while dynamic resolution is off.");
    y += 24.0f;
    if (changed) settings_changed();
    return button(x, y, PANEL_W, "Done");
}

static void leave_settings(void) {
    /* Benchmarks never reach a menu, but the guard keeps a script from overwriting a player's file by accident. */
    if (!g_opt.benchmark) settings_save();
}

#define SETTINGS_PANEL_H 420.0f

/* ------------------------------------------------------------- pause menu */

bool menu_is_open(void) { return g_screen != SCREEN_NONE; }

bool menu_quit_requested(void) { return g_quit_requested; }

void menu_set_open(bool open) {
    if (open == menu_is_open()) return;
    g_screen = open ? SCREEN_PAUSE : SCREEN_NONE;
    g_settings_live = true;
    window_set_cursor_captured(!open);
}

/* Escape steps back one level: settings to pause, pause to the game. */
void menu_back(void) {
    if (g_screen == SCREEN_SETTINGS) { leave_settings(); g_screen = SCREEN_PAUSE; }
    else menu_set_open(false);
}

void menu_draw(int width, int height) {
    if (!menu_is_open()) return;
    float cx = ((float)width - PANEL_W) * 0.5f;
    ui_rect(0, 0, (float)width, (float)height, rgba(0, 0, 0, 120));
    if (g_screen == SCREEN_PAUSE) {
        float y = (float)height * 0.5f - 130.0f;
        draw_centered(cx, y - 60, PANEL_W, 40, TITLE_SIZE, COL_TEXT, "Paused");
        if (button(cx, y, PANEL_W, "Resume")) menu_set_open(false);
        y += ROW_H + ROW_GAP;
        if (button(cx, y, PANEL_W, "Settings")) g_screen = SCREEN_SETTINGS;
        y += ROW_H + ROW_GAP;
        if (button(cx, y, PANEL_W, "Save and quit")) { g_quit_requested = true; g_win.should_close = true; }
    } else {
        float top = ((float)height - SETTINGS_PANEL_H) * 0.5f;
        draw_centered(cx, top - 56, PANEL_W, 40, TITLE_SIZE, COL_TEXT, "Settings");
        if (settings_rows(cx, top)) { leave_settings(); g_screen = SCREEN_PAUSE; }
    }
}

/* -------------------------------------------------------------- title menu */

typedef struct TitleState {
    StrList worlds;
    int scroll;
    bool creating;
    char name[FIELD_CAP], seed[FIELD_CAP];
    int field; /* 0 name, 1 seed */
    char message[160];
    bool settings;
} TitleState;

static void refresh_worlds(TitleState *t) {
    strlist_free(&t->worlds);
    StrList all = {0};
    dir_list("saves", &all);
    for (int i = 0; i < all.n; i++) {
        char path[300];
        snprintf(path, sizeof path, "saves/%s", all.d[i]);
        if (path_is_dir(path)) vec_push(t->worlds, xstrdup(all.d[i]));
    }
    strlist_free(&all);
}

/* Folder names are limited to characters that are valid on every platform the engine runs on. */
static bool name_char_ok(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'; }

static void edit_field(char *field, size_t cap, bool names_only) {
    size_t len = strlen(field);
    for (int i = 0; i < g_in.text_len; i++) {
        char c = g_in.text[i];
        if (c == ' ' && names_only) c = '_';
        if (names_only ? !name_char_ok(c) : c < 32) continue;
        if (len + 1 < cap) { field[len++] = c; field[len] = 0; }
    }
    if (key_pressed(GLFW_KEY_BACKSPACE) && len > 0) field[len - 1] = 0;
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

static void text_field(float x, float y, float w, const char *label, const char *value, bool active) {
    ui_text(x, y - 20, 14, COL_DIM, label);
    ui_rect(x, y, w, ROW_H, active ? COL_FIELD_ACTIVE : COL_FIELD);
    char shown[FIELD_CAP + 2];
    snprintf(shown, sizeof shown, "%s%s", value, active && ((int)(time_now_s() * 2.0) & 1) ? "_" : "");
    ui_text(x + 10, y + 8, TEXT_SIZE, COL_TEXT, shown);
}

/* Returns true when a world was chosen; the choice is written to world_out and seed_out. */
static bool title_create_form(TitleState *t, float cx, float y, char *world_out, size_t cap, u64 *seed_out, bool *seed_set) {
    text_field(cx, y + 20, PANEL_W, "World name", t->name, t->field == 0);
    if (clicked(cx, y + 20, PANEL_W, ROW_H, GLFW_MOUSE_BUTTON_LEFT)) t->field = 0;
    y += 20 + ROW_H + 40;
    text_field(cx, y, PANEL_W, "Seed (empty for random, any text is accepted)", t->seed, t->field == 1);
    if (clicked(cx, y, PANEL_W, ROW_H, GLFW_MOUSE_BUTTON_LEFT)) t->field = 1;
    y += ROW_H + ROW_GAP + 10;
    if (key_pressed(GLFW_KEY_TAB)) t->field ^= 1;
    edit_field(t->field == 0 ? t->name : t->seed, FIELD_CAP, t->field == 0);
    bool create = button(cx, y, PANEL_W, "Create world") || key_pressed(GLFW_KEY_ENTER) || key_pressed(GLFW_KEY_KP_ENTER);
    y += ROW_H + ROW_GAP;
    if (button(cx, y, PANEL_W, "Back") || key_pressed(GLFW_KEY_ESCAPE)) { t->creating = false; t->message[0] = 0; }
    if (t->message[0]) ui_text(cx, y + ROW_H + 12, 14, COL_WARN, t->message);
    if (!create) return false;
    if (!t->name[0]) { snprintf(t->message, sizeof t->message, "Enter a name for the world."); return false; }
    char path[300];
    snprintf(path, sizeof path, "saves/%s", t->name);
    if (path_exists(path)) { snprintf(t->message, sizeof t->message, "A world named '%s' already exists. Pick it from the list or choose another name.", t->name); return false; }
    snprintf(world_out, cap, "%s", t->name);
    *seed_set = true;
    *seed_out = t->seed[0] ? seed_from_text(t->seed) : (u64)(time_now_s() * 1e6) ^ 0x9E3779B97F4A7C15ull;
    return true;
}

static bool title_world_list(TitleState *t, float cx, float y, char *world_out, size_t cap) {
    int shown = MIN(LIST_ROWS, t->worlds.n);
    t->scroll = CLAMP(t->scroll - (int)g_in.scroll, 0, MAX(0, t->worlds.n - LIST_ROWS));
    for (int i = 0; i < shown; i++) {
        const char *name = t->worlds.d[t->scroll + i];
        if (button(cx, y, PANEL_W, name)) { snprintf(world_out, cap, "%s", name); return true; }
        y += ROW_H + ROW_GAP;
    }
    if (!t->worlds.n) { ui_text(cx, y + 6, TEXT_SIZE, COL_DIM, "No worlds yet. Create one to begin."); y += ROW_H; }
    else if (t->worlds.n > LIST_ROWS) { ui_text(cx, y, 14, COL_DIM, "Scroll the wheel for more worlds."); y += 24; }
    y = MAX(y, 0) + 8;
    if (button(cx, y, PANEL_W, "New world")) { t->creating = true; t->field = 0; t->name[0] = t->seed[0] = 0; }
    y += ROW_H + ROW_GAP;
    if (button(cx, y, PANEL_W, "Settings")) t->settings = true;
    y += ROW_H + ROW_GAP;
    if (button(cx, y, PANEL_W, "Quit")) g_win.should_close = true;
    return false;
}

bool menu_title(char *world_out, size_t cap, u64 *seed_out, bool *seed_set) {
    TitleState t = {0};
    refresh_worlds(&t);
    window_set_cursor_captured(false);
    g_settings_live = false;
    bool chosen = false;
    while (!chosen) {
        window_poll();
        if (g_win.should_close) break;
        glViewport(0, 0, g_win.fb_width, g_win.fb_height);
        glClearColor(0.07f, 0.09f, 0.13f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        ui_begin(g_win.width, g_win.height);
        float cx = ((float)g_win.width - PANEL_W) * 0.5f, top = MAX(90.0f, (float)g_win.height * 0.5f - 200.0f);
        ui_rect_gradient(0, 0, (float)g_win.width, (float)g_win.height, rgba(24, 34, 58, 255), rgba(8, 10, 16, 255));
        draw_centered(cx, top - 70, PANEL_W, 50, TITLE_SIZE + 6, COL_TEXT, "Defective Engine");
        if (t.settings) {
            if (settings_rows(cx, top)) { leave_settings(); t.settings = false; }
            if (key_pressed(GLFW_KEY_ESCAPE)) { leave_settings(); t.settings = false; }
        } else if (t.creating) chosen = title_create_form(&t, cx, top, world_out, cap, seed_out, seed_set);
        else chosen = title_world_list(&t, cx, top, world_out, cap);
        ui_end();
        window_swap();
        sleep_ms(SCREEN_FRAME_MS);
    }
    strlist_free(&t.worlds);
    return chosen;
}
