#pragma once
#include "dfe.h"

typedef struct UIWRect {
    float x, y, w, h;
} UIWRect;

/* A stack is a CSS-like flex row or column: next() advances one fixed-size item. */
typedef struct UIWStack {
    UIWRect bounds;
    float gap;
    float cursor;
    bool horizontal;
} UIWStack;

typedef struct UIWState {
    u32 active_id;
} UIWState;

typedef struct UIWScroll {
    float offset;
} UIWScroll;

typedef void (*UIWSlotContentFn)(UIWRect inner, void *user);

UIWStack uiw_vstack(UIWRect bounds, float gap);
UIWStack uiw_hstack(UIWRect bounds, float gap);
UIWRect uiw_stack_next(UIWStack *stack, float extent);
UIWRect uiw_grid_cell(UIWRect bounds, int columns, int index, float gap_x, float gap_y, float cell_h);
UIWRect uiw_inset(UIWRect bounds, float padding);

bool uiw_hovered(UIWRect bounds);
int uiw_button_action(int id, UIWRect bounds, const char *label, bool enabled, float font_size, u32 label_color);
bool uiw_button(int id, UIWRect bounds, const char *label, bool enabled, float font_size);
int uiw_cycle_row(int id, UIWRect bounds, const char *label, const char *value, bool enabled, float font_size);
bool uiw_text_field(int id, UIWRect bounds, const char *label, char *value, size_t capacity,
                    bool active, bool names_only, float font_size);
void uiw_slot(UIWRect bounds, bool selected, const char *count, UIWSlotContentFn draw_content, void *user);
void uiw_tooltip(UIWRect bounds, const char *text, float font_size);
bool uiw_slider(UIWState *state, int id, UIWRect bounds, float *value, float minimum, float maximum, float step);

/* Begin clips content and returns its negative Y translation. End restores clipping and draws the optional bar. */
float uiw_scroll_begin(UIWScroll *state, UIWRect viewport, float content_height);
void uiw_scroll_end(const UIWScroll *state, UIWRect viewport, float content_height, bool scrollbar);