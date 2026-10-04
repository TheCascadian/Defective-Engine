/* UI icon atlas. The JSON is parsed once into a fixed array and the pictures are stitched on the CPU into one
 * RGBA buffer; the GL upload happens lazily on first draw so headless runs (selftest) never need a context. */
#include "icons.h"

#include <GLFW/glfw3.h>
#include "stb_image.h"

#define ICON_MAX 256
#define ATLAS_PX 1024

typedef struct Icon { char name[32]; float uv[4]; } Icon;

typedef struct IconSet {
    Icon icon[ICON_MAX];
    int n;
    u8 *pixels; /* ATLAS_PX * ATLAS_PX RGBA */
} IconSet;

static IconSet g_set;
static GLuint g_icon_tex;
static bool g_dirty;

static int find_index(const IconSet *s, const char *name) {
    for (int i = 0; i < s->n; i++) if (!strcmp(s->icon[i].name, name)) return i;
    return -1;
}

static bool read_png(const char *path, u8 **px, int *w, int *h, char *err, size_t cap) {
    size_t n;
    u8 *file = vfs_read(path, &n, NULL);
    if (!file) { snprintf(err, cap, "%s not found", path); return false; }
    int comp;
    *px = stbi_load_from_memory(file, (int)n, w, h, &comp, 4);
    free(file);
    if (!*px) { snprintf(err, cap, "%s is not a valid image", path); return false; }
    if (*w > ATLAS_PX || *h > ATLAS_PX) { snprintf(err, cap, "%s is larger than %d px", path, ATLAS_PX); stbi_image_free(*px); *px = NULL; return false; }
    return true;
}

/* Simple shelf packer: rows fill left to right, a new row starts below the tallest picture of the last one. */
typedef struct Shelf { int x, y, row_h; } Shelf;

static bool shelf_place(Shelf *sh, int w, int h, int *ox, int *oy) {
    if (sh->x + w > ATLAS_PX) { sh->y += sh->row_h; sh->x = 0; sh->row_h = 0; }
    if (sh->y + h > ATLAS_PX) return false;
    *ox = sh->x; *oy = sh->y;
    sh->x += w;
    if (h > sh->row_h) sh->row_h = h;
    return true;
}

/* Reads one icons.json + icons.png pair from assets/<id>/ui and merges it into s. */
static bool merge_pack(IconSet *s, Shelf *sh, const char *id, bool first, char *err, size_t cap) {
    char jpath[96], ppath[96];
    snprintf(jpath, sizeof jpath, "assets/%s/ui/icons.json", id);
    snprintf(ppath, sizeof ppath, "assets/%s/ui/icons.png", id);
    size_t n;
    u8 *text = vfs_read(jpath, &n, NULL);
    if (!text) { if (first) snprintf(err, cap, "%s not found", jpath); return !first; }
    char perr[80];
    int line = 0;
    Json *j = json_parse((const char *)text, n, perr, sizeof perr, &line);
    free(text);
    if (!j || j->type != JSON_OBJECT) { snprintf(err, cap, "%s line %d: %s", jpath, line, j ? "expected an object" : perr); json_free(j); return false; }
    u8 *px = NULL;
    int w = 0, h = 0, ox = 0, oy = 0;
    const Json *icons = json_get(j, "icons");
    int cell = json_int(j, "cell", 16);
    bool ok = false;
    if (!icons || icons->type != JSON_OBJECT) snprintf(err, cap, "%s: \"icons\" must be an object", jpath);
    else if (cell < 1) snprintf(err, cap, "%s: bad cell size", jpath);
    else if (read_png(ppath, &px, &w, &h, err, cap)) {
        ok = first ? (sh->y = h, true) : shelf_place(sh, w, h, &ox, &oy);
        if (!ok) snprintf(err, cap, "atlas is full while adding %s", ppath);
    }
    for (int i = 0; ok && i < icons->count; i++) {
        const Json *v = icons->items[i];
        const char *name = icons->keys[i];
        int r[4]; /* x y w h in pixels */
        if (!name[0] || strlen(name) >= sizeof s->icon[0].name) { snprintf(err, cap, "%s: bad icon name \"%s\"", jpath, name); ok = false; break; }
        if (v->type == JSON_ARRAY && json_len(v) == 2 && json_at(v, 0)->type == JSON_NUMBER && json_at(v, 1)->type == JSON_NUMBER) {
            r[0] = (int)json_as_num(json_at(v, 0), -1) * cell; r[1] = (int)json_as_num(json_at(v, 1), -1) * cell; r[2] = r[3] = cell;
            if (json_as_num(json_at(v, 0), -1) < 0 || json_as_num(json_at(v, 1), -1) < 0) r[0] = r[1] = -1;
        } else if (v->type == JSON_OBJECT) {
            r[0] = json_int(v, "x", -1); r[1] = json_int(v, "y", -1); r[2] = json_int(v, "w", cell); r[3] = json_int(v, "h", cell);
        } else {
            snprintf(err, cap, "%s: icon \"%s\" must be [col,row] or {x,y,w,h}", jpath, name); ok = false; break;
        }
        if (r[0] < 0 || r[1] < 0 || r[2] < 1 || r[3] < 1 || r[0] + r[2] > w || r[1] + r[3] > h) {
            snprintf(err, cap, "%s: icon \"%s\" is outside the %dx%d image", jpath, name, w, h); ok = false; break;
        }
        int at = find_index(s, name);
        if (at < 0) {
            if (s->n >= ICON_MAX) { snprintf(err, cap, "%s: more than %d icons", jpath, ICON_MAX); ok = false; break; }
            at = s->n++;
            snprintf(s->icon[at].name, sizeof s->icon[at].name, "%s", name);
        }
        s->icon[at].uv[0] = (float)(ox + r[0]) / ATLAS_PX;
        s->icon[at].uv[1] = (float)(oy + r[1]) / ATLAS_PX;
        s->icon[at].uv[2] = (float)(ox + r[0] + r[2]) / ATLAS_PX;
        s->icon[at].uv[3] = (float)(oy + r[1] + r[3]) / ATLAS_PX;
    }
    if (ok) for (int y = 0; y < h; y++) memcpy(s->pixels + ((size_t)(oy + y) * ATLAS_PX + ox) * 4, px + (size_t)y * w * 4, (size_t)w * 4);
    if (px) stbi_image_free(px);
    json_free(j);
    return ok;
}

bool icons_load(void) {
    static IconSet next; /* too large for the stack */
    free(next.pixels);
    memset(&next, 0, sizeof next);
    next.pixels = calloc((size_t)ATLAS_PX * ATLAS_PX, 4);
    char err[240] = "";
    Shelf sh = {0};
    bool ok = next.pixels && merge_pack(&next, &sh, "dfe", true, err, sizeof err);
    if (ok && find_index(&next, "missing") < 0) { snprintf(err, sizeof err, "\"missing\" icon is required"); ok = false; }
    /* Roots run lowest to highest priority, so later packs overwrite earlier names. */
    for (int i = 0; ok && i < vfs_root_count(); i++) {
        const char *id = vfs_root_mod(i);
        if (id && strcmp(id, "dfe")) ok = merge_pack(&next, &sh, id, false, err, sizeof err);
    }
    if (!ok) {
        LOGE("UI icons not loaded, keeping the previous set: %s", err);
        free(next.pixels); next.pixels = NULL;
        return false;
    }
    free(g_set.pixels);
    g_set = next;
    next.pixels = NULL;
    g_dirty = true;
    return true;
}

bool icons_reload(void) { return icons_load(); }

void icons_shutdown(void) {
    free(g_set.pixels);
    memset(&g_set, 0, sizeof g_set);
    if (g_icon_tex && g_win.handle) glDeleteTextures(1, &g_icon_tex);
    g_icon_tex = 0;
    g_dirty = false;
}

bool icons_has(const char *name) { return name && find_index(&g_set, name) >= 0; }

bool icons_find(const char *name, float uv[4]) {
    int i = name ? find_index(&g_set, name) : -1;
    if (i < 0) i = find_index(&g_set, "missing");
    if (i < 0) return false;
    memcpy(uv, g_set.icon[i].uv, sizeof g_set.icon[i].uv);
    return true;
}

int icons_count(void) { return g_set.n; }

static void upload(void) {
    ui_flush(); /* queued quads still point at the texture bound before this one */
    if (!g_icon_tex) glGenTextures(1, &g_icon_tex);
    glBindTexture(GL_TEXTURE_2D, g_icon_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ATLAS_PX, ATLAS_PX, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_set.pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); /* crisp pixel art */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g_dirty = false;
}

void icons_draw(const char *name, float x, float y, float size, u32 color) {
    float uv[4];
    if (!g_win.handle || !g_set.pixels || !icons_find(name, uv)) return;
    if (g_dirty || !g_icon_tex) upload();
    ui_image(g_icon_tex, x, y, size, size, uv[0], uv[1], uv[2], uv[3], color);
}

/* Draws the left part of an icon. The cut falls on a whole icon pixel (sixteenths of the icon), so a half heart is
 * exactly half of the artwork at any integer scale. */
void icons_draw_part(const char *name, float x, float y, float size, float frac, u32 color) {
    float uv[4];
    int cut = (int)floorf(CLAMP(frac, 0.0f, 1.0f) * ICON_PART_STEPS);
    if (cut <= 0 || !g_win.handle || !g_set.pixels || !icons_find(name, uv)) return;
    if (g_dirty || !g_icon_tex) upload();
    float f = (float)cut / ICON_PART_STEPS;
    ui_image(g_icon_tex, x, y, size * f, size, uv[0], uv[1], uv[0] + (uv[2] - uv[0]) * f, uv[3], color);
}
