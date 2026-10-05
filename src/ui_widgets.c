#include "ui_widgets.h"
#include <GLFW/glfw3.h>

typedef struct Tooltip {
    UIWRect bounds;
    char text[64];
    float font_size;
} Tooltip;

static VEC(Tooltip) g_tooltips;

static bool point_in_rect(double x, double y, UIWRect bounds) {
    return x >= bounds.x && x < bounds.x + bounds.w && y >= bounds.y && y < bounds.y + bounds.h;
}

UIWStack uiw_vstack(UIWRect bounds, float gap) {
    UIWStack stack = {bounds, gap, bounds.y, false};
    return stack;
}

UIWStack uiw_hstack(UIWRect bounds, float gap) {
    UIWStack stack = {bounds, gap, bounds.x, true};
    return stack;
}

UIWRect uiw_stack_next(UIWStack *stack, float extent) {
    UIWRect item;
    if (stack->horizontal) {
        item = (UIWRect){stack->cursor, stack->bounds.y, extent, stack->bounds.h};
    } else {
        item = (UIWRect){stack->bounds.x, stack->cursor, stack->bounds.w, extent};
    }
    stack->cursor += extent + stack->gap;
    return item;
}

UIWRect uiw_grid_cell(UIWRect bounds, int columns, int index, float gap_x, float gap_y, float cell_h) {
    if (columns < 1) return (UIWRect){bounds.x, bounds.y, 0, 0};
    float cell_w = (bounds.w - gap_x * (float)(columns - 1)) / (float)columns;
    int column = index % columns;
    int row = index / columns;
    return (UIWRect){bounds.x + (float)column * (cell_w + gap_x),
                     bounds.y + (float)row * (cell_h + gap_y), cell_w, cell_h};
}

UIWRect uiw_inset(UIWRect bounds, float padding) {
    return (UIWRect){bounds.x + padding, bounds.y + padding,
                     MAX(0.0f, bounds.w - padding * 2.0f), MAX(0.0f, bounds.h - padding * 2.0f)};
}

bool uiw_hovered(UIWRect bounds) {
    return point_in_rect(g_in.mouse_x, g_in.mouse_y, bounds);
}

static void bevel(UIWRect bounds, u32 fill, u32 light, u32 dark) {
    float border = MIN(1.0f, MIN(bounds.w, bounds.h) * 0.5f);
    ui_rect(bounds.x, bounds.y, bounds.w, bounds.h, rgba(0, 0, 0, 255));
    ui_rect(bounds.x + border, bounds.y + border, bounds.w - 2 * border, bounds.h - 2 * border, fill);
    ui_rect(bounds.x + border, bounds.y + border, bounds.w - 2 * border, border, light);
    ui_rect(bounds.x + border, bounds.y + border, border, bounds.h - 2 * border, light);
    ui_rect(bounds.x + border, bounds.y + bounds.h - 3 * border, bounds.w - 2 * border, 2 * border, dark);
    ui_rect(bounds.x + bounds.w - 2 * border, bounds.y + border, border, bounds.h - 2 * border, dark);
}

static float text_top(UIWRect bounds, float size) {
    return bounds.y + (bounds.h - size) * 0.5f - size * 0.11f;
}

int uiw_button_action(int id, UIWRect bounds, const char *label, bool enabled, float font_size, u32 label_color) {
    (void)id;
    bool hot = enabled && uiw_hovered(bounds);
    if (!enabled) bevel(bounds, rgba(58, 58, 58, 255), rgba(80, 80, 80, 255), rgba(40, 40, 40, 255));
    else if (hot) bevel(bounds, rgba(112, 112, 168, 255), rgba(172, 172, 226, 255), rgba(66, 66, 112, 255));
    else bevel(bounds, rgba(112, 112, 112, 255), rgba(172, 172, 172, 255), rgba(62, 62, 62, 255));
    u32 color = !enabled ? rgba(160, 160, 160, 255) : label_color ? label_color : hot ? rgba(255, 255, 160, 255) : rgba(255, 255, 255, 255);
        ui_text(bounds.x + (bounds.w - ui_text_width(font_size, label)) * 0.5f,
            text_top(bounds, font_size), font_size, color, label);
    if (!enabled || !hot) return 0;
    if (g_in.mouse_pressed[GLFW_MOUSE_BUTTON_LEFT]) return 1;
    if (g_in.mouse_pressed[GLFW_MOUSE_BUTTON_RIGHT]) return -1;
    return 0;
}

bool uiw_button(int id, UIWRect bounds, const char *label, bool enabled, float font_size) {
    return uiw_button_action(id, bounds, label, enabled, font_size, 0) == 1;
}

int uiw_cycle_row(int id, UIWRect bounds, const char *label, const char *value, bool enabled, float font_size) {
    char text[160];
    snprintf(text, sizeof text, "%s: %s", label, value);
    return uiw_button_action(id, bounds, text, enabled, font_size, 0);
}

static bool valid_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_';
}

bool uiw_text_field(int id, UIWRect bounds, const char *label, char *value, size_t capacity,
                    bool active, bool names_only, float font_size) {
    (void)id;
    bool changed = false;
    if (active && capacity > 0) {
        size_t length = strlen(value);
        for (int i = 0; i < g_in.text_len; i++) {
            char c = g_in.text[i];
            if (c == ' ' && names_only) c = '_';
            if (names_only ? !valid_name_char(c) : c < 32) continue;
            if (length + 1 < capacity) {
                value[length++] = c;
                value[length] = 0;
                changed = true;
            }
        }
        if (key_pressed(GLFW_KEY_BACKSPACE) && length > 0) {
            value[length - 1] = 0;
            changed = true;
        }
    }
    float size = font_size;
    if (label && label[0]) ui_text(bounds.x, bounds.y - size * 1.375f, size * 0.9f, rgba(160, 160, 160, 255), label);
    ui_rect(bounds.x, bounds.y, bounds.w, bounds.h, active ? rgba(255, 255, 255, 255) : rgba(160, 160, 160, 255));
    ui_rect(bounds.x + 1, bounds.y + 1, bounds.w - 2, bounds.h - 2, rgba(0, 0, 0, 255));
    char shown[256];
    snprintf(shown, sizeof shown, "%s%s", value, active && ((int)(time_now_s() * 2.0) & 1) ? "_" : "");
    ui_text(bounds.x + 4, text_top(bounds, size), size, rgba(224, 224, 224, 255), shown);
    return changed;
}

void uiw_slot(UIWRect bounds, bool selected, const char *count, UIWSlotContentFn draw_content, void *user) {
    ui_rect(bounds.x, bounds.y, bounds.w, bounds.h, rgba(20, 22, 28, 190));
    if (selected) {
        u32 white = rgba(255, 255, 255, 235);
        ui_rect(bounds.x - 2, bounds.y - 2, bounds.w + 4, 2, white);
        ui_rect(bounds.x - 2, bounds.y + bounds.h, bounds.w + 4, 2, white);
        ui_rect(bounds.x - 2, bounds.y, 2, bounds.h, white);
        ui_rect(bounds.x + bounds.w, bounds.y, 2, bounds.h, white);
    }
    if (draw_content) draw_content(uiw_inset(bounds, 3), user);
    if (count && count[0]) {
        float size = 7.0f;
        ui_text(bounds.x + bounds.w - ui_text_width(size, count) - 3,
                bounds.y + bounds.h - size - 1, size, rgba(255, 255, 255, 240), count);
    }
}

void uiw_tooltip(UIWRect bounds, const char *text, float font_size) {
    if (!text || !text[0] || !uiw_hovered(bounds)) return;
    Tooltip tt = {bounds, {0}, font_size};
    snprintf(tt.text, sizeof tt.text, "%s", text);
    vec_push(g_tooltips, tt);
}

void uiw_tooltip_draw_all(void) {
    for (int i = 0; i < g_tooltips.n; i++) {
        Tooltip *tt = &g_tooltips.d[i];
        float size = MAX(1.0f, tt->font_size), scale = size / 8.0f;
        float padding = 5.0f * scale, cursor_gap = 10.0f * scale;
        float width = ui_text_width(size, tt->text) + padding * 2.0f;
        float height = size + padding * 2.0f;
        float x = (float)g_in.mouse_x + cursor_gap, y = (float)g_in.mouse_y - 4.0f * scale;
        if (x + width > (float)g_win.width) x = (float)g_in.mouse_x - width - cursor_gap;
        if (y + height > (float)g_win.height) y = (float)g_win.height - height;
        x = MAX(0.0f, x);
        y = MAX(0.0f, y);
        ui_rect(x, y, width, height, rgba(10, 10, 16, 230));
        ui_text(x + padding, y + padding, size, rgba(240, 240, 250, 255), tt->text);
    }
    g_tooltips.n = 0;
}

static float slider_value(float x, UIWRect bounds, float minimum, float maximum, float step) {
    float ratio = CLAMP((x - bounds.x) / bounds.w, 0.0f, 1.0f);
    float value = minimum + ratio * (maximum - minimum);
    if (step > 0.0f) value = minimum + roundf((value - minimum) / step) * step;
    return CLAMP(value, minimum, maximum);
}

bool uiw_slider(UIWState *state, int id, UIWRect bounds, float *value, float minimum, float maximum, float step) {
    if (!state || !value || maximum <= minimum || bounds.w <= 0.0f) return false;
    bool hot = uiw_hovered(bounds);
    if (g_in.mouse_pressed[GLFW_MOUSE_BUTTON_LEFT] && hot) state->active_id = (u32)id;
    bool dragging = state->active_id == (u32)id && g_in.mouse_buttons[GLFW_MOUSE_BUTTON_LEFT];
    if (state->active_id == (u32)id && !g_in.mouse_buttons[GLFW_MOUSE_BUTTON_LEFT] &&
        !g_in.mouse_pressed[GLFW_MOUSE_BUTTON_LEFT]) state->active_id = 0;
    bool changed = false;
    if (dragging) {
        float next = slider_value((float)g_in.mouse_x, bounds, minimum, maximum, step);
        if (next != *value) { *value = next; changed = true; }
    }
    float center_y = bounds.y + bounds.h * 0.5f;
    float ratio = CLAMP((*value - minimum) / (maximum - minimum), 0.0f, 1.0f);
    float knob_x = bounds.x + ratio * bounds.w;
    ui_rect(bounds.x, center_y - 2.0f, bounds.w, 4.0f, rgba(20, 20, 20, 230));
    ui_rect(bounds.x, center_y - 1.0f, ratio * bounds.w, 2.0f, rgba(180, 180, 180, 255));
    ui_rect(knob_x - 3.0f, center_y - 6.0f, 6.0f, 12.0f,
            state->active_id == (u32)id || hot ? rgba(255, 255, 255, 255) : rgba(190, 190, 190, 255));
    return changed;
}

float uiw_scroll_begin(UIWScroll *state, UIWRect viewport, float content_height) {
    if (!state) return 0.0f;
    float max_offset = MAX(0.0f, content_height - viewport.h);
    if (uiw_hovered(viewport) && g_in.scroll != 0.0) state->offset -= (float)g_in.scroll * 24.0f;
    state->offset = CLAMP(state->offset, 0.0f, max_offset);
    ui_clip((int)viewport.x, (int)viewport.y, (int)viewport.w, (int)viewport.h);
    return -state->offset;
}

void uiw_scroll_end(const UIWScroll *state, UIWRect viewport, float content_height, bool scrollbar) {
    ui_clip(0, 0, 0, 0);
    if (!state || !scrollbar || content_height <= viewport.h || viewport.h <= 0.0f) return;
    float track_h = viewport.h;
    float thumb_h = MAX(8.0f, track_h * viewport.h / content_height);
    float max_offset = content_height - viewport.h;
    float thumb_y = viewport.y + (track_h - thumb_h) * (max_offset > 0.0f ? state->offset / max_offset : 0.0f);
    float bar_x = viewport.x + viewport.w - 4.0f;
    ui_rect(bar_x, viewport.y, 2.0f, track_h, rgba(0, 0, 0, 100));
    ui_rect(bar_x, thumb_y, 2.0f, thumb_h, rgba(190, 190, 190, 220));
}