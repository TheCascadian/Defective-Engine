/* Screen and widget system. Screens are plain structs in a fixed table, so drawing and input never allocate. */
#include "screen.h"
#include "ui.h"

#include <GLFW/glfw3.h>

#define PAD 8.0f
#define ROW_H 18.0f
#define TEXT_PX 14.0f

typedef struct Screen {
    ScreenDef def;
    Widget *widgets;
    int count, focus;
    bool used;
} Screen;

static Screen S[SCREEN_MAX];
static int g_stack[SCREEN_STACK], g_depth;

static Screen *find(const char *id) {
    for (int i = 0; i < SCREEN_MAX; i++) if (S[i].used && !strcmp(S[i].def.id, id)) return &S[i];
    return NULL;
}

bool screen_register(const ScreenDef *def) {
    if (!def->id[0] || find(def->id)) return false;
    for (int i = 0; i < SCREEN_MAX; i++) if (!S[i].used) {
        S[i] = (Screen){.def = *def, .used = true};
        S[i].widgets = xcalloc(SCREEN_MAX_WIDGETS, sizeof(Widget));
        return true;
    }
    return false;
}

bool screen_add_widget(const char *screen, const Widget *w) {
    Screen *s = find(screen);
    if (!s || !w->id[0] || s->count >= SCREEN_MAX_WIDGETS || screen_widget(screen, w->id)) return false;
    s->widgets[s->count] = *w;
    s->widgets[s->count].selected = w->selected ? w->selected : -1;
    if (w->type == WIDGET_BUTTON || w->type == WIDGET_LIST) s->widgets[s->count].focusable = true;
    s->count++;
    return true;
}

Widget *screen_widget(const char *screen, const char *widget) {
    Screen *s = find(screen);
    for (int i = 0; s && i < s->count; i++) if (!strcmp(s->widgets[i].id, widget)) return &s->widgets[i];
    return NULL;
}

static int stack_index(const Screen *s) { return (int)(s - S); }

bool screen_open(const char *id) {
    Screen *s = find(id);
    if (!s || g_depth >= SCREEN_STACK) return false;
    for (int i = 0; i < g_depth; i++) if (g_stack[i] == stack_index(s)) return false;
    g_stack[g_depth++] = stack_index(s);
    s->focus = -1;
    for (int i = 0; i < s->count && s->focus < 0; i++) if (s->widgets[i].focusable && s->widgets[i].enabled) s->focus = i;
    if (g_win.handle) window_set_cursor_captured(false);
    if (s->def.on_open) s->def.on_open(s->def.id, "", 0, s->def.user);
    return true;
}

bool screen_close(void) {
    if (!g_depth) return false;
    Screen *s = &S[g_stack[--g_depth]];
    if (s->def.on_close) s->def.on_close(s->def.id, "", 0, s->def.user);
    if (!g_depth && g_win.handle && !hud_inventory_open() && !menu_is_open()) window_set_cursor_captured(true);
    return true;
}

void screen_close_all(void) { while (screen_close()) {} }

bool screen_is_open(const char *id) {
    if (!id) return g_depth > 0;
    for (int i = 0; i < g_depth; i++) if (!strcmp(S[g_stack[i]].def.id, id)) return true;
    return false;
}

const char *screen_top(void) { return g_depth ? S[g_stack[g_depth - 1]].def.id : ""; }

static void release(Screen *s) {
    if (s->def.release) s->def.release(s->def.user);
    for (int i = 0; i < s->count; i++) if (s->widgets[i].user && s->widgets[i].release) s->widgets[i].release(s->widgets[i].user);
    free(s->widgets);
    *s = (Screen){0};
}

bool screen_unregister(const char *id) {
    Screen *s = find(id);
    if (!s) return false;
    int idx = stack_index(s), out = 0;
    for (int i = 0; i < g_depth; i++) if (g_stack[i] != idx) g_stack[out++] = g_stack[i];
    if (out != g_depth) { g_depth = out; }
    release(s);
    return true;
}

void screen_remove_scripted(void) {
    for (int i = 0; i < SCREEN_MAX; i++) if (S[i].used && S[i].def.scripted) screen_unregister(S[i].def.id);
}

void screen_shutdown(void) {
    g_depth = 0;
    for (int i = 0; i < SCREEN_MAX; i++) if (S[i].used) release(&S[i]);
}

typedef struct Frame { float x, y, w, h; } Frame;
typedef struct Metrics { float k, tscale; } Metrics; /* layout multiplier and text multiplier */

static Metrics metrics(int width, int height) { return (Metrics){ui_gui_scale(width, height), ui_ui_text_scale()}; }
static float text_px(const Metrics *m, float base) { return ui_snap_text(base * m->k * m->tscale); }

static float row_h(const Metrics *m) { return ceilf(MAX(ROW_H * m->k, text_px(m, TEXT_PX) + 4.0f * m->k)); }

static Frame widget_frame(const Frame *f, const Widget *w, const Metrics *m) {
    float tw = ui_snap(w->w * m->k), th = ui_snap(w->h * m->k);
    if (w->type == WIDGET_LABEL || w->type == WIDGET_BUTTON) ui_fit(tw, th, w->text, text_px(m, TEXT_PX), UI_FIT_PAD * m->k, &tw, &th);
    return (Frame){ui_snap(f->x + w->x * m->k), ui_snap(f->y + w->y * m->k), tw, th};
}

/* The screen grows to hold its widgets once text scaling pushes them past the authored size. */
static Frame frame_of(const Screen *s, int width, int height, const Metrics *m) {
    float w = (s->def.w > 0 ? s->def.w : 240) * m->k, h = (s->def.h > 0 ? s->def.h : 160) * m->k;
    Frame origin = {0, 0, 0, 0};
    for (int i = 0; i < s->count; i++) {
        Frame r = widget_frame(&origin, &s->widgets[i], m);
        w = MAX(w, r.x + r.w + PAD * m->k);
        h = MAX(h, r.y + r.h + PAD * m->k);
    }
    w = ceilf(w); h = ceilf(h);
    return (Frame){ui_snap(((float)width - w) * 0.5f), ui_snap(((float)height - h) * 0.5f), w, h};
}

static bool inside(const Frame *r, float x, float y) { return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h; }

static void activate(Screen *s, Widget *w, int index) {
    if (!w->enabled) return;
    if (w->type == WIDGET_LIST) w->selected = index;
    if (w->on_click) w->on_click(s->def.id, w->id, index, w->user);
}

bool screen_click_at(int width, int height, float mx, float my, bool right) {
    if (!g_depth) return false;
    Screen *s = &S[g_stack[g_depth - 1]];
    Metrics m = metrics(width, height);
    Frame f = frame_of(s, width, height, &m);
    if (!inside(&f, mx, my)) return s->def.modal;
    for (int i = s->count - 1; i >= 0; i--) {
        Widget *w = &s->widgets[i];
        Frame r = widget_frame(&f, w, &m);
        if (!inside(&r, mx, my) || w->type == WIDGET_PANEL || w->type == WIDGET_LABEL) continue;
        if (w->type == WIDGET_BUTTON || w->type == WIDGET_SLOT || w->type == WIDGET_BAR) { if (!right) { s->focus = i; activate(s, w, 0); } return true; }
        int row = w->scroll + (int)((my - r.y) / row_h(&m));
        if (row >= 0 && row < w->item_count) { s->focus = i; activate(s, w, row); }
        return true;
    }
    return true;
}

bool screen_key(int key) {
    if (!g_depth) return false;
    Screen *s = &S[g_stack[g_depth - 1]];
    if (key == GLFW_KEY_ESCAPE) { if (s->def.close_on_escape || s->def.modal) screen_close(); return true; }
    if (key == GLFW_KEY_TAB || key == -GLFW_KEY_TAB) { /* a negative key means Shift-Tab */
        int dir = key < 0 ? -1 : 1;
        for (int n = 1; n <= s->count; n++) {
            int i = ((s->focus < 0 ? (dir > 0 ? -1 : 0) : s->focus) + dir * n % s->count + s->count) % s->count;
            if (s->widgets[i].focusable && s->widgets[i].enabled) { s->focus = i; break; }
        }
        return true;
    }
    Widget *w = s->focus >= 0 ? &s->widgets[s->focus] : NULL;
    if (!w) return s->def.modal;
    if (w->type == WIDGET_LIST && w->item_count) {
        if (key == GLFW_KEY_DOWN) { w->selected = MIN(w->selected + 1, w->item_count - 1); return true; }
        if (key == GLFW_KEY_UP) { w->selected = MAX(w->selected - 1, 0); return true; }
    }
    if (key == GLFW_KEY_ENTER || key == GLFW_KEY_SPACE) { activate(s, w, MAX(w->selected, 0)); return true; }
    return s->def.modal;
}

const char *screen_focus(void) {
    if (!g_depth) return "";
    const Screen *s = &S[g_stack[g_depth - 1]];
    return s->focus >= 0 ? s->widgets[s->focus].id : "";
}

void screen_update(int width, int height) {
    if (!g_depth) return;
    for (int b = 0; b < 2; b++) if (g_in.mouse_pressed[b]) screen_click_at(width, height, (float)g_in.mouse_x, (float)g_in.mouse_y, b == 1);
    static const int keys[] = {GLFW_KEY_TAB, GLFW_KEY_ENTER, GLFW_KEY_SPACE, GLFW_KEY_UP, GLFW_KEY_DOWN, GLFW_KEY_ESCAPE};
    for (int i = 0; i < ARRAY_LEN(keys) && g_depth; i++) if (key_pressed(keys[i])) screen_key(keys[i] == GLFW_KEY_TAB && key_down(GLFW_KEY_LEFT_SHIFT) ? -GLFW_KEY_TAB : keys[i]);
}

static void draw_widget(const Screen *s, int idx, const Frame *f, const Metrics *m) {
    const Widget *w = &s->widgets[idx];
    Frame r = widget_frame(f, w, m);
    u32 text = ui_theme_color("text", rgba(255, 255, 255, 255)), back = ui_theme_color("bar_back", rgba(10, 10, 12, 180));
    float ts = text_px(m, TEXT_PX), rh = row_h(m);
    bool hot = g_in.mouse_x >= r.x && g_in.mouse_x < r.x + r.w && g_in.mouse_y >= r.y && g_in.mouse_y < r.y + r.h;
    switch (w->type) {
    case WIDGET_PANEL: ui_rect(r.x, r.y, r.w, r.h, back); break;
    case WIDGET_LABEL: ui_text(r.x, r.y, ts, text, w->text); break;
    case WIDGET_BUTTON:
        ui_rect(r.x, r.y, r.w, r.h, !w->enabled ? rgba(40, 40, 44, 200) : hot ? rgba(90, 96, 120, 230) : rgba(60, 64, 82, 220));
        ui_text(r.x + (r.w - ui_text_width(ts, w->text)) * 0.5f, r.y + (r.h - ts) * 0.5f, ts, w->enabled ? text : rgba(140, 140, 140, 255), w->text);
        break;
    case WIDGET_SLOT: ui_rect(r.x, r.y, r.w, r.h, hot ? rgba(120, 124, 140, 220) : rgba(70, 72, 84, 200)); break;
    case WIDGET_BAR: {
        ui_rect(r.x, r.y, r.w, r.h, back);
        float ratio = w->max > 0 ? CLAMP(w->value / w->max, 0.0f, 1.0f) : 0.0f;
        ui_rect(r.x, r.y, r.w * ratio, r.h, ui_theme_color(w->text[0] ? w->text : "xp", rgba(200, 200, 200, 255)));
        break;
    }
    case WIDGET_LIST: case WIDGET_SCROLL: {
        ui_rect(r.x, r.y, r.w, r.h, back);
        int rows = (int)(r.h / rh);
        for (int i = 0; i < rows && w->scroll + i < w->item_count; i++) {
            int it = w->scroll + i;
            if (w->type == WIDGET_LIST && it == w->selected) ui_rect(r.x, r.y + (float)i * rh, r.w, rh, rgba(90, 96, 120, 200));
            ui_text(r.x + 4 * m->k, r.y + (float)i * rh + 2 * m->k, ts, text, w->items[it]);
        }
        break;
    }
    }
    if (idx == s->focus) { /* keyboard focus outline */
        u32 c = rgba(255, 220, 90, 255);
        ui_rect(r.x - 1, r.y - 1, r.w + 2, 1, c); ui_rect(r.x - 1, r.y + r.h, r.w + 2, 1, c);
        ui_rect(r.x - 1, r.y, 1, r.h, c); ui_rect(r.x + r.w, r.y, 1, r.h, c);
    }
}

void screen_draw(int width, int height) {
    for (int d = 0; d < g_depth; d++) {
        const Screen *s = &S[g_stack[d]];
        Metrics m = metrics(width, height);
        Frame f = frame_of(s, width, height, &m);
        if (s->def.modal) ui_rect(0, 0, (float)width, (float)height, rgba(0, 0, 0, 120));
        ui_rect(f.x, f.y, f.w, f.h, rgba(28, 30, 38, 235));
        if (s->def.title[0]) ui_text(f.x + PAD * m.k, f.y + PAD * 0.5f * m.k, text_px(&m, 16.0f), ui_theme_color("text", rgba(255, 255, 255, 255)), s->def.title);
        for (int i = 0; i < s->count; i++) draw_widget(s, i, &f, &m);
    }
}
