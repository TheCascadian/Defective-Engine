/* ui_widgets.h - a generic, immediate-mode widget layer on top of the 2D batch in render.c.
 *
 * Design: every widget is one function call per frame that draws itself, reads the input and returns what
 * happened. No widget tree is kept between frames; only the few pieces of state that cannot be recomputed
 * (hover timers, scroll offsets, slider drag) live in a small fixed-size table keyed by the widget's ID.
 *
 * IDs: pass any unique u32. The convention in this engine is `ui_id("settings.render_scale")` - hash the
 * dotted path of the setting, which also gives mods stable IDs across hot reloads. Pointer values work too
 * (`ui_id_of(&g_settings.vsync)`).
 *
 * Layout: UiBox + layout_next() is the C equivalent of a CSS flex container. You describe a rect and a
 * direction, then ask it for the next cell; the cursor advances automatically, like `flex-direction: column`
 * with a fixed `gap`. Boxes are plain structs passed by pointer and never allocate. */
#pragma once

#include "dfe.h"

/* ------------------------------------------------------------- identifiers */

static inline u32 ui_id(const char *key) { return hash_str(key); }
static inline u32 ui_id_of(const void *p) { return (u32)(uintptr_t)p; }

/* ------------------------------------------------------------------- theme */

typedef struct UiTheme {
    float text_size;      /* body text height in pixels */
    float row_h;          /* default widget height, like a CSS line-height for controls */
    float gap;            /* default spacing between stacked items */
    float pad;            /* default inner padding of containers */
    u32 col_bg;           /* panel background */
    u32 col_btn;          /* control at rest */
    u32 col_btn_hot;      /* control under the cursor */
    u32 col_btn_down;     /* control being pressed or active */
    u32 col_text;
    u32 col_dim;          /* secondary text, hints */
    u32 col_warn;
    u32 col_field;        /* text field at rest */
    u32 col_field_active;
    u32 col_slider_trough;
    u32 col_slider_fill;
    u32 col_scrollbar;
} UiTheme;
extern UiTheme g_ui_theme;
void ui_theme_defaults(void); /* called once from ui_begin(); menus may overwrite fields afterwards */

/* ------------------------------------------------------------ input helpers */

bool ui_hover(float x, float y, float w, float h);                            /* cursor inside the rect */
bool ui_clicked(float x, float y, float w, float h, int mouse_button);        /* press started inside */
void ui_center_text(float x, float y, float w, float h, float size, u32 color, const char *text);

/* --------------------------------------------------------------- box layout */

typedef enum { UI_STACK_V = 0, UI_STACK_H } UiDir;

typedef struct UiBox {
    float x, y, w, h;   /* the remaining area, exactly like a flex container shrinking as children take space */
    UiDir dir;          /* main axis: column (V) or row (H) */
    float gap;          /* space inserted before every child after the first */
    bool first;         /* true until the first child has been taken; keeps gaps off the leading edge */
} UiBox;

/* A box that lays its children out along `dir`, inset by `pad`, with `gap` between them. */
UiBox ui_box(float x, float y, float w, float h, UiDir dir, float gap, float pad);
/* Takes the next cell of `size` along the main axis and returns it. Cross axis fills the box. */
UiBox layout_next(UiBox *b, float size);
/* Same, but the caller picks the extent along the main axis (like flex-basis on one item). */
UiBox layout_next_extent(UiBox *b, float size, float extent);
/* Widgets take their cell by const pointer; this turns "take the next cell and hand it over" into one
 * statement: `UiCell(btn, &col, ROW_H); widget_button(&btn, ...)`. In C you cannot take the address of a
 * temporary the way JS hands around object literals, so we materialize the cell first.
 * One struct on the stack per widget, no heap, nothing retained. */
#define UiCell(cell_var, box_ptr, size) UiBox cell_var = layout_next((box_ptr), (size))
/* Cell `index` of a `cols`-wide grid inside `b` (CSS grid without the template parsing). `rows` may be 0
 * when only the index matters; pass the visible row count to stretch the cells to fill the box. */
UiBox layout_grid_cell(const UiBox *b, int cols, int rows, float gap, int index);
/* Centers a fixed-size rect inside another one - the "margin: auto" of this system. */
UiBox ui_center_in(const UiBox *outer, float w, float h);

/* ------------------------------------------------------- persistent state */

typedef struct UiState {
    u32 id;
    float scroll;   /* scrollable containers, in pixels */
    bool held;      /* slider owns the mouse button */
    bool hot_last;  /* previous frame hover, drives the click flash timer */
    float flash;    /* seconds left of the pressed look */
} UiState;
UiState *ui_state_get(u32 id); /* NULL when the fixed table is full - widgets simply lose their memory */

/* ---------------------------------------------------------------- widgets */

/* Returns true on the frame the button was activated (left click inside, or Enter/Space while focused). */
bool widget_button(const UiBox *cell, u32 id, const char *label);
/* Focus model: the keyboard goes to the widget the cursor is over, or - while the mouse button is held -
 * the widget that grabbed it. That keeps sliders draggable off-widget without a retained focus chain. */
u32 ui_focus_id(void);
bool ui_focused(u32 id);     /* true when `id` owns the keyboard right now */
bool ui_key_held(int key);   /* key_down for keys that are not movement keys while any UI has focus */
void ui_clear_focus(void);   /* call when a screen closes so stale focus does not fire a hidden widget */

typedef enum { UI_STEP_NONE = 0, UI_STEP_BACK, UI_STEP_FWD } UiStep;
/* Arrows on both sides plus "< label: value >" in the middle. Middle click steps forward, right click back. */
UiStep widget_cycle_row(const UiBox *cell, u32 id, const char *label, const char *value);

/* Horizontal slider over a float range. Returns true when the value changed this frame.
 * Dragging works even when the cursor leaves the trough, so a slow laptop with a jumpy cursor stays usable. */
bool widget_slider(const UiBox *cell, u32 id, const char *label, float *value, float min, float max, float step);

/* Text field. Returns true when the text was edited. `active` is the caller's own selection state, because
 * which of several fields is active is app logic (the title screen lets Tab switch name/seed). */
bool widget_text_field(const UiBox *cell, const char *label, const char *value, size_t cap, bool active);

/* Inventory slot: icon, count and an optional selection frame. Returns true when the cursor is over it,
 * which the caller uses to drive the tooltip below. The icon callback is how hud.c plugs its atlas drawing in
 * without this module knowing anything about blocks - the same seam mods use for custom slot content. */
typedef void (*UiIconFn)(u16 state, float x, float y, float size);
bool widget_slot(const UiBox *cell, u16 state, u8 count, bool selected, UiIconFn draw_icon);

/* ------------------------------------------------- scrollable containers */

typedef struct UiScroll {
    UiBox view;      /* the clipped window; draw children through sc.content */
    UiBox content;   /* an infinite-height column: layout_next() always succeeds inside it */
    UiBox scrollbar; /* empty unless the content overflowed */
    float max_scroll;
    float scroll;
    bool dragging;
} UiScroll;
/* `content_h` may be <= 0 when unknown: the wheel still scrolls and the bar just fills the track. */
UiScroll ui_scroll_begin(u32 id, UiBox box, float content_h);
/* Wheel over the view, thumb drag on the bar. Call right after ui_scroll_begin(), before laying out rows,
 * so the visible cells are computed from the final offset. */
void ui_scroll_update(UiScroll *sc, u32 id);
void ui_scroll_end(UiScroll *sc);    /* releases the clip and draws the bar; must run even if the body early-returns */
void ui_scrollbar(const UiScroll *sc); /* bar only, for callers that manage the clip themselves */

/* ------------------------------------------------------------------ frame */

/* Per-frame housekeeping (timers, focus reset, tooltip slots). Called once from ui_begin(); app code that
 * drives its own UI loop (the title screen) may call it too - it is idempotent within a frame. */
void ui_frame_begin(void);

/* ------------------------------------------------------------- tooltips */

void ui_tooltip_anchor(u32 id, const UiBox *cell, const char *text); /* register during the widget pass */
void ui_tooltips_update(float dt);                                   /* dwell timer with an explicit dt */
void ui_tooltips_tick(void);                                         /* dwell timer using the frame dt from ui_frame_begin() */
void ui_tooltips_draw(void);                                         /* last, so a tip is never clipped away */
