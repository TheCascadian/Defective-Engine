/* Menus: the title screen with the world list, the pause menu and the settings screen.
 *
 * Everything is immediate mode on top of the 2D layer: each frame a widget is drawn and tells the caller whether it
 * was clicked, so there is no widget tree to keep in sync with the data. Settings are applied as they change so the
 * player sees the effect behind the menu, and are written to disk when the screen is left.
 *
 * The controls themselves (buttons, cycle rows, sliders, fields, scroll panes, tooltips) live in ui_widgets.c; this
 * file only says what each setting means. Layout comes from UiBox stacks instead of hand-added y coordinates -
 * think of it as flexbox: describe the container once, then take cells and let the cursor advance itself. */
#include "dfe.h"
#include "ui_widgets.h"

#include <GLFW/glfw3.h>

#define PANEL_W 420.0f
#define ROW_H 36.0f
#define TEXT_SIZE 18.0f
#define TITLE_SIZE 34.0f
#define LIST_ROWS 6
#define FIELD_CAP 40
#define WORLD_NAME_CAP 40
#define SCREEN_FRAME_MS 16

/* Render distance choices; 0 follows the preset. */
static const int RD_CHOICES[] = {0, 4, 6, 8, 10, 12, 16, 20, 24, 32};
static const int DYN_CHOICES[] = {-1, 1, 0}; /* automatic, on, off */
#define FOV_MIN 50
#define FOV_MAX 110
#define FOV_STEP 5
#define SCALE_MIN 0.5f
#define SCALE_MAX 1.0f
#define SCALE_STEP 0.05f

/* Stable widget IDs: hashing the dotted setting path means scroll/focus memory survives hot reload, and two
 * screens that show the same setting share its state instead of fighting over it. */
#define ID_PRESET ui_id("settings.preset")
#define ID_RDIST ui_id("settings.render_distance")
#define ID_DYNRES ui_id("settings.dynamic_resolution")
#define ID_SCALE ui_id("settings.render_scale")
#define ID_FOV ui_id("settings.fov")
#define ID_VSYNC ui_id("settings.vsync")
#define ID_DONE ui_id("settings.done")
#define ID_HINT ui_id("settings.scale_hint")
#define ID_SCROLL ui_id("settings.scroll")

typedef enum MenuScreen { SCREEN_NONE, SCREEN_PAUSE, SCREEN_SETTINGS } MenuScreen;

static MenuScreen g_screen = SCREEN_NONE;
static bool g_quit_requested;
static bool g_settings_live; /* true when the world exists, so gfx_apply may push values to the scene */

static inline int wrap(int i, int count) { return ((i % count) + count) % count; }
static int index_of_int(const int *list, int count, int value) {
    for (int i = 0; i < count; i++) if (list[i] == value) return i;
    return 0;
}

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

/* Draws the settings panel at the given rect and returns true when Done was pressed. `panel` is a cell taken from
 * the caller's column, so the same function serves the centered pause overlay and the free-floating title screen.
 * The row stack sits inside a scroll pane: on a low window where seven rows do not fit, the wheel still reaches
 * everything and the scrollbar shows where you are - overflow_h is the natural height of the content. */
static bool settings_rows(UiBox panel, float panel_h) {
    char v[64];
    bool changed = false;
    const Preset *p = preset_find(g_settings.preset);
    UiBox col = ui_box(panel.x, panel.y, panel.w, panel_h, UI_STACK_V, g_ui_theme.gap, 0.0f);
    const float overflow_h = 7.0f * ROW_H + 2.0f * (ROW_H + 10.0f) + 6.0f * g_ui_theme.gap + 24.0f;
    UiScroll sc = ui_scroll_begin(ID_SCROLL, col, MAX(overflow_h, MIN(col.h, panel_h)));
    ui_scroll_update(&sc, ID_SCROLL);

    UiStep s;
    UiCell(preset, &sc.content, ROW_H);
    if ((s = widget_cycle_row(&preset, ID_PRESET, "Preset", p ? p->name : g_settings.preset))) { step_preset(s == UI_STEP_FWD ? 1 : -1); changed = true; }
    if (g_settings.render_distance > 0) snprintf(v, sizeof v, "%d chunks", g_settings.render_distance);
    else snprintf(v, sizeof v, "preset (%d)", p ? p->render_distance : 0);
    UiCell(rdist, &sc.content, ROW_H);
    if ((s = widget_cycle_row(&rdist, ID_RDIST, "Render distance", v))) { step_render_distance(s == UI_STEP_FWD ? 1 : -1); changed = true; }
    snprintf(v, sizeof v, "%s", g_settings.dynamic_resolution < 0 ? "preset" : g_settings.dynamic_resolution ? "on" : "off");
    UiCell(dynres, &sc.content, ROW_H);
    if ((s = widget_cycle_row(&dynres, ID_DYNRES, "Dynamic resolution", v))) { step_dynamic(s == UI_STEP_FWD ? 1 : -1); changed = true; }
    /* The two continuous settings use the generic slider instead of cycling through fixed steps: dragging to a
     * value is one motion where cycling took up to ten clicks, which matters on a slow laptop at 30 fps. */
    UiCell(scale, &sc.content, ROW_H + 10.0f);
    if (widget_slider(&scale, ID_SCALE, "Render scale", &g_settings.render_scale, SCALE_MIN, SCALE_MAX, SCALE_STEP)) changed = true;
    UiCell(fov, &sc.content, ROW_H + 10.0f);
    if (widget_slider(&fov, ID_FOV, "Field of view", &g_settings.fov_deg, FOV_MIN, FOV_MAX, FOV_STEP)) changed = true;
    UiCell(vsync, &sc.content, ROW_H);
    if ((s = widget_cycle_row(&vsync, ID_VSYNC, "Vertical sync", g_settings.vsync ? "on" : "off"))) { g_settings.vsync = !g_settings.vsync; changed = true; }
    UiBox hint = layout_next(&sc.content, 20.0f);
    ui_text(hint.x, hint.y, 14, g_ui_theme.col_dim, "Render scale applies while dynamic resolution is off.");
    ui_scroll_end(&sc);

    UiCell(done_cell, &col, ROW_H);
    bool done = widget_button(&done_cell, ID_DONE, "Done");
    ui_tooltip_anchor(ID_HINT, &hint, "Only used when Dynamic resolution is off.");
    if (changed) settings_changed();
    return done;
}

static void leave_settings(void) {
    /* Benchmarks never reach a menu, but the guard keeps a script from overwriting a player's file by accident. */
    if (!g_opt.benchmark) settings_save();
}

/* ------------------------------------------------------------- pause menu */

bool menu_is_open(void) { return g_screen != SCREEN_NONE; }

bool menu_quit_requested(void) { return g_quit_requested; }

void menu_set_open(bool open) {
    if (open == menu_is_open()) return;
    g_screen = open ? SCREEN_PAUSE : SCREEN_NONE;
    g_settings_live = true;
    ui_clear_focus(); /* stale hover focus must not fire a widget on the next screen */
    window_set_cursor_captured(!open);
}

/* Escape steps back one level: settings to pause, pause to the game. */
void menu_back(void) {
    if (g_screen == SCREEN_SETTINGS) { leave_settings(); g_screen = SCREEN_PAUSE; ui_clear_focus(); }
    else menu_set_open(false);
}

void menu_draw(int width, int height) {
    if (!menu_is_open()) return;
    ui_rect(0, 0, (float)width, (float)height, rgba(0, 0, 0, 120));
    UiBox outer = ui_box(0, 0, (float)width, (float)height, UI_STACK_V, 0.0f, 0.0f);
    UiBox panel = ui_center_in(&outer, PANEL_W, (float)height * 0.8f); /* margin:auto: the block sits dead center */
    UiBox col = ui_box(panel.x, panel.y, panel.w, panel.h, UI_STACK_V, g_ui_theme.gap, 0.0f);
    if (g_screen == SCREEN_PAUSE) {
        UiBox head = layout_next_extent(&col, 56.0f, 56.0f + 60.0f); /* the title reserves 60px of space above it */
        ui_center_text(head.x, head.y, head.w, 40.0f, TITLE_SIZE, g_ui_theme.col_text, "Paused");
        UiCell(btn0, &col, ROW_H);
        if (widget_button(&btn0, ui_id("pause.resume"), "Resume")) menu_set_open(false);
        UiCell(btn1, &col, ROW_H);
        if (widget_button(&btn1, ui_id("pause.settings"), "Settings")) { g_screen = SCREEN_SETTINGS; ui_clear_focus(); }
        UiCell(btn2, &col, ROW_H);
        if (widget_button(&btn2, ui_id("pause.quit"), "Save and quit")) { g_quit_requested = true; g_win.should_close = true; }
    } else {
        UiBox head = layout_next_extent(&col, 56.0f, 56.0f + 60.0f);
        ui_center_text(head.x, head.y, head.w, 40.0f, TITLE_SIZE, g_ui_theme.col_text, "Settings");
        if (settings_rows(col, col.h)) { leave_settings(); g_screen = SCREEN_PAUSE; ui_clear_focus(); }
    }
    ui_tooltips_tick(); /* dwell timer, fed by the dt measured in ui_frame_begin() */
    ui_tooltips_draw();
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

/* Returns true when a world was chosen; the choice is written to world_out and seed_out. The fields are widgets,
 * but which one is active stays app logic (Tab switches name/seed), so `active` is passed in and the returned
 * cell is tested with ui_clicked() to take selection from the mouse. */
static bool title_create_form(TitleState *t, UiBox col, char *world_out, size_t cap, u64 *seed_out, bool *seed_set) {
    UiBox f0 = layout_next(&col, ROW_H + 20.0f);
    widget_text_field(&f0, "World name", t->name, FIELD_CAP, t->field == 0);
    if (ui_clicked(f0.x, f0.y, f0.w, f0.h, GLFW_MOUSE_BUTTON_LEFT)) t->field = 0;
    UiBox f1 = layout_next(&col, ROW_H + 20.0f);
    widget_text_field(&f1, "Seed (empty for random, any text is accepted)", t->seed, FIELD_CAP, t->field == 1);
    if (ui_clicked(f1.x, f1.y, f1.w, f1.h, GLFW_MOUSE_BUTTON_LEFT)) t->field = 1;
    if (key_pressed(GLFW_KEY_TAB)) t->field ^= 1;
    edit_field(t->field == 0 ? t->name : t->seed, FIELD_CAP, t->field == 0);
    UiCell(btn3, &col, ROW_H);
    bool create = widget_button(&btn3, ui_id("title.create"), "Create world") ||
                  key_pressed(GLFW_KEY_ENTER) || key_pressed(GLFW_KEY_KP_ENTER);
    UiCell(btn4, &col, ROW_H);
    if (widget_button(&btn4, ui_id("title.back"), "Back") || key_pressed(GLFW_KEY_ESCAPE)) {
        t->creating = false; t->message[0] = 0;
    }
    if (t->message[0]) {
        UiBox msg = layout_next(&col, 20.0f);
        ui_text(msg.x, msg.y, 14, g_ui_theme.col_warn, t->message);
    }
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

/* The world list is a scroll pane whose children are addressed like grid cells: one column, LIST_ROWS visible,
 * and the integer scroll offset simply shifts the starting index. That keeps the old clamped int semantics
 * (selection code outside this function still thinks in rows) while the wheel, thumb and clipping come free -
 * the pane's own state slot holds the pixel offset, so the bar and the drag work without extra app code. */
static bool title_world_list(TitleState *t, UiBox col, char *world_out, size_t cap) {
    UiBox list_area = layout_next(&col, (float)LIST_ROWS * (ROW_H + g_ui_theme.gap));
    int max_top = MAX(t->worlds.n - LIST_ROWS, 0);
    float row_pitch = ROW_H + g_ui_theme.gap;
    UiScroll sc = ui_scroll_begin(ui_id("title.worlds"), list_area, (float)t->worlds.n * row_pitch);
    ui_scroll_update(&sc, ui_id("title.worlds")); /* wheel + thumb write the shared state slot */
    t->scroll = CLAMP((int)(sc.scroll / row_pitch + 0.5f), 0, max_top); /* back to whole rows for the indexing below */
    sc.content.y = list_area.y - (float)t->scroll * row_pitch;          /* keep the drawn text on exact row lines */
    for (int i = 0; i < LIST_ROWS; i++) {
        int idx = t->scroll + i;
        if (idx >= t->worlds.n) break;
        UiBox cell = layout_grid_cell(&sc.content, 1, LIST_ROWS, g_ui_theme.gap, i);
        if (widget_button(&cell, ui_id_of(t->worlds.d[idx]), t->worlds.d[idx])) {
            snprintf(world_out, cap, "%s", t->worlds.d[idx]);
            ui_scroll_end(&sc);
            return true;
        }
    }
    if (!t->worlds.n) {
        UiBox empty = layout_grid_cell(&sc.content, 1, LIST_ROWS, g_ui_theme.gap, 0);
        ui_text(empty.x, empty.y + 6, TEXT_SIZE, g_ui_theme.col_dim, "No worlds yet. Create one to begin.");
    }
    ui_scroll_end(&sc);
    if (t->worlds.n > LIST_ROWS) ui_tooltip_anchor(ui_id("title.worlds.hint"), &list_area, "Scroll the wheel for more worlds.");
    UiCell(btn5, &col, ROW_H);
    if (widget_button(&btn5, ui_id("title.new"), "New world")) { t->creating = true; t->field = 0; t->name[0] = t->seed[0] = 0; }
    UiCell(btn6, &col, ROW_H);
    if (widget_button(&btn6, ui_id("title.settings"), "Settings")) t->settings = true;
    UiCell(btn7, &col, ROW_H);
    if (widget_button(&btn7, ui_id("title.quit"), "Quit")) g_win.should_close = true;
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
        ui_begin(g_win.width, g_win.height); /* also runs ui_frame_begin(): timers reset for this pass */
        UiBox screen = ui_box(0, 0, (float)g_win.width, (float)g_win.height, UI_STACK_V, 0.0f, 0.0f);
        UiBox panel = ui_center_in(&screen, PANEL_W, MIN((float)g_win.height * 0.85f, 560.0f));
        ui_rect_gradient(0, 0, (float)g_win.width, (float)g_win.height, rgba(24, 34, 58, 255), rgba(8, 10, 16, 255));
        UiBox col = ui_box(panel.x, panel.y, panel.w, panel.h, UI_STACK_V, g_ui_theme.gap, 0.0f);
        UiBox title = layout_next(&col, 56.0f);
        ui_center_text(title.x, title.y, title.w, 50.0f, TITLE_SIZE + 6, g_ui_theme.col_text, "Defective Engine");
        if (t.settings) {
            if (settings_rows(col, col.h)) { leave_settings(); t.settings = false; }
            if (key_pressed(GLFW_KEY_ESCAPE)) { leave_settings(); t.settings = false; }
        } else if (t.creating) chosen = title_create_form(&t, col, world_out, cap, seed_out, seed_set);
        else chosen = title_world_list(&t, col, world_out, cap);
        ui_tooltips_tick();
        ui_tooltips_draw();
        ui_end();
        window_swap();
        sleep_ms(SCREEN_FRAME_MS);
    }
    strlist_free(&t.worlds);
    return chosen;
}
