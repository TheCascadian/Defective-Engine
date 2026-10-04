/* Data-driven status HUD: elements come from assets/dfe/ui/hud.json, colours from assets/dfe/ui/theme/default.json.
 * Both are parsed once into plain structs; drawing never touches JSON. */
#include "ui.h"
#include "icons.h"

typedef struct Element {
    char id[24], source[24], max[24], color[24], style[8], anchor[16], label[8], icon[24];
    float x, y, w, h, pip_value, pip_size, spacing;
    bool survival, creative, align_right, requires_max, hide_when_empty;
    int override_state; /* -1 follow mode */
} Element;

typedef struct ThemeColor { char name[24]; u32 value; } ThemeColor;

typedef struct Layout {
    Element el[UI_MAX_ELEMENTS];
    int n;
    float scale;
    ThemeColor col[32];
    int ncol;
    float text_size;
} Layout;

static Layout g_layout;
static PlayerStatus g_status;
static bool g_loaded;
static struct { char id[24]; int state; } g_over[UI_MAX_ELEMENTS];
static int g_nover;

/* Pixel-exact scaling. Every layout length is an integer number of screen pixels per art pixel:
 *   ui_auto_scale   window size / 320x360, 1..4 (the automatic GUI scale)
 *   ui_gui_scale    explicit ui_scale or the automatic one; an integer, used for menus and screens
 *   ui_hud_scale    the HUD's integer layout scale; round(auto * hud_scale) in 1..8, then halved (floor, min 1) because
 *                   HUD art is drawn at twice the 16 px icon grid. This is what hotbar, hearts and bars multiply by
 *   ui_icon_scale   the same halving for a GUI scale, used by the inventory
 * Text sizes then snap to the font's pixel grid in ui_snap_text, so scaled text never blurs. */
float ui_hud_text_scale(void) { return CLAMP(g_settings.hud_text_scale > 0 ? g_settings.hud_text_scale : 1.0f, 0.75f, 2.0f); }
float ui_ui_text_scale(void) { return CLAMP(g_settings.ui_text_scale > 0 ? g_settings.ui_text_scale : 1.0f, 0.75f, 2.0f); }

float ui_auto_scale(int width, int height) { return (float)CLAMP(MIN(width / 320, height / 360), 1, 4); }

float ui_gui_scale(int width, int height) {
    if (g_settings.ui_scale > 0) return (float)CLAMP(g_settings.ui_scale, 1, 4);
    return ui_auto_scale(width, height);
}

float ui_icon_scale(float gui) { return MAX(1.0f, floorf(gui * 0.5f)); }

float ui_hud_scale(int width, int height) {
    float mult = CLAMP(g_settings.hud_scale > 0 ? g_settings.hud_scale : 1.0f, 0.5f, 3.0f);
    return ui_icon_scale(CLAMP(roundf(ui_auto_scale(width, height) * mult), 1.0f, 8.0f));
}

static float (*g_measure)(float, const char *);
void ui_set_text_measure(float (*fn)(float, const char *)) { g_measure = fn; }
static float measure(float px, const char *t) { return g_measure ? g_measure(px, t) : ui_text_width(px, t); }

void ui_fit(float base_w, float base_h, const char *text, float text_px, float pad, float *w, float *h) {
    float tw = text && text[0] ? measure(text_px, text) + 2 * pad : 0, th = text && text[0] ? text_px + 2 * pad : 0;
    *w = ceilf(MAX(base_w, tw));
    *h = ceilf(MAX(base_h, th));
}

static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static bool parse_color(const char *s, u32 *out) {
    if (!s || *s != '#') return false;
    size_t n = strlen(s + 1);
    if (n != 6 && n != 8) return false;
    int v[8];
    for (size_t i = 0; i < n; i++) if ((v[i] = hexv(s[1 + i])) < 0) return false;
    *out = rgba(v[0] * 16 + v[1], v[2] * 16 + v[3], v[4] * 16 + v[5], n == 8 ? v[6] * 16 + v[7] : 255);
    return true;
}

static void copy_str(char *dst, size_t cap, const char *s) { snprintf(dst, cap, "%s", s ? s : ""); }


static bool parse_layout(const Json *root, Layout *L, char *err, size_t cap) {
    const Json *els = json_get(root, "elements");
    if (!els || els->type != JSON_ARRAY) { snprintf(err, cap, "hud.json needs an \"elements\" array"); return false; }
    L->scale = CLAMP((float)json_num(root, "scale", 1.0), 0.25f, 8.0f);
    for (int i = 0; i < json_len(els); i++) {
        const Json *e = json_at(els, i);
        if (L->n >= UI_MAX_ELEMENTS) { snprintf(err, cap, "hud.json has more than %d elements", UI_MAX_ELEMENTS); return false; }
        const char *id = json_str(e, "id", "");
        if (!*id) { snprintf(err, cap, "element %d has no \"id\"", i); return false; }
        Element *el = &L->el[L->n++];
        memset(el, 0, sizeof *el);
        el->override_state = -1;
        copy_str(el->id, sizeof el->id, id);
        copy_str(el->source, sizeof el->source, json_str(e, "source", ""));
        copy_str(el->max, sizeof el->max, json_str(e, "max", ""));
        copy_str(el->color, sizeof el->color, json_str(e, "color", "text"));
        copy_str(el->style, sizeof el->style, json_str(e, "style", "bar"));
        copy_str(el->anchor, sizeof el->anchor, json_str(e, "anchor", "top_left"));
        copy_str(el->label, sizeof el->label, json_str(e, "label", ""));
        el->x = (float)json_num(e, "x", 0); el->y = (float)json_num(e, "y", 0);
        el->w = (float)json_num(e, "w", 16); el->h = (float)json_num(e, "h", 8);
        el->pip_value = MAX((float)json_num(e, "pip_value", 2), 0.001f);
        copy_str(el->icon, sizeof el->icon, json_str(e, "icon", ""));
        el->pip_size = (float)json_num(e, "pip_size", 9); el->spacing = (float)json_num(e, "spacing", 1);
        el->align_right = !strcmp(json_str(e, "align", "left"), "right");
        el->requires_max = json_bool(e, "requires_max", false);
        el->hide_when_empty = json_bool(e, "hide_when_empty", false);
        const Json *modes = json_get(e, "modes");
        if (!modes) el->survival = el->creative = true;
        for (int m = 0; modes && m < json_len(modes); m++) {
            const char *s = json_as_str(json_at(modes, m), "");
            if (!strcmp(s, "survival")) el->survival = true; else if (!strcmp(s, "creative")) el->creative = true;
            else { snprintf(err, cap, "element %s: unknown mode \"%s\"", id, s); return false; }
        }
        if (strcmp(el->style, "bar") && strcmp(el->style, "pips")) { snprintf(err, cap, "element %s: style must be \"bar\" or \"pips\"", id); return false; }
        static const char *anchors[] = {"top_left", "top_center", "top_right", "center", "bottom_left", "bottom_center", "bottom_right"};
        bool ok = false;
        for (int a = 0; a < ARRAY_LEN(anchors); a++) ok |= !strcmp(anchors[a], el->anchor);
        if (!ok) { snprintf(err, cap, "element %s: unknown anchor \"%s\"", id, el->anchor); return false; }
    }
    return true;
}

static bool parse_theme(const Json *root, Layout *L, char *err, size_t cap) {
    const Json *c = json_get(root, "colors");
    if (!c || c->type != JSON_OBJECT) { snprintf(err, cap, "theme needs a \"colors\" object"); return false; }
    for (int i = 0; i < c->count; i++) {
        if (L->ncol >= ARRAY_LEN(L->col)) break;
        u32 v;
        if (!parse_color(json_as_str(c->items[i], ""), &v)) { snprintf(err, cap, "colour \"%s\" must look like #rrggbb or #rrggbbaa", c->keys[i]); return false; }
        copy_str(L->col[L->ncol].name, sizeof L->col[0].name, c->keys[i]);
        L->col[L->ncol++].value = v;
    }
    L->text_size = CLAMP((float)json_num(root, "text_size", 14), 6.0f, 48.0f);
    return true;
}

static Json *read_json(const char *path, char *err, size_t cap) {
    size_t n;
    u8 *text = vfs_read(path, &n, NULL);
    if (!text) { snprintf(err, cap, "%s not found", path); return NULL; }
    char perr[160];
    int line = 0;
    Json *j = json_parse((const char *)text, n, perr, sizeof perr, &line);
    free(text);
    if (!j || j->type != JSON_OBJECT) { snprintf(err, cap, "%s line %d: %s", path, line, j ? "expected an object" : perr); json_free(j); return NULL; }
    return j;
}

bool ui_load(void) {
    static Layout next; /* too large for the stack */
    memset(&next, 0, sizeof next);
    char err[240] = "";
    Json *hud = read_json("assets/dfe/ui/hud.json", err, sizeof err), *theme = NULL;
    bool ok = hud && parse_layout(hud, &next, err, sizeof err);
    if (ok) { theme = read_json("assets/dfe/ui/theme/default.json", err, sizeof err); ok = theme && parse_theme(theme, &next, err, sizeof err); }
    json_free(hud); json_free(theme);
    if (!ok) { LOGE("HUD layout not loaded, keeping the previous one: %s", err); return false; }
    for (int i = 0; i < next.n; i++) { /* overrides survive a reload */
        for (int o = 0; o < g_nover; o++) if (!strcmp(g_over[o].id, next.el[i].id)) next.el[i].override_state = g_over[o].state;
    }
    g_layout = next;
    g_loaded = true;
    return true;
}

bool ui_reload(void) { return ui_load(); }
void ui_status_shutdown(void) { memset(&g_layout, 0, sizeof g_layout); g_loaded = false; }
PlayerStatus *ui_status(void) { return &g_status; }
int ui_element_count(void) { return g_layout.n; }
const char *ui_element_id(int i) { return i >= 0 && i < g_layout.n ? g_layout.el[i].id : ""; }

void ui_status_update(void) {
    PlayerStatus *s = &g_status;
    if (!s->max_health) { /* gameplay systems that do not exist yet leave their defaults */
        s->max_hunger = 20; s->hunger = 20; s->max_armor = 20; s->xp_next = 1;
    }
    s->health = g_player.health;
    s->max_health = PLAYER_MAX_HEALTH;
}

bool ui_status_set_custom(const char *name, float value, float max) {
    for (int i = 0; i < g_status.custom_count; i++)
        if (!strcmp(g_status.custom[i].name, name)) { g_status.custom[i].value = value; g_status.custom[i].max = max; return true; }
    if (g_status.custom_count >= UI_MAX_CUSTOM || !*name) return false;
    copy_str(g_status.custom[g_status.custom_count].name, sizeof g_status.custom[0].name, name);
    g_status.custom[g_status.custom_count].value = value;
    g_status.custom[g_status.custom_count++].max = max;
    return true;
}

static bool status_value(const char *name, float *v) {
    const PlayerStatus *s = &g_status;
    static const struct { const char *n; size_t off; } F[] = {
        {"health", offsetof(PlayerStatus, health)}, {"max_health", offsetof(PlayerStatus, max_health)}, {"absorption", offsetof(PlayerStatus, absorption)},
        {"hunger", offsetof(PlayerStatus, hunger)}, {"max_hunger", offsetof(PlayerStatus, max_hunger)}, {"saturation", offsetof(PlayerStatus, saturation)},
        {"stamina", offsetof(PlayerStatus, stamina)}, {"max_stamina", offsetof(PlayerStatus, max_stamina)},
        {"magicka", offsetof(PlayerStatus, magicka)}, {"max_magicka", offsetof(PlayerStatus, max_magicka)},
        {"xp", offsetof(PlayerStatus, xp)}, {"xp_next", offsetof(PlayerStatus, xp_next)},
        {"armor", offsetof(PlayerStatus, armor)}, {"max_armor", offsetof(PlayerStatus, max_armor)}};
    for (int i = 0; i < ARRAY_LEN(F); i++) if (!strcmp(F[i].n, name)) { *v = *(const float *)((const char *)s + F[i].off); return true; }
    for (int i = 0; i < s->custom_count; i++) {
        size_t l = strlen(s->custom[i].name);
        if (!strcmp(s->custom[i].name, name)) { *v = s->custom[i].value; return true; }
        if (!strncmp(name, s->custom[i].name, l) && !strcmp(name + l, "_max")) { *v = s->custom[i].max; return true; }
    }
    return false;
}

static Element *find(const char *id) {
    for (int i = 0; i < g_layout.n; i++) if (!strcmp(g_layout.el[i].id, id)) return &g_layout.el[i];
    return NULL;
}

int ui_element_override(const char *id) { Element *e = find(id); return e ? e->override_state : -1; }

bool ui_element_set_override(const char *id, int state) {
    state = CLAMP(state, -1, 1);
    int slot = -1;
    for (int o = 0; o < g_nover; o++) if (!strcmp(g_over[o].id, id)) slot = o;
    if (slot < 0 && g_nover < UI_MAX_ELEMENTS) { slot = g_nover++; copy_str(g_over[slot].id, sizeof g_over[0].id, id); }
    if (slot >= 0) g_over[slot].state = state;
    Element *e = find(id);
    if (e) e->override_state = state;
    return e != NULL;
}

static bool element_visible(const Element *e) {
    if (e->override_state >= 0) return e->override_state == 1;
    if (!(g_creative ? e->creative : e->survival)) return false;
    float v, m;
    if (e->requires_max && (!status_value(e->max, &m) || m <= 0)) return false;
    if (e->hide_when_empty && (!status_value(e->source, &v) || v <= 0)) return false;
    return true;
}

bool ui_element_visible(const char *id) { Element *e = find(id); return e && element_visible(e); }

u32 ui_theme_color(const char *name, u32 fallback) {
    for (int i = 0; i < g_layout.ncol; i++) if (!strcmp(g_layout.col[i].name, name)) return g_layout.col[i].value;
    return fallback;
}

/* Layout units to screen pixels: the file's whole-number scale times the integer HUD scale. */
static float unit(int width, int height) { return MAX(1.0f, roundf(g_layout.scale)) * ui_hud_scale(width, height); }

static float hud_text_px(float k) { return ui_snap_text(g_layout.text_size * k * ui_hud_text_scale()); }

static void rect_of(const Element *e, int width, int height, float *x, float *y, float *w, float *h) {
    float s = unit(width, height), ax = 0, ay = 0;
    if (strstr(e->anchor, "center") && strcmp(e->anchor, "center") != 0) ax = width * 0.5f;
    if (!strcmp(e->anchor, "center")) { ax = width * 0.5f; ay = height * 0.5f; }
    if (strstr(e->anchor, "right")) ax = (float)width;
    if (!strncmp(e->anchor, "bottom", 6)) ay = (float)height;
    *w = e->w * s; *h = e->h * s;
    if (!strcmp(e->label, "level") && g_status.level > 0) {
        char t[16];
        snprintf(t, sizeof t, "%d", g_status.level);
        ui_fit(*w, *h, t, hud_text_px(s), UI_FIT_PAD * s, w, &(float){0});
    }
    *w = ui_snap(*w); *h = ui_snap(*h);
    *x = ui_snap(ax + e->x * s - (e->align_right ? *w : 0));
    *y = ui_snap(ay + e->y * s);
}

bool ui_element_rect(const char *id, int width, int height, float *x, float *y, float *w, float *h) {
    Element *e = find(id);
    if (e) rect_of(e, width, height, x, y, w, h);
    return e != NULL;
}

static void draw_element(const Element *e, int width, int height) {
    float x, y, w, h, v = 0, m = 0;
    if (!status_value(e->source, &v) || !status_value(e->max, &m) || m <= 0) return;
    rect_of(e, width, height, &x, &y, &w, &h);
    u32 col = ui_theme_color(e->color, rgba(255, 255, 255, 255)), back = ui_theme_color("bar_back", rgba(10, 10, 12, 180));
    float ratio = CLAMP(v / m, 0.0f, 1.0f);
    if (!strcmp(e->style, "bar")) {
        ui_rect(x, y, w, h, back);
        ui_rect(x, y, w * ratio, h, col);
        if (!strcmp(e->label, "level") && g_status.level > 0) {
            char t[16];
            snprintf(t, sizeof t, "%d", g_status.level);
            float ts = hud_text_px(unit(width, height));
            ui_text(x + (w - ui_text_width(ts, t)) * 0.5f, y - ts - 2 * unit(width, height), ts, ui_theme_color("text", rgba(255, 255, 255, 255)), t);
        }
        return;
    }
    float k = unit(width, height), ps = e->pip_size * k, step = ps + e->spacing * k;
    int pips = (int)ceilf(m / e->pip_value);
    u32 dark = rgba(50, 50, 56, 170), white = rgba(255, 255, 255, 255);
    for (int i = 0; i < pips; i++) {
        float px = e->align_right ? x + w - ps - (float)i * step : x + (float)i * step;
        float fill = CLAMP((v - (float)i * e->pip_value) / e->pip_value, 0.0f, 1.0f);
        if (e->icon[0]) { /* artwork: a dark empty icon with the filled part drawn over it, tinted by the theme colour */
            icons_draw(e->icon, px, y, ps, dark);
            icons_draw_part(e->icon, px, y, ps, fill, white);
        } else {
            ui_rect(px, y, ps, ps, back);
            if (fill > 0) ui_rect(px, y, ps * fill, ps, col);
        }
    }
}

void ui_status_draw(int width, int height) {
    if (!g_loaded) return;
    for (int i = 0; i < g_layout.n; i++) if (element_visible(&g_layout.el[i])) draw_element(&g_layout.el[i], width, height);
}

void ui_overrides_load(const Json *root) {
    const Json *o = json_get(root, "hud_elements");
    if (!o || o->type != JSON_OBJECT) return;
    for (int i = 0; i < o->count; i++) if (o->items[i]->type == JSON_BOOL) ui_element_set_override(o->keys[i], o->items[i]->boolean ? 1 : 0);
}

int ui_overrides_write(char *buf, size_t cap) {
    int n = 0;
    for (int i = 0; i < g_nover; i++) {
        if (g_over[i].state < 0) continue;
        n += snprintf(buf + n, cap - (size_t)n, "%s\"%s\": %s", n ? ", " : "", g_over[i].id, g_over[i].state ? "true" : "false");
        if ((size_t)n >= cap) return (int)cap - 1;
    }
    return n;
}
