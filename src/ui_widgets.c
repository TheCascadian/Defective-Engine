/* ui_widgets.c - generic immediate-mode widgets, flex-style layout, scroll panes and tooltips.
 *
 * Performance notes (the reference machine is a 2015 dual-core with Intel HD 520):
 *  - Layout is pure arithmetic on stack structs; nothing is allocated and there is no per-frame setup pass.
 *    A screen of thirty widgets costs thirty small function calls, which -O2 inlines down to plain math.
 *  - Widget memory lives in one fixed table searched linearly over a handful of entries. A hash map would
 *    only pay off with hundreds of live widgets per frame; menus and HUDs have tens at most.
 *  - Scrollable containers reuse the existing scissor path (ui_clip), so clipping never becomes a stencil
 *    pass or an offscreen buffer. ui_clip flushes the batch once, which is why a pane costs exactly one
 *    extra draw call for its whole body rather than one per child.
 *  - Tooltips are registered during the widget pass and drawn last, outside every clip rect. */
#include "ui_widgets.h"
#include <GLFW/glfw3.h>

#define UI_STATE_MAX 64           /* widgets that can keep memory across frames; screens never come close */
#define UI_TIP_MAX 8              /* tooltip anchors registered per frame */
#define UI_HOVER_DELAY 0.35f      /* seconds the cursor must dwell before a tooltip pops */
#define UI_FLASH_S 0.12f          /* how long a clicked control keeps the pressed look */
#define UI_SCROLL_WHEEL 90.0f     /* pixels scrolled per wheel notch */
#define UI_SLIDER_KEY_STEP 0.05f  /* arrow-key nudge as a fraction of the slider range */
#define UI_TRACK_UNKNOWN 1e6f     /* max_scroll sentinel: content height was not known up front */

UiTheme g_ui_theme;

static struct {
    UiState states[UI_STATE_MAX];
    int count;
    u32 focus;                    /* widget that owns the keyboard this frame */
    u32 grab;                     /* widget holding the mouse button, wins over hover */
    float frame_dt;               /* measured here so callers need not thread it through */
    double last_time;
    struct TipSlot { u32 id; UiBox cell; const char *text; } tips[UI_TIP_MAX];
    int tip_count;
    u32 tip_id;                   /* anchor currently under the cursor */
    float tip_timer;              /* dwell time accumulated on tip_id */
} W;

void ui_theme_defaults(void) {
    g_ui_theme = (UiTheme){
        .text_size = 18.0f, .row_h = 36.0f, .gap = 8.0f, .pad = 12.0f,
        .col_bg = rgba(14, 17, 24, 235),
        .col_btn = rgba(40, 48, 66, 255),
        .col_btn_hot = rgba(66, 94, 150, 255),
        .col_btn_down = rgba(90, 128, 196, 255),
        .col_text = rgba(232, 235, 242, 255),
        .col_dim = rgba(150, 158, 175, 255),
        .col_warn = rgba(255, 170, 140, 255),
        .col_field = rgba(22, 26, 36, 255),
        .col_field_active = rgba(34, 44, 66, 255),
        .col_slider_trough = rgba(22, 26, 36, 255),
        .col_slider_fill = rgba(66, 94, 150, 255),
        .col_scrollbar = rgba(120, 130, 155, 200),
    };
}

/* ------------------------------------------------------- frame bookkeeping */

void ui_frame_begin(void) {
    if (!g_ui_theme.row_h) ui_theme_defaults(); /* lazily on the first frame: no init hook to forget */
    double now = time_now_s();
    W.frame_dt = W.last_time > 0.0 ? (float)(now - W.last_time) : 0.016f;
    W.last_time = now;
    W.focus = 0; /* re-acquired by hover/grab below; zero means "nothing has the keyboard" */
    if (!g_in.mouse_buttons[GLFW_MOUSE_BUTTON_LEFT]) W.grab = 0;
    W.tip_count = 0;
    /* Tick the click-flash timers once per frame instead of inside each widget. */
    for (int i = 0; i < W.count; i++) if (W.states[i].flash > 0.0f) W.states[i].flash -= W.frame_dt;
}

/* The dwell timer advances here, fed by the dt measured in ui_frame_begin(), so callers never have to thread a
 * delta through the UI code. Call it right before ui_tooltips_draw() at the end of a screen's draw pass. */
void ui_tooltips_tick(void) { ui_tooltips_update(W.frame_dt); }

/* ------------------------------------------------------------ input helpers */

bool ui_hover(float x, float y, float w, float h) {
    return g_in.mouse_x >= x && g_in.mouse_x < x + w && g_in.mouse_y >= y && g_in.mouse_y < y + h;
}

bool ui_clicked(float x, float y, float w, float h, int mouse_button) {
    return g_in.mouse_pressed[mouse_button] && ui_hover(x, y, w, h);
}

void ui_center_text(float x, float y, float w, float h, float size, u32 color, const char *text) {
    ui_text(x + (w - ui_text_width(size, text)) * 0.5f, y + (h - size) * 0.5f - 2.0f, size, color, text);
}

/* ---------------------------------------------------------------- box layout
 * UiBox is a div with `display:flex`: describe the area and the main axis once, then take cells with
 * layout_next(). The cursor advances itself, so calling code reads like markup instead of coordinate math.
 * Everything lives on the caller's stack; a nested container is just another UiBox copy. */

UiBox ui_box(float x, float y, float w, float h, UiDir dir, float gap, float pad) {
    UiBox b;
    b.x = x + pad; b.y = y + pad;
    b.w = MAX(w - 2.0f * pad, 0.0f);
    b.h = MAX(h - 2.0f * pad, 0.0f);
    b.dir = dir;
    b.gap = gap;
    b.first = true;
    return b;
}

UiBox layout_next_extent(UiBox *b, float size, float extent) {
    UiBox cell;
    /* `extent` lets one item pick its own main-axis size (flex-basis); 0 falls back to `size`. */
    float want = extent > 0.0f ? extent : size;
    float take = MIN(want, b->dir == UI_STACK_V ? b->h : b->w);
    if (!b->first) { /* gaps sit between children, never before the first or after the last */
        if (b->dir == UI_STACK_V) { b->y += b->gap; b->h = MAX(b->h - b->gap, 0.0f); }
        else { b->x += b->gap; b->w = MAX(b->w - b->gap, 0.0f); }
    }
    b->first = false;
    if (b->dir == UI_STACK_V) {
        cell = (UiBox){b->x, b->y, b->w, take};
        b->y += take; b->h = MAX(b->h - take, 0.0f);
    } else {
        cell = (UiBox){b->x, b->y, take, b->h};
        b->x += take; b->w = MAX(b->w - take, 0.0f);
    }
    cell.dir = b->dir; cell.gap = b->gap; cell.first = true; /* a cell is itself a usable container */
    return cell;
}

UiBox layout_next(UiBox *b, float size) { return layout_next_extent(b, size, 0.0f); }

/* Cell `index` of a `cols`-wide grid inside `b`. `rows` may be 0 (natural square-ish cells derived from the
 * column count); pass the visible row count to stretch the cells so the grid fills the box exactly. The
 * caller picks the index, so scrolling a grid is just an offset added to it - no second layout pass. */
UiBox layout_grid_cell(const UiBox *b, int cols, int rows, float gap, int index) {
    cols = MAX(cols, 1);
    float cw = (b->w - (float)MAX(cols - 1, 0) * gap) / (float)cols;
    float ch = rows > 0 ? (b->h - (float)MAX(rows - 1, 0) * gap) / (float)rows : cw;
    return (UiBox){b->x + (float)(index % cols) * (cw + gap), b->y + (float)(index / cols) * (ch + gap),
                   cw, ch, UI_STACK_V, gap, true};
}

UiBox ui_center_in(const UiBox *outer, float w, float h) {
    /* The "margin: auto" of this system: a fixed-size rect centered inside another one. */
    float cw = MIN(w, outer->w), ch = MIN(h, outer->h);
    return (UiBox){outer->x + (outer->w - cw) * 0.5f, outer->y + (outer->h - ch) * 0.5f,
                   cw, ch, outer->dir, outer->gap, true};
}

/* ----------------------------------------------------------- widget state */

UiState *ui_state_get(u32 id) {
    for (int i = 0; i < W.count; i++) if (W.states[i].id == id) return &W.states[i];
    if (W.count >= UI_STATE_MAX || !id) return NULL; /* full or invalid id: the widget still draws, it just forgets */
    UiState *s = &W.states[W.count++];
    memset(s, 0, sizeof *s);
    s->id = id;
    return s;
}

u32 ui_focus_id(void) { return W.focus; }
bool ui_focused(u32 id) { return id != 0 && W.focus == id; }
void ui_clear_focus(void) { W.focus = 0; W.grab = 0; }

/* Movement keys belong to the player unless a widget grabbed them, so WASD never leaks into a slider. */
bool ui_key_held(int key) {
    switch (key) {
    case GLFW_KEY_UP: case GLFW_KEY_DOWN: case GLFW_KEY_LEFT: case GLFW_KEY_RIGHT:
    case GLFW_KEY_ENTER: case GLFW_KEY_KP_ENTER: case GLFW_KEY_SPACE: case GLFW_KEY_TAB:
        return W.focus != 0;
    default:
        return false;
    }
}

/* Hover gives focus; a press grabs it until the button is released. Widgets call this once, early. */
static void ui_claim(u32 id, bool hot, bool press) {
    if (press && hot) W.grab = id;
    if (hot) W.focus = id;
    else if (W.grab == id) W.focus = id;
}

static void ui_press_flash(UiState *s, bool active) {
    if (active && !s->hot_last) s->flash = UI_FLASH_S; /* rising edge: keep the pressed look for a moment */
    s->hot_last = active;
}

/* ---------------------------------------------------------------- widgets */

bool widget_button(const UiBox *cell, u32 id, const char *label) {
    bool hot = ui_hover(cell->x, cell->y, cell->w, cell->h);
    bool press = g_in.mouse_pressed[GLFW_MOUSE_BUTTON_LEFT];
    ui_claim(id, hot, press);
    UiState *s = ui_state_get(id);
    if (s) ui_press_flash(s, hot && press);
    u32 col = g_ui_theme.col_btn;
    if (hot) col = g_ui_theme.col_btn_hot;
    if (s && s->flash > 0.0f) col = g_ui_theme.col_btn_down;
    ui_rect(cell->x, cell->y, cell->w, cell->h, col);
    ui_center_text(cell->x, cell->y, cell->w, cell->h, g_ui_theme.text_size, g_ui_theme.col_text, label);
    bool key_fire = ui_focused(id) && (key_pressed(GLFW_KEY_ENTER) || key_pressed(GLFW_KEY_KP_ENTER) || key_pressed(GLFW_KEY_SPACE));
    return ui_clicked(cell->x, cell->y, cell->w, cell->h, GLFW_MOUSE_BUTTON_LEFT) || key_fire;
}

UiStep widget_cycle_row(const UiBox *cell, u32 id, const char *label, const char *value) {
    const float aw = 40.0f; /* arrow zone width, same proportions as the old menu.c row */
    char text[160];
    snprintf(text, sizeof text, "%s: %s", label, value);
    bool hot_l = ui_hover(cell->x, cell->y, aw, cell->h);
    bool hot_r = ui_hover(cell->x + cell->w - aw, cell->y, aw, cell->h);
    bool hot_mid = ui_hover(cell->x + aw, cell->y, cell->w - 2.0f * aw, cell->h);
    bool press = g_in.mouse_pressed[GLFW_MOUSE_BUTTON_LEFT];
    ui_claim(id, hot_l || hot_mid || hot_r, press);
    UiState *s = ui_state_get(id);
    if (s) ui_press_flash(s, (hot_l || hot_mid || hot_r) && press);
    ui_rect(cell->x, cell->y, cell->w, cell->h, (s && s->flash > 0.0f) ? g_ui_theme.col_btn_down : g_ui_theme.col_btn);
    if (hot_l) ui_rect(cell->x, cell->y, aw, cell->h, g_ui_theme.col_btn_hot);
    if (hot_r) ui_rect(cell->x + cell->w - aw, cell->y, aw, cell->h, g_ui_theme.col_btn_hot);
    ui_center_text(cell->x, cell->y, aw, cell->h, g_ui_theme.text_size, g_ui_theme.col_text, "<");
    ui_center_text(cell->x + cell->w - aw, cell->y, aw, cell->h, g_ui_theme.text_size, g_ui_theme.col_text, ">");
    ui_center_text(cell->x + aw, cell->y, cell->w - 2.0f * aw, cell->h, g_ui_theme.text_size, g_ui_theme.col_text, text);
    /* Whole-row clicks cycle too, so a mouse-only player can set everything with one hand. */
    if (ui_clicked(cell->x, cell->y, aw, cell->h, GLFW_MOUSE_BUTTON_LEFT)) return UI_STEP_BACK;
    if (ui_clicked(cell->x + cell->w - aw, cell->y, aw, cell->h, GLFW_MOUSE_BUTTON_LEFT)) return UI_STEP_FWD;
    if (ui_clicked(cell->x + aw, cell->y, cell->w - 2.0f * aw, cell->h, GLFW_MOUSE_BUTTON_LEFT)) return UI_STEP_FWD;
    if (ui_clicked(cell->x + aw, cell->y, cell->w - 2.0f * aw, cell->h, GLFW_MOUSE_BUTTON_RIGHT)) return UI_STEP_BACK;
    if (ui_focused(id)) { /* arrows step while focused: the settings screens work without a mouse */
        if (key_pressed(GLFW_KEY_LEFT)) return UI_STEP_BACK;
        if (key_pressed(GLFW_KEY_RIGHT)) return UI_STEP_FWD;
    }
    return UI_STEP_NONE;
}

bool widget_slider(const UiBox *cell, u32 id, const char *label, float *value, float min, float max, float step) {
    const float knob_w = 12.0f, bar_h = 8.0f;
    UiBox row = *cell;
    UiBox head = layout_next(&row, g_ui_theme.text_size + 4.0f);
    UiBox track = layout_next(&row, row.h);
    float range = MAX(max - min, 1e-6f);
    /* The readout is the value itself when it is already a nice number (fov 70), and a percentage when the
     * range is a 0..1 scale factor (render scale), which is what the settings screens actually show. */
    char text[64];
    if (min >= 1.0f || max <= 0.0f || fmodf(min, 1.0f) != 0.0f || fmodf(max, 1.0f) != 0.0f)
        snprintf(text, sizeof text, "%d%%", (int)((*value - min) / range * 100.0f + 0.5f));
    else snprintf(text, sizeof text, "%d", (int)*value);
    ui_text(head.x, head.y, g_ui_theme.text_size * 0.8f, g_ui_theme.col_text, label);
    float tw = ui_text_width(g_ui_theme.text_size * 0.8f, text);
    ui_text(head.x + head.w - tw, head.y, g_ui_theme.text_size * 0.8f, g_ui_theme.col_dim, text);

    bool hot = ui_hover(track.x, track.y, track.w, track.h);
    bool press = g_in.mouse_pressed[GLFW_MOUSE_BUTTON_LEFT];
    ui_claim(id, hot, press);
    UiState *s = ui_state_get(id);
    bool held = s && W.grab == id;
    float inner_w = MAX(track.w - knob_w, 1.0f);
    float cy = track.y + track.h * 0.5f;

    float want = *value;
    if (held) {
        /* Drag follows the cursor even outside the trough; releasing anywhere ends the grab. */
        want = min + CLAMP((float)(g_in.mouse_x - track.x - knob_w * 0.5f) / inner_w, 0.0f, 1.0f) * range;
    } else if (ui_focused(id)) {
        if (key_down(GLFW_KEY_LEFT)) want = *value - range * UI_SLIDER_KEY_STEP;
        if (key_down(GLFW_KEY_RIGHT)) want = *value + range * UI_SLIDER_KEY_STEP;
    }
    if (step > 0.0f) want = roundf(want / step) * step; /* snap before clamping so the ends land exactly */
    want = CLAMP(want, min, max);
    bool changed = want != *value;
    if (changed) *value = want;

    float t = CLAMP((*value - min) / range, 0.0f, 1.0f);
    float knob_x = track.x + t * inner_w;
    ui_rect(track.x, cy - bar_h * 0.5f, track.w, bar_h, g_ui_theme.col_slider_trough);
    ui_rect(track.x, cy - bar_h * 0.5f, knob_x + knob_w * 0.5f - track.x, bar_h, g_ui_theme.col_slider_fill);
    ui_rect(knob_x, cy - track.h * 0.5f + 2.0f, knob_w, MAX(track.h - 4.0f, 6.0f),
            held ? g_ui_theme.col_btn_down : (hot ? g_ui_theme.col_btn_hot : g_ui_theme.col_btn));
    return changed;
}

bool widget_text_field(const UiBox *cell, const char *label, const char *value, size_t cap, bool active) {
    UiBox row = *cell;
    UiBox head = layout_next_extent(&row, 18.0f, label && label[0] ? 18.0f : 0.0f);
    UiBox body = layout_next(&row, row.h);
    if (head.h > 0.0f) ui_text(head.x, head.y, g_ui_theme.text_size * 0.78f, g_ui_theme.col_dim, label);
    ui_rect(body.x, body.y, body.w, body.h, active ? g_ui_theme.col_field_active : g_ui_theme.col_field);
    char shown[128];
    snprintf(shown, sizeof shown, "%s%s", value, active && ((int)(time_now_s() * 2.0) & 1) ? "_" : "");
    ui_text(body.x + 10.0f, body.y + (body.h - g_ui_theme.text_size) * 0.5f - 2.0f, g_ui_theme.text_size,
            g_ui_theme.col_text, shown);
    (void)cap; /* editing stays in app code: field limits (names vs seeds) are app policy, not widget policy */
    return false;
}

bool widget_slot(const UiBox *cell, u16 state, u8 count, bool selected, UiIconFn draw_icon) {
    float sz = MIN(cell->w, cell->h);
    ui_rect(cell->x, cell->y, sz, sz, rgba(20, 22, 28, 190));
    if (selected) {
        u32 c = rgba(255, 255, 255, 235);
        ui_rect(cell->x - 2, cell->y - 2, sz + 4, 2, c);
        ui_rect(cell->x - 2, cell->y + sz, sz + 4, 2, c);
        ui_rect(cell->x - 2, cell->y, 2, sz, c);
        ui_rect(cell->x + sz, cell->y, 2, sz, c);
    }
    bool hot = ui_hover(cell->x, cell->y, sz, sz);
    if (!count) return hot;
    float pad = sz * 0.09f;
    if (draw_icon) draw_icon(state, cell->x + pad, cell->y + pad, sz - 2.0f * pad);
    if (count > 1) {
        char n[8];
        snprintf(n, sizeof n, "%d", count);
        float ts = g_ui_theme.text_size * 0.85f;
        ui_text(cell->x + sz - ui_text_width(ts, n) - 3.0f, cell->y + sz - ts - 1.0f, ts, rgba(255, 255, 255, 240), n);
    }
    return hot;
}

/* ---------------------------------------------------- scrollable containers
 * A pane is three boxes: the clipped view, a content column tall enough for anything, and the scrollbar
 * gutter. Begin applies the scissor, the caller lays out children into sc.content, End releases it. */

UiScroll ui_scroll_begin(u32 id, UiBox box, float content_h) {
    UiScroll sc;
    sc.view = box;
    sc.max_scroll = content_h > 0.0f ? MAX(content_h - box.h, 0.0f) : UI_TRACK_UNKNOWN;
    UiState *s = ui_state_get(id);
    sc.scroll = s ? CLAMP(s->scroll, 0.0f, sc.max_scroll) : 0.0f;
    sc.dragging = false;
    bool overflow = sc.max_scroll > 0.0f && sc.max_scroll < UI_TRACK_UNKNOWN;
    const float bar_w = 8.0f;
    sc.scrollbar = overflow ? (UiBox){box.x + box.w - bar_w, box.y, bar_w, box.h, UI_STACK_V, 0.0f, true}
                            : (UiBox){box.x + box.w, box.y, 0.0f, 0.0f, UI_STACK_V, 0.0f, true};
    if (overflow) sc.view.w = MAX(box.w - bar_w - 6.0f, 0.0f); /* keep children clear of the bar */
    sc.content = (UiBox){sc.view.x, sc.view.y - sc.scroll, sc.view.w, 1e9f, UI_STACK_V, box.gap, true};
    ui_clip((int)box.x, (int)box.y, (int)box.w, (int)box.h);
    return sc;
}

void ui_scroll_update(UiScroll *sc, u32 id) {
    UiState *s = ui_state_get(id);
    if (!s) return;
    bool in_view = ui_hover(sc->view.x, sc->view.y, sc->view.w, sc->view.h);
    bool in_bar = sc->scrollbar.w > 0.0f &&
                  ui_hover(sc->scrollbar.x - 2.0f, sc->scrollbar.y, sc->scrollbar.w + 4.0f, sc->scrollbar.h);
    if (in_view) s->scroll -= (float)g_in.scroll * UI_SCROLL_WHEEL;
    if (in_bar && g_in.mouse_pressed[GLFW_MOUSE_BUTTON_LEFT]) W.grab = id;
    bool held = W.grab == id;
    if (held && sc->scrollbar.w > 0.0f) {
        /* Jump-to-click then follow the thumb: center the thumb on the cursor, in track space. */
        float frac = MIN(sc->view.h / MAX(sc->view.h + sc->max_scroll, 1.0f), 1.0f);
        float th = MAX(sc->scrollbar.h * frac, 20.0f);
        float p = (float)(g_in.mouse_y - sc->scrollbar.y - th * 0.5f) / MAX(sc->scrollbar.h - th, 1.0f);
        s->scroll = CLAMP(p, 0.0f, 1.0f) * sc->max_scroll;
    }
    s->scroll = CLAMP(s->scroll, 0.0f, sc->max_scroll);
    sc->scroll = s->scroll;
    sc->dragging = held;
}

void ui_scroll_end(UiScroll *sc) {
    ui_clip(0, 0, 0, 0); /* one call disables the scissor; the bar below draws unclipped */
    ui_scrollbar(sc);
}

void ui_scrollbar(const UiScroll *sc) {
    if (sc->scrollbar.w <= 0.0f) return; /* no overflow: nothing to show */
    ui_rect(sc->scrollbar.x, sc->scrollbar.y, sc->scrollbar.w, sc->scrollbar.h, rgba(0, 0, 0, 90));
    float frac = MIN(sc->view.h / MAX(sc->view.h + sc->max_scroll, 1.0f), 1.0f);
    float th = MAX(sc->scrollbar.h * frac, 20.0f);
    float ty = sc->scrollbar.y + (sc->scrollbar.h - th) * (sc->scroll / MAX(sc->max_scroll, 1.0f));
    ui_rect(sc->scrollbar.x, ty, sc->scrollbar.w, th, g_ui_theme.col_scrollbar);
}

/* -------------------------------------------------------------- tooltips */

void ui_tooltip_anchor(u32 id, const UiBox *cell, const char *text) {
    if (!text || !text[0] || W.tip_count >= UI_TIP_MAX) return; /* slots full: first anchors win, cheap and predictable */
    struct TipSlot *tip = &W.tips[W.tip_count++];
    tip->id = id; tip->cell = *cell; tip->text = text;
}

void ui_tooltips_update(float dt) {
    W.tip_id = 0;
    for (int i = 0; i < W.tip_count; i++) {
        const UiBox *c = &W.tips[i].cell;
        if (ui_hover(c->x, c->y, c->w, c->h)) { W.tip_id = W.tips[i].id; break; }
    }
    if (!W.tip_id) W.tip_timer = 0.0f; /* moved off: the delay restarts for the next thing hovered */
    else W.tip_timer += dt;
}

void ui_tooltips_draw(void) {
    if (!W.tip_id || W.tip_timer < UI_HOVER_DELAY) return;
    for (int i = 0; i < W.tip_count; i++) {
        if (W.tips[i].id != W.tip_id) continue;
        const UiBox *c = &W.tips[i].cell;
        float ts = g_ui_theme.text_size * 0.8f;
        const char *text = W.tips[i].text;
        float w = ui_text_width(ts, text) + 12.0f, h = ts + 10.0f;
        float x = (float)g_in.mouse_x + 12.0f, y = c->y - h - 4.0f;
        if (y < 0.0f) y = c->y + c->h + 4.0f; /* no room above the widget: flip below it */
        x = MIN(x, (float)g_win.width - w - 4.0f); /* never run off the right edge */
        ui_rect(x, y, w, h, rgba(10, 10, 16, 235));
        ui_text(x + 6.0f, y + 5.0f, ts, rgba(240, 240, 250, 255), text);
        return;
    }
}
