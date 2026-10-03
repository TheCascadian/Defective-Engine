/* Block registry, state tables, data-file loading for blocks, texture-array stitching,
 * and the per-world block name table that keeps saves independent of runtime ids. */
#include "dfe.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_TGA
/* The vendored header defines helpers for formats this build disables; silence only that, only here. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_image.h"
#pragma GCC diagnostic pop

const int DIR_VEC[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};

BlockDef *g_blocks[MAX_STATES];
int g_block_count;
u16 g_state_block[MAX_STATES + 1];
u8 g_state_flags[MAX_STATES + 1];
u8 g_state_opacity[MAX_STATES + 1];
u16 g_state_emit[MAX_STATES + 1];
TextureSet g_tex;

static StrMap g_block_map;
static int g_state_total;

/* ---------------------------------------------------------- data errors */

#define DATA_ERROR_KEEP 64
static struct { char text[DATA_ERROR_KEEP][512]; int count, total; } g_derr;

void data_error(const char *mod, const char *file, int line, const char *fmt, ...) {
    char msg[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    char full[512];
    if (line > 0) snprintf(full, sizeof full, "[mod %s] %s:%d: %s", mod && mod[0] ? mod : "?", file, line, msg);
    else snprintf(full, sizeof full, "[mod %s] %s: %s", mod && mod[0] ? mod : "?", file, msg);
    LOGE("%s", full);
    if (g_derr.count < DATA_ERROR_KEEP) memcpy(g_derr.text[g_derr.count++], full, sizeof full);
    g_derr.total++;
}
int data_error_count(void) { return g_derr.total; }
const char *data_error_text(int i) { return i >= 0 && i < g_derr.count ? g_derr.text[i] : ""; }
void data_error_reset(void) { memset(&g_derr, 0, sizeof g_derr); }

/* --------------------------------------------------------- block registry */

void registry_reset(void) {
    for (int i = 0; i < g_block_count; i++) free(g_blocks[i]);
    g_block_count = 0;
    g_state_total = 0;
    strmap_free(&g_block_map);
    memset(g_state_block, 0, sizeof g_state_block);
    memset(g_state_flags, 0, sizeof g_state_flags);
    memset(g_state_opacity, 0, sizeof g_state_opacity);
    memset(g_state_emit, 0, sizeof g_state_emit);

    /* The engine owns exactly two blocks: air, and a stand-in for ids a world saved but no mod provides. */
    BlockDef air = {0};
    snprintf(air.name, sizeof air.name, "dfe:air");
    snprintf(air.mod, sizeof air.mod, "dfe");
    air.shape = SHAPE_NONE;
    air.flags = BF_REPLACEABLE | BF_NO_ITEM;
    air.hardness = -1;
    block_register(&air);
    BlockDef missing = {0};
    snprintf(missing.name, sizeof missing.name, "dfe:missing");
    snprintf(missing.mod, sizeof missing.mod, "dfe");
    missing.shape = SHAPE_CUBE;
    missing.layer = LAYER_OPAQUE;
    missing.opacity = 15;
    missing.flags = BF_SOLID | BF_NO_ITEM;
    missing.hardness = 0.5f;
    for (int i = 0; i < 6; i++) snprintf(missing.tex_name[i], sizeof missing.tex_name[i], "dfe:missing");
    block_register(&missing);
}

static int block_state_count_of(const BlockDef *d) {
    int n = 1;
    for (int i = 0; i < d->nprops; i++) n *= MAX(d->props[i].count, 1);
    return n;
}

BlockDef *block_register(const BlockDef *def) {
    if (!strchr(def->name, ':') || strlen(def->name) < 3) {
        data_error(def->mod, def->file, 0, "block name '%s' must look like namespace:name (for example mymod:ruby_ore)", def->name);
        return NULL;
    }
    int states = block_state_count_of(def);
    if (g_state_total + states > MAX_STATES) {
        data_error(def->mod, def->file, 0, "block '%s' needs %d states but only %d remain; reduce property value counts or remove blocks", def->name, states, MAX_STATES - g_state_total);
        return NULL;
    }
    u32 existing;
    if (strmap_get(&g_block_map, def->name, &existing)) {
        /* Same name registered twice: the later definition wins, keeping the id so placed blocks stay valid. */
        BlockDef *old = g_blocks[existing];
        if (block_state_count_of(def) != old->state_count) {
            data_error(def->mod, def->file, 0, "block '%s' redefines the property layout of an earlier definition from mod '%s'; keep the same properties or use a new name", def->name, old->mod);
            return NULL;
        }
        u16 id = old->id, first = old->first_state;
        *old = *def;
        old->id = id;
        old->first_state = first;
        old->state_count = (u16)states;
        return old;
    }
    BlockDef *b = xmalloc(sizeof *b);
    *b = *def;
    b->id = (u16)g_block_count;
    b->first_state = (u16)g_state_total;
    b->state_count = (u16)states;
    b->default_state = b->first_state;
    g_state_total += states;
    g_blocks[g_block_count++] = b;
    strmap_set(&g_block_map, b->name, b->id);
    return b;
}

BlockDef *block_find(const char *name) {
    u32 id;
    return strmap_get(&g_block_map, name, &id) ? g_blocks[id] : NULL;
}

BlockDef *block_of_state(u16 state) { return state < g_state_total ? g_blocks[g_state_block[state]] : NULL; }

static int prop_find(const BlockDef *b, const char *name) {
    for (int i = 0; i < b->nprops; i++) if (!strcmp(b->props[i].name, name)) return i;
    return -1;
}

static int prop_stride(const BlockDef *b, int prop) {
    int stride = 1;
    for (int i = 0; i < prop; i++) stride *= b->props[i].count;
    return stride;
}

int block_state_prop_index(const BlockDef *b, u16 state, int prop) {
    int idx = state - b->first_state;
    return (idx / prop_stride(b, prop)) % b->props[prop].count;
}

u16 block_state_with(const BlockDef *b, u16 state, const char *prop, const char *value) {
    int p = prop_find(b, prop);
    if (p < 0) return state;
    int v = -1;
    for (int i = 0; i < b->props[p].count; i++) if (!strcmp(b->props[p].values[i], value)) v = i;
    if (v < 0) return state;
    int idx = state - b->first_state;
    int stride = prop_stride(b, p);
    int cur = (idx / stride) % b->props[p].count;
    return (u16)(b->first_state + idx + (v - cur) * stride);
}

/* Parses "ns:name" or "ns:name[prop=value,prop=value]" into a state. Unknown blocks, properties and values
 * all fail rather than being ignored, so a typo in a mod is reported instead of silently placing the wrong block. */
u16 block_parse_state(const char *spec) {
    char name[96];
    const char *open = strchr(spec, '[');
    size_t name_len = open ? (size_t)(open - spec) : strlen(spec);
    if (name_len == 0 || name_len >= sizeof name) return STATE_UNLOADED;
    memcpy(name, spec, name_len);
    name[name_len] = 0;
    const BlockDef *b = block_find(name);
    if (!b) return STATE_UNLOADED;
    u16 state = b->default_state;
    if (!open) return state;
    const char *p = open + 1;
    while (*p && *p != ']') {
        const char *eq = strchr(p, '='), *end = p + strcspn(p, ",]");
        if (!eq || eq > end) return STATE_UNLOADED;
        char prop[sizeof b->props[0].name], value[sizeof b->props[0].values[0]];
        if ((size_t)(eq - p) >= sizeof prop || (size_t)(end - eq - 1) >= sizeof value) return STATE_UNLOADED;
        memcpy(prop, p, (size_t)(eq - p)); prop[eq - p] = 0;
        memcpy(value, eq + 1, (size_t)(end - eq - 1)); value[end - eq - 1] = 0;
        int pi = prop_find(b, prop);
        if (pi < 0) return STATE_UNLOADED;
        bool known = false;
        for (int i = 0; i < b->props[pi].count; i++) known |= !strcmp(b->props[pi].values[i], value);
        if (!known) return STATE_UNLOADED;
        state = block_state_with(b, state, prop, value);
        p = *end == ',' ? end + 1 : end;
    }
    return *p == ']' ? state : STATE_UNLOADED;
}

/* Writes the canonical text of a state, "ns:name" or "ns:name[prop=value,...]". Returns false for an invalid state. */
bool block_format_state(u16 state, char *out, size_t cap) {
    const BlockDef *b = block_of_state(state);
    if (!b || cap == 0) return false;
    size_t n = (size_t)snprintf(out, cap, "%s", b->name);
    for (int i = 0; i < b->nprops && n < cap; i++)
        n += (size_t)snprintf(out + n, cap - n, "%c%s=%s", i == 0 ? '[' : ',', b->props[i].name,
                              b->props[i].values[block_state_prop_index(b, state, i)]);
    if (b->nprops > 0 && n < cap) snprintf(out + n, cap - n, "]");
    return true;
}

void registry_freeze_blocks(void) {
    for (int i = 0; i < g_block_count; i++) {
        BlockDef *b = g_blocks[i];
        if (b->shape == SHAPE_CUBE && b->layer == LAYER_OPAQUE) b->flags |= BF_OPAQUE;
        else b->flags &= (u8)~BF_OPAQUE;
        if (b->flags & BF_OPAQUE) b->opacity = 15;
        for (int s = 0; s < b->state_count; s++) {
            u16 st = (u16)(b->first_state + s);
            g_state_block[st] = b->id;
            g_state_flags[st] = b->flags;
            g_state_opacity[st] = b->opacity;
            bool gated = b->emit_prop_name[0] && b->emit_prop >= 0;
            bool emits = !gated || !strcmp(b->props[b->emit_prop].values[block_state_prop_index(b, st, b->emit_prop)], b->emit_value);
            g_state_emit[st] = emits ? (u16)((b->emit[0] << 8) | (b->emit[1] << 4) | b->emit[2]) : 0;
        }
    }
}

/* ---------------------------------------------------------- data loading */

static const char *const FACE_KEYS[6] = {"east", "west", "up", "down", "south", "north"};

static void parse_textures(BlockDef *b, const Json *tex, const char *file, int line) {
    if (!tex || tex->type != JSON_OBJECT) return;
    const char *all = json_str(tex, "all", NULL), *side = json_str(tex, "side", NULL);
    for (int f = 0; f < 6; f++) {
        const char *v = json_str(tex, FACE_KEYS[f], NULL);
        if (!v && (f == DIR_PY || f == DIR_NY)) v = json_str(tex, f == DIR_PY ? "top" : "bottom", NULL);
        if (!v && f != DIR_PY && f != DIR_NY) v = side;
        if (!v) v = all;
        if (!v) v = side ? side : json_str(tex, "top", NULL);
        if (!v) {
            data_error(b->mod, file, line, "block '%s' has a textures object but no texture for face '%s'; add \"all\": \"ns:path\" or the missing face key", b->name, FACE_KEYS[f]);
            v = "dfe:missing";
        }
        snprintf(b->tex_name[f], sizeof b->tex_name[f], "%s", v);
    }
}

static int enum_index(const char *v, const char *const *names, int n, int def) {
    if (!v) return def;
    for (int i = 0; i < n; i++) if (!strcmp(v, names[i])) return i;
    return -1;
}

static bool parse_block_file(const char *ns, const char *stem, const char *rel, const char *mod, const Json *root) {
    BlockDef d = {0};
    if (strlen(ns) + strlen(stem) + 2 > sizeof d.name || strlen(rel) + 1 > sizeof d.file) {
        data_error(mod, rel, 1, "the block id '%s:%s' or its path is too long (ids are limited to %d characters, paths to %d). "
                   "Shorten the folder or file name.", ns, stem, (int)sizeof d.name - 1, (int)sizeof d.file - 1);
        return false;
    }
    /* The length check above guarantees this fits, so the pieces are copied directly. */
    size_t ns_len = strlen(ns);
    memcpy(d.name, ns, ns_len);
    d.name[ns_len] = ':';
    memcpy(d.name + ns_len + 1, stem, strlen(stem) + 1);
    snprintf(d.mod, sizeof d.mod, "%.*s", (int)sizeof d.mod - 1, mod); /* mod ids are validated shorter, truncation is only a safety net */
    snprintf(d.file, sizeof d.file, "%s", rel);
    static const char *const shapes[] = {"none", "cube", "cross", "fluid", "model"};
    static const char *const layers[] = {"opaque", "cutout", "translucent"};
    static const char *const tints[] = {"none", "grass", "foliage", "water"};
    int shape = enum_index(json_str(root, "shape", "cube"), shapes, 5, -1);
    int layer = enum_index(json_str(root, "layer", "opaque"), layers, 3, -1);
    int tint = enum_index(json_str(root, "tint", "none"), tints, 4, -1);
    const Json *jshape = json_get(root, "shape");
    if (shape < 0) { data_error(mod, rel, jshape ? jshape->line : 1, "unknown shape '%s'; use one of none, cube, cross, fluid, model", json_str(root, "shape", "")); return false; }
    if (layer < 0) { data_error(mod, rel, json_get(root, "layer")->line, "unknown layer '%s'; use opaque, cutout or translucent", json_str(root, "layer", "")); return false; }
    if (tint < 0) { data_error(mod, rel, json_get(root, "tint")->line, "unknown tint '%s'; use none, grass, foliage or water", json_str(root, "tint", "")); return false; }
    d.shape = (u8)shape;
    d.layer = (u8)layer;
    d.tint = (u8)tint;
    d.tint_mask = 0x3F;
    const Json *tf = json_get(root, "tint_faces");
    if (tf && tf->type == JSON_ARRAY) {
        d.tint_mask = 0;
        for (int i = 0; i < tf->count; i++) {
            int f = enum_index(json_as_str(tf->items[i], ""), FACE_KEYS, 6, -1);
            if (f < 0) data_error(mod, rel, tf->items[i]->line, "unknown face '%s' in tint_faces; use east, west, up, down, south or north", json_as_str(tf->items[i], ""));
            else d.tint_mask |= (u8)(1u << f);
        }
    }
    bool solid_default = shape == SHAPE_CUBE || shape == SHAPE_MODEL;
    if (json_bool(root, "solid", solid_default)) d.flags |= BF_SOLID;
    if (json_bool(root, "replaceable", shape == SHAPE_CROSS || shape == SHAPE_FLUID)) d.flags |= BF_REPLACEABLE;
    if (json_bool(root, "wind", false)) d.flags |= BF_WIND;
    if (json_bool(root, "random_tick", false)) d.flags |= BF_RANDOM_TICK;
    if (json_bool(root, "climbable", false)) d.flags |= BF_CLIMBABLE;
    if (!json_bool(root, "item", true)) d.flags |= BF_NO_ITEM;
    const Json *light = json_get(root, "light");
    d.opacity = (u8)CLAMP(json_int(light, "opacity", (shape == SHAPE_CUBE && layer == LAYER_OPAQUE) ? 15 : (shape == SHAPE_FLUID ? 2 : 0)), 0, 15);
    const Json *emit = json_get(light, "emit");
    for (int i = 0; i < 3; i++) d.emit[i] = (u8)CLAMP((int)json_as_num(json_at(emit, i), 0), 0, 15);
    d.emit_prop = -1;
    const Json *emit_when = json_get(light, "emit_when");
    if (emit_when && (emit_when->type != JSON_OBJECT || emit_when->count != 1)) {
        data_error(mod, rel, emit_when->line, "\"emit_when\" must hold exactly one property, such as {\"lit\": \"on\"}");
        emit_when = NULL;
    }
    if (emit_when) { /* the block emits only in states where that property has that value; resolved once properties are parsed */
        snprintf(d.emit_prop_name, sizeof d.emit_prop_name, "%s", emit_when->keys[0]);
        snprintf(d.emit_value, sizeof d.emit_value, "%s", json_as_str(emit_when->items[0], ""));
    }
    d.hardness = (float)json_num(root, "hardness", 1.0);
    snprintf(d.tool, sizeof d.tool, "%s", json_str(root, "tool", ""));
    snprintf(d.drop, sizeof d.drop, "%s", json_str(root, "drops", d.name));
    snprintf(d.sound, sizeof d.sound, "%s", json_str(root, "sound", "stone"));
    snprintf(d.model, sizeof d.model, "%s", json_str(root, "model", ""));
    d.friction = (float)json_num(root, "friction", 1.0);
    const Json *fluid = json_get(root, "fluid");
    if (fluid) {
        d.flags |= BF_FLUID;
        d.fluid_viscosity = MAX(json_int(fluid, "viscosity", 5), 1);
        d.fluid_group = (int)(hash_str(json_str(fluid, "group", d.name)) & 0x7FFFFFFF);
        d.fluid_reach = CLAMP(json_int(fluid, "reach", FLUID_DEFAULT_REACH), 1, MAX_PROP_VALUES - 2);
        d.fluid_infinite = json_bool(fluid, "infinite", false);
    }
    const Json *props = json_get(root, "properties");
    if (props && props->type == JSON_OBJECT) {
        for (int i = 0; i < props->count; i++) {
            if (d.nprops >= MAX_BLOCK_PROPS) {
                data_error(mod, rel, props->items[i]->line, "block '%s' has more than %d properties; merge or remove some", d.name, MAX_BLOCK_PROPS);
                break;
            }
            PropDef *p = &d.props[d.nprops];
            snprintf(p->name, sizeof p->name, "%s", props->keys[i]);
            const Json *vals = props->items[i];
            if (vals->type != JSON_ARRAY || vals->count < 1 || vals->count > MAX_PROP_VALUES) {
                data_error(mod, rel, vals->line, "property '%s' must be an array of 1 to %d string values", props->keys[i], MAX_PROP_VALUES);
                return false;
            }
            p->count = vals->count;
            for (int k = 0; k < vals->count; k++) snprintf(p->values[k], sizeof p->values[k], "%s", json_as_str(vals->items[k], "?"));
            d.nprops++;
        }
    }
    d.fluid_level_prop = -1;
    if (fluid) { /* the simulation reads and writes the "level" property: 0 source, 1..reach flowing, reach + 1 falling */
        int pi = prop_find(&d, "level");
        if (pi < 0 || d.props[pi].count != d.fluid_reach + 2)
            data_error(mod, rel, fluid->line, "fluid block '%s' needs a \"level\" property with %d values (0 is a source, 1 to %d are flowing, %d is falling). Add \"properties\": {\"level\": [\"0\", ..., \"%d\"]} or change \"reach\" under \"fluid\"",
                       d.name, d.fluid_reach + 2, d.fluid_reach, d.fluid_reach + 1, d.fluid_reach + 1);
        else d.fluid_level_prop = pi;
    }
    if (d.emit_prop_name[0]) {
        int pi = prop_find(&d, d.emit_prop_name), vi = -1;
        for (int i = 0; pi >= 0 && i < d.props[pi].count; i++) if (!strcmp(d.props[pi].values[i], d.emit_value)) vi = i;
        if (pi < 0 || vi < 0) data_error(mod, rel, json_get(light, "emit_when")->line, "\"emit_when\" names %s '%s', which block '%s' does not declare under \"properties\"; declare the property with that value",
                                         pi < 0 ? "property" : "value", pi < 0 ? d.emit_prop_name : d.emit_value, d.name);
        else d.emit_prop = pi;
    }
    parse_textures(&d, json_get(root, "textures"), rel, json_get(root, "textures") ? json_get(root, "textures")->line : 1);
    if (d.shape != SHAPE_NONE && d.shape != SHAPE_MODEL && !d.tex_name[0][0]) {
        data_error(mod, rel, 1, "block '%s' has no \"textures\" entry; add {\"textures\": {\"all\": \"%s:block/%s\"}}", d.name, ns, stem);
        for (int i = 0; i < 6; i++) snprintf(d.tex_name[i], sizeof d.tex_name[i], "dfe:missing");
    }
    BlockDef *b = block_register(&d);
    if (!b) return false;
    /* Default state: first value of every property unless "defaults" overrides it. */
    const Json *defs = json_get(root, "defaults");
    u16 st = b->first_state;
    for (int i = 0; defs && i < defs->count; i++) {
        u16 next = block_state_with(b, st, defs->keys[i], json_as_str(defs->items[i], ""));
        if (next == st && prop_find(b, defs->keys[i]) < 0)
            data_error(mod, rel, defs->items[i]->line, "defaults names property '%s' which block '%s' does not declare under \"properties\"", defs->keys[i], b->name);
        st = next;
    }
    b->default_state = st;
    return true;
}

int registry_load_blocks(void) {
    int errors_before = data_error_count();
    StrList namespaces = {0};
    vfs_list("data", &namespaces);
    for (int n = 0; n < namespaces.n; n++) {
        char dir[160];
        snprintf(dir, sizeof dir, "data/%s/blocks", namespaces.d[n]);
        StrList files = {0};
        vfs_list(dir, &files);
        for (int f = 0; f < files.n; f++) {
            size_t len = strlen(files.d[f]);
            if (len < 6 || strcmp(files.d[f] + len - 5, ".json")) continue;
            char rel[260], stem[96];
            snprintf(rel, sizeof rel, "%s/%s", dir, files.d[f]);
            snprintf(stem, sizeof stem, "%.*s", (int)(len - 5), files.d[f]);
            size_t size;
            const char *owner = "?";
            u8 *text = vfs_read(rel, &size, &owner);
            if (!text) continue;
            char err[200];
            int err_line = 0;
            Json *root = json_parse((const char *)text, size, err, sizeof err, &err_line);
            free(text);
            if (!root) { data_error(owner, rel, err_line, "%s. Fix the JSON syntax at that line.", err); continue; }
            if (root->type != JSON_OBJECT) data_error(owner, rel, 1, "a block file must contain one JSON object like {\"shape\": \"cube\", ...}");
            else parse_block_file(namespaces.d[n], stem, rel, owner, root);
            json_free(root);
        }
        strlist_free(&files);
    }
    strlist_free(&namespaces);
    registry_freeze_blocks();
    return data_error_count() - errors_before;
}

/* -------------------------------------------------------- texture stitch */

typedef struct TexSource {
    char name[64];
    u8 *rgba;   /* frames stacked vertically, w x (w*frames) */
    int w, frames;
    float fps;
    bool has_alpha;
} TexSource;

static u8 *make_missing_tile(int size) {
    u8 *px = xmalloc((size_t)size * size * 4);
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            bool a = ((x * 2 / size) + (y * 2 / size)) & 1;
            u8 *p = px + ((size_t)y * size + x) * 4;
            p[0] = a ? 255 : 20; p[1] = a ? 0 : 20; p[2] = a ? 255 : 20; p[3] = 255;
        }
    return px;
}

static bool texture_path(const char *res, char *out, size_t cap) {
    const char *colon = strchr(res, ':');
    if (!colon) return false;
    snprintf(out, cap, "assets/%.*s/textures/%s.png", (int)(colon - res), res, colon + 1);
    return true;
}

static bool load_texture_source(TexSource *t, const char *res, const char *owner_mod, const char *owner_file) {
    char path[200];
    if (!texture_path(res, path, sizeof path)) {
        data_error(owner_mod, owner_file, 0, "texture id '%s' must look like namespace:folder/name", res);
        return false;
    }
    size_t size;
    const char *mod = "?";
    u8 *file = vfs_read(path, &size, &mod);
    if (!file) {
        data_error(owner_mod, owner_file, 0, "texture '%s' not found; expected the file %s inside a mod. Add the PNG there or fix the name", res, path);
        return false;
    }
    int w, h, comp;
    u8 *px = stbi_load_from_memory(file, (int)size, &w, &h, &comp, 4);
    free(file);
    if (!px) {
        data_error(mod, path, 0, "cannot decode PNG: %s. Re-export it as a standard 8-bit RGBA PNG", stbi_failure_reason());
        return false;
    }
    if (h % w != 0) {
        data_error(mod, path, 0, "texture is %dx%d; height must equal width or be a whole multiple of it for animation strips", w, h);
        stbi_image_free(px);
        return false;
    }
    snprintf(t->name, sizeof t->name, "%s", res);
    t->rgba = px;
    t->w = w;
    t->frames = h / w;
    t->fps = 4.0f;
    /* Only texels below the alpha-test threshold make a texture a cutout. A uniformly translucent one such as
     * water must not be treated as one: the coverage rescale would then drag its alpha toward the threshold
     * on every mip level and the sea would turn half transparent and dark. */
    t->has_alpha = false;
    for (int i = 0; i < w * h; i++) if (px[i * 4 + 3] < 128) { t->has_alpha = true; break; }
    char jpath[200];
    snprintf(jpath, sizeof jpath, "%.*s.json", (int)(strlen(path) - 4), path);
    size_t jsize;
    u8 *jtext = vfs_read(jpath, &jsize, NULL);
    if (jtext) {
        char err[120];
        int line;
        Json *j = json_parse((const char *)jtext, jsize, err, sizeof err, &line);
        if (j) { t->fps = (float)json_num(j, "fps", 4.0); json_free(j); }
        else data_error(mod, jpath, line, "%s", err);
        free(jtext);
    }
    return true;
}

/* Nearest-neighbour resize keeps pixel-art edges crisp when mixed resolutions share one array. */
static void blit_tile(u8 *dst, int dsize, const u8 *src, int ssize) {
    for (int y = 0; y < dsize; y++)
        for (int x = 0; x < dsize; x++)
            memcpy(dst + ((size_t)y * dsize + x) * 4, src + ((size_t)(y * ssize / dsize) * ssize + (x * ssize / dsize)) * 4, 4);
}

/* Colour of fully transparent texels is meaningless but gets averaged into mips, producing dark fringes on leaves. */
static void bleed_transparent_color(u8 *tile, int size) {
    u64 r = 0, g = 0, b = 0, n = 0;
    for (int i = 0; i < size * size; i++) if (tile[i * 4 + 3] > 127) { r += tile[i * 4]; g += tile[i * 4 + 1]; b += tile[i * 4 + 2]; n++; }
    if (!n) return;
    for (int i = 0; i < size * size; i++)
        if (tile[i * 4 + 3] == 0) { tile[i * 4] = (u8)(r / n); tile[i * 4 + 1] = (u8)(g / n); tile[i * 4 + 2] = (u8)(b / n); }
}

static float alpha_coverage(const u8 *tile, int size, float scale) {
    int hit = 0;
    for (int i = 0; i < size * size; i++) if (tile[i * 4 + 3] * scale >= 127.5f) hit++;
    return (float)hit / (float)(size * size);
}

/* Builds the next mip level in place. For cutout tiles alpha is rescaled so the fraction of texels
 * passing the alpha test stays constant, otherwise foliage thins out with distance. */
static void downsample_tile(const u8 *src, int ssize, u8 *dst, bool cutout, float target_coverage) {
    int dsize = ssize / 2;
    for (int y = 0; y < dsize; y++)
        for (int x = 0; x < dsize; x++)
            for (int c = 0; c < 4; c++) {
                int sum = src[(((size_t)y * 2) * ssize + x * 2) * 4 + c] + src[(((size_t)y * 2) * ssize + x * 2 + 1) * 4 + c] +
                          src[(((size_t)y * 2 + 1) * ssize + x * 2) * 4 + c] + src[(((size_t)y * 2 + 1) * ssize + x * 2 + 1) * 4 + c];
                dst[((size_t)y * dsize + x) * 4 + c] = (u8)((sum + 2) / 4);
            }
    if (!cutout || dsize < 1) return;
    float lo = 0.5f, hi = 4.0f, scale = 1.0f;
    for (int it = 0; it < 12; it++) {
        scale = (lo + hi) * 0.5f;
        if (alpha_coverage(dst, dsize, scale) < target_coverage) lo = scale; else hi = scale;
    }
    for (int i = 0; i < dsize * dsize; i++) dst[i * 4 + 3] = (u8)MIN(255.0f, dst[i * 4 + 3] * scale + 0.5f);
}

bool textures_build(void) {
    textures_destroy();
    VEC(TexSource) sources = {0};
    StrMap seen;
    strmap_init(&seen);
    TexSource missing_src = {0};
    snprintf(missing_src.name, sizeof missing_src.name, "dfe:missing");
    vec_push(sources, missing_src); /* layer 0 */
    strmap_set(&seen, "dfe:missing", 0);

    for (int i = 0; i < g_block_count; i++) {
        BlockDef *b = g_blocks[i];
        if (b->shape == SHAPE_NONE) continue;
        for (int f = 0; f < 6; f++) {
            const char *name = b->tex_name[f];
            if (!name[0] || strmap_get(&seen, name, NULL)) continue;
            TexSource t = {0};
            if (load_texture_source(&t, name, b->mod, b->file)) {
                strmap_set(&seen, name, (u32)sources.n);
                vec_push(sources, t);
            } else strmap_set(&seen, name, 0xFFFFFFFFu); /* remember the failure so it is reported once */
        }
    }

    int tile = 16;
    for (int i = 1; i < sources.n; i++) tile = MAX(tile, sources.d[i].w);
    if (tile > 128) { LOGW("Largest block texture is %d px; capping the array tile size at 128.", tile); tile = 128; }
    int layers = 1;
    for (int i = 1; i < sources.n; i++) layers += sources.d[i].frames;
    if (layers > MAX_TEXTURE_LAYERS) {
        data_error("?", "textures", 0, "%d texture layers requested but the limit is %d; remove unused block textures or animation frames", layers, MAX_TEXTURE_LAYERS);
        layers = MAX_TEXTURE_LAYERS;
    }
    int levels = 1;
    for (int s = tile; s > 1; s >>= 1) levels++;

    size_t tile_bytes = (size_t)tile * tile * 4;
    u8 *base = xcalloc((size_t)layers, tile_bytes);
    bool *cutout = xcalloc((size_t)layers, sizeof(bool));
    u8 *anim = xcalloc((size_t)layers * 2, 1);
    u8 *miss = make_missing_tile(tile);
    memcpy(base, miss, tile_bytes);
    free(miss);
    anim[0] = 1;

    strmap_init(&g_tex.name_to_layer);
    strmap_set(&g_tex.name_to_layer, "dfe:missing", 0);
    int layer = 1;
    for (int i = 1; i < sources.n && layer < layers; i++) {
        TexSource *t = &sources.d[i];
        strmap_set(&g_tex.name_to_layer, t->name, (u32)layer);
        for (int fr = 0; fr < t->frames && layer < layers; fr++, layer++) {
            blit_tile(base + (size_t)layer * tile_bytes, tile, t->rgba + (size_t)fr * t->w * t->w * 4, t->w);
            cutout[layer] = t->has_alpha;
            if (t->has_alpha) bleed_transparent_color(base + (size_t)layer * tile_bytes, tile);
            anim[layer * 2] = (u8)MIN(t->frames, 255);
            anim[layer * 2 + 1] = (u8)CLAMP((int)(t->fps * 4.0f), 1, 255);
        }
        free(t->rgba);
    }

    for (int i = 0; i < g_block_count; i++) {
        BlockDef *b = g_blocks[i];
        for (int f = 0; f < 6; f++) {
            u32 l = 0;
            if (b->tex_name[f][0] && strmap_get(&g_tex.name_to_layer, b->tex_name[f], &l)) b->tex[f] = (u16)l;
            else b->tex[f] = 0;
        }
    }

    glGenTextures(1, &g_tex.gl_array);
    glBindTexture(GL_TEXTURE_2D_ARRAY, g_tex.gl_array);
    u8 *level_buf = xmalloc((size_t)layers * tile_bytes);
    u8 *prev = base;
    int size = tile;
    u8 *owned_prev = NULL;
    for (int lv = 0; lv < levels; lv++) {
        glTexImage3D(GL_TEXTURE_2D_ARRAY, lv, GL_RGBA8, size, size, layers, 0, GL_RGBA, GL_UNSIGNED_BYTE, prev);
        if (lv + 1 == levels) break;
        int nsize = size / 2;
        for (int l = 0; l < layers; l++) {
            float cov = cutout[l] ? alpha_coverage(base + (size_t)l * tile_bytes, tile, 1.0f) : 0;
            downsample_tile(prev + (size_t)l * size * size * 4, size, level_buf + (size_t)l * nsize * nsize * 4, cutout[l], cov);
        }
        u8 *next = xmalloc((size_t)layers * nsize * nsize * 4);
        memcpy(next, level_buf, (size_t)layers * nsize * nsize * 4);
        free(owned_prev);
        owned_prev = next;
        prev = next;
        size = nsize;
    }
    free(owned_prev);
    free(level_buf);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, levels - 1);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    if (GLAD_GL_EXT_texture_filter_anisotropic) glTexParameterf(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_ANISOTROPY_EXT, 4.0f);

    glGenTextures(1, &g_tex.gl_anim);
    glBindTexture(GL_TEXTURE_2D, g_tex.gl_anim);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, layers, 1, 0, GL_RG, GL_UNSIGNED_BYTE, anim);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    g_tex.tile_size = tile;
    g_tex.layer_count = layers;
    LOGI("Texture array: %d layers of %dx%d, %d mip levels, %.1f MB", layers, tile, tile, levels, layers * tile_bytes * 1.34 / 1048576.0);
    g_tex.pixels = base; /* kept for the item icons, a few kilobytes per layer */
    free(cutout);
    free(anim);
    for (int i = 0; i < sources.n; i++) (void)0;
    vec_free(sources);
    strmap_free(&seen);
    return true;
}

u16 texture_layer_lookup(const char *res_id) {
    u32 l = 0;
    return strmap_get(&g_tex.name_to_layer, res_id, &l) ? (u16)l : 0;
}

void textures_destroy(void) {
    if (g_tex.gl_array) glDeleteTextures(1, &g_tex.gl_array);
    if (g_tex.gl_anim) glDeleteTextures(1, &g_tex.gl_anim);
    free(g_tex.pixels);
    strmap_free(&g_tex.name_to_layer);
    memset(&g_tex, 0, sizeof g_tex);
}

/* ------------------------------------------------------- world name table */

void block_table_save_names(BlockNameTable *t) {
    t->names.d = NULL;
    t->names.n = t->names.cap = 0;
    for (int i = 0; i < g_block_count; i++) vec_push(t->names, xstrdup(g_blocks[i]->name));
}

u16 block_table_remap_state(const BlockNameTable *saved, u32 block_index, u32 state_index) {
    if (block_index >= (u32)saved->names.n) return STATE_MISSING;
    const BlockDef *b = block_find(saved->names.d[block_index]);
    if (!b) return STATE_MISSING;
    /* A property layout that shrank since the save clamps to the default instead of reading a neighbour's state. */
    return state_index < b->state_count ? (u16)(b->first_state + state_index) : b->default_state;
}

void block_table_free(BlockNameTable *t) {
    for (int i = 0; i < t->names.n; i++) free(t->names.d[i]);
    vec_free(t->names);
}
