#pragma once
#include "dfe.h"

/* Generic screens built from widgets. Coordinates are relative to the screen panel, which is centred in the window.
 * Logic stays in callbacks (C functions, or Lua through script.c). */
typedef enum { WIDGET_PANEL, WIDGET_LABEL, WIDGET_BUTTON, WIDGET_SLOT, WIDGET_BAR, WIDGET_LIST, WIDGET_SCROLL } WidgetType;

#define SCREEN_MAX 32
#define SCREEN_MAX_WIDGETS 64
#define SCREEN_STACK 8
#define WIDGET_MAX_ITEMS 64

typedef void (*ScreenFn)(const char *screen, const char *widget, int index, void *user);

typedef struct Widget {
    WidgetType type;
    char id[24], text[64];
    float x, y, w, h;
    float value, max;            /* bar fill */
    bool enabled, focusable;
    int selected, scroll;        /* list/scroll state */
    char items[WIDGET_MAX_ITEMS][32];
    int item_count;
    ScreenFn on_click;
    void *user;
    void (*release)(void *user); /* frees user when the screen is removed */
} Widget;

typedef struct ScreenDef {
    char id[32], title[48];
    float w, h;
    bool modal;                  /* blocks the screen below it and draws a dim backdrop */
    bool close_on_escape;
    ScreenFn on_open, on_close;
    void *user;
    void (*release)(void *user); /* called when the screen is removed */
    bool scripted;               /* owned by a script; dropped when scripts shut down */
} ScreenDef;

bool screen_register(const ScreenDef *def);                  /* false: duplicate id, empty id or table full */
bool screen_add_widget(const char *screen, const Widget *w); /* false: unknown screen, duplicate widget id or full */
Widget *screen_widget(const char *screen, const char *widget);
bool screen_open(const char *id);                            /* pushes on the stack and fires on_open */
bool screen_close(void);                                     /* pops the top screen and fires on_close */
void screen_close_all(void);
bool screen_is_open(const char *id);                         /* any screen when id is NULL */
const char *screen_top(void);
bool screen_unregister(const char *id);
void screen_remove_scripted(void);
void screen_shutdown(void);
bool screen_click_at(int width, int height, float mx, float my, bool right); /* true when a widget consumed it */
bool screen_key(int glfw_key);                               /* Tab/Shift-Tab focus, Enter/Space activates, Escape closes */
const char *screen_focus(void);
void screen_update(int width, int height);                   /* reads g_in */
void screen_draw(int width, int height);
