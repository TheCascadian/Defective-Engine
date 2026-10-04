#pragma once
#include "dfe.h"

/* Everything the status HUD reads. Custom fields let Lua and plugins add their own bars. */
#define UI_MAX_CUSTOM 16
typedef struct PlayerStatus {
    float health, max_health, absorption;
    float hunger, max_hunger, saturation;
    float stamina, max_stamina;
    float magicka, max_magicka; /* a max of 0 means the player has no pool */
    float xp, xp_next;
    int level;
    float armor, max_armor;
    struct { char name[24]; float value, max; } custom[UI_MAX_CUSTOM];
    int custom_count;
} PlayerStatus;

#define UI_MAX_ELEMENTS 32
bool ui_load(void);   /* parse hud.json and the theme, replacing the current set only if both are valid */
bool ui_reload(void); /* same as ui_load; the previous layout stays on any error */
void ui_status_shutdown(void);
void ui_status_update(void); /* fill the status from the player */
PlayerStatus *ui_status(void);
bool ui_status_set_custom(const char *name, float value, float max);
int ui_element_count(void);
const char *ui_element_id(int i);
bool ui_element_visible(const char *id);                 /* mode default combined with the override */
bool ui_element_set_override(const char *id, int state); /* -1 follow mode, 0 hide, 1 show */
int ui_element_override(const char *id);
bool ui_element_rect(const char *id, int width, int height, float *x, float *y, float *w, float *h);
u32 ui_theme_color(const char *name, u32 fallback);
void ui_status_draw(int width, int height);
void ui_overrides_load(const Json *settings_root);
int ui_overrides_write(char *buf, size_t cap); /* "hud_elements" object body for settings.json */

/* Scaling (see ui.c: every scale that multiplies pixel art is an integer). HUD and UI scale and text scale are four separate settings. Text scale only grows a parent
 * (a label's width, a button's height) when the scaled text no longer fits; it never shrinks one. */
#define UI_FIT_PAD 4.0f
float ui_auto_scale(int width, int height);     /* automatic GUI scale, 1..4 */
float ui_gui_scale(int width, int height);      /* explicit ui_scale or automatic; integer */
float ui_icon_scale(float gui);                 /* max(1, floor(gui / 2)): the integer scale for 16 px icon art drawn at 2x */
float ui_hud_scale(int width, int height);      /* HUD layout scale, integer; hotbar, hearts and bars multiply by it */
float ui_hud_text_scale(void);                  /* clamped 0.75..2 */
float ui_ui_text_scale(void);                   /* clamped 0.75..2 */
/* base size in layout units; text_px is the already scaled font size. Result >= base on both axes and
 * exceeds it by at most the text's own size plus 2*pad, so growth is bounded by text_scale. */
void ui_fit(float base_w, float base_h, const char *text, float text_px, float pad, float *w, float *h);
/* Text measurement defaults to ui_text_width; tests swap in a fixed-advance measure since the font needs GL. */
void ui_set_text_measure(float (*fn)(float size, const char *text));
