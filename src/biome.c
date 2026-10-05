#include "biome.h"
#include <stdarg.h>
#include <string.h>

static BiomeDef g_b[BIOME_MAX];
static int g_n;

static const char *const LEGACY_IDS[BIOME_LEGACY] = {"base:ocean", "base:beach", "base:desert", "base:tundra", "base:swamp", "base:forest", "base:plains", "base:mountain"};

void biome_reset(void) { g_n = 0; memset(g_b, 0, sizeof g_b); }
int biome_count(void) { return g_n; }
const BiomeDef *biome_get(int i) { return i >= 0 && i < g_n ? &g_b[i] : NULL; }
int biome_find(const char *id) {
    for (int i = 0; i < g_n; i++) if (!strcmp(g_b[i].id, id)) return i;
    return -1;
}
bool biome_has_tag(const BiomeDef *b, const char *tag) {
    for (int i = 0; i < b->tag_count; i++) if (!strcmp(b->tags[i], tag)) return true;
    return false;
}

typedef struct Ctx { const char *owner, *rel; bool ok; } Ctx;

static void fail(Ctx *c, const Json *at, const char *fmt, ...) {
    char msg[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    data_error(c->owner, c->rel, at ? at->line : 1, "%s", msg);
    c->ok = false;
}

static void check_keys(Ctx *c, const Json *obj, const char *const *allowed, const char *where) {
    for (int i = 0; i < obj->count; i++) {
        bool known = false;
        for (int k = 0; allowed[k] && !known; k++) known = !strcmp(obj->keys[i], allowed[k]);
        if (!known) fail(c, obj->items[i], "unknown field \"%s\" in %s.", obj->keys[i], where);
    }
}

static bool num_in(Ctx *c, const Json *o, const char *key, float *out, float lo, float hi) {
    const Json *v = json_get(o, key);
    if (!v) return false;
    if (v->type != JSON_NUMBER || v->num < lo || v->num > hi) { fail(c, v, "\"%s\" must be a number between %g and %g.", key, lo, hi); return false; }
    *out = (float)v->num;
    return true;
}

static bool pair_in(Ctx *c, const Json *o, const char *key, float *a, float *b, float lo, float hi) {
    const Json *v = json_get(o, key);
    if (!v) return false;
    if (v->type != JSON_ARRAY || v->count != 2 || v->items[0]->type != JSON_NUMBER || v->items[1]->type != JSON_NUMBER) { fail(c, v, "\"%s\" must be [min, max].", key); return false; }
    float x = (float)v->items[0]->num, y = (float)v->items[1]->num;
    if (x > y || x < lo || y > hi) { fail(c, v, "\"%s\" must be [min, max] with min <= max, within %g to %g.", key, lo, hi); return false; }
    *a = x; *b = y;
    return true;
}

static u16 block_state(Ctx *c, const Json *at, const char *name) {
    BlockDef *b = name ? block_find(name) : NULL;
    if (!b) { fail(c, at, "unknown block \"%s\".", name ? name : "<missing>"); return STATE_MISSING; }
    return b->default_state;
}

static int noise_id(Ctx *c, const Json *at, const char *name) {
    if (name && !strcmp(name, "a")) return 0;
    if (name && !strcmp(name, "b")) return 1;
    if (name && !strcmp(name, "detail")) return 2;
    fail(c, at, "noise must be \"a\" (broad patches), \"b\" (fine speckle) or \"detail\".");
    return 0;
}

static void cond_default(BiomeCond *w) {
    memset(w, 0, sizeof *w);
    w->slope_min = -1e9f; w->river_min = -1e9f; w->slope_max = 1e9f; w->water_dist_max = 1e9f; w->height_min = -1e9f; w->height_max = 1e9f;
}

static void parse_patch(Ctx *c, const Json *p, BiomeCond *w) {
    if (p->type != JSON_OBJECT) { fail(c, p, "\"patch\" entries must be {\"noise\":\"a\"|\"b\",\"lo\":n,\"hi\":n}."); return; }
    static const char *const keys[] = {"noise", "lo", "hi", NULL};
    check_keys(c, p, keys, "a patch condition");
    if (w->patch_count >= 3) { fail(c, p, "at most three patch conditions per \"when\"."); return; }
    int i = w->patch_count++;
    w->patch[i].noise = (u8)noise_id(c, p, json_str(p, "noise", NULL));
    w->patch[i].lo = (float)json_num(p, "lo", -2.0);
    w->patch[i].hi = (float)json_num(p, "hi", 2.0);
}

static void parse_when(Ctx *c, const Json *when, BiomeCond *w) {
    cond_default(w);
    if (!when) return;
    if (when->type != JSON_OBJECT) { fail(c, when, "\"when\" must be an object."); return; }
    static const char *const keys[] = {"slope_min", "slope_max", "river_min", "water_dist_max", "height_min", "height_max", "near_river", "above_snowline", "above_treeline", "patch", "stripe", NULL};
    check_keys(c, when, keys, "\"when\"");
    num_in(c, when, "slope_min", &w->slope_min, -1000, 1000);
    num_in(c, when, "slope_max", &w->slope_max, -1000, 1000);
    num_in(c, when, "river_min", &w->river_min, -1, 2);
    num_in(c, when, "water_dist_max", &w->water_dist_max, 0, 100000);
    num_in(c, when, "height_min", &w->height_min, -4096, 4096);
    num_in(c, when, "height_max", &w->height_max, -4096, 4096);
    if (json_bool(when, "near_river", false) && w->river_min < 0.4f) w->river_min = 0.4f;
    w->above_snowline = json_bool(when, "above_snowline", false);
    w->above_treeline = json_bool(when, "above_treeline", false);
    const Json *p = json_get(when, "patch");
    if (p && p->type == JSON_ARRAY) for (int i = 0; i < p->count; i++) parse_patch(c, p->items[i], w);
    else if (p) parse_patch(c, p, w);
    const Json *s = json_get(when, "stripe");
    if (s) {
        if (s->type != JSON_OBJECT) { fail(c, s, "\"stripe\" must be {\"period\":n,\"lo\":n,\"hi\":n}."); return; }
        static const char *const sk[] = {"period", "lo", "hi", NULL};
        check_keys(c, s, sk, "\"stripe\"");
        w->stripe = true;
        w->stripe_period = json_int(s, "period", 8);
        w->stripe_lo = json_int(s, "lo", 0);
        w->stripe_hi = json_int(s, "hi", 1);
        if (w->stripe_period < 2 || w->stripe_period > 256 || w->stripe_lo < 0 || w->stripe_hi > w->stripe_period || w->stripe_lo >= w->stripe_hi)
            fail(c, s, "\"stripe\" needs 2 <= period <= 256 and 0 <= lo < hi <= period.");
    }
}

static void parse_layers(Ctx *c, const Json *arr, BiomeDef *b) {
    if (!arr || arr->type != JSON_ARRAY || arr->count == 0) { fail(c, arr, "\"surface\" must be a non-empty array of layers."); return; }
    int last_depth = 0;
    bool top_open = false;
    for (int i = 0; i < arr->count; i++) {
        const Json *l = arr->items[i];
        if (l->type != JSON_OBJECT) { fail(c, l, "each surface layer must be an object."); continue; }
        static const char *const keys[] = {"block", "depth", "when", NULL};
        check_keys(c, l, keys, "a surface layer");
        if (b->layer_count >= BIOME_MAX_LAYERS) { fail(c, l, "too many surface layers; the limit is %d.", BIOME_MAX_LAYERS); break; }
        BiomeLayer *ly = &b->layers[b->layer_count];
        ly->state = block_state(c, l, json_str(l, "block", NULL));
        if (ly->state == STATE_AIR) fail(c, l, "a surface layer cannot be air.");
        ly->depth = json_int(l, "depth", 0);
        if (ly->depth < 0 || ly->depth > 16) fail(c, l, "\"depth\" must be 0 to 16.");
        parse_when(c, json_get(l, "when"), &ly->when);
        if (ly->depth == 0) {
            if (b->layer_count && b->layers[b->layer_count - 1].depth > 0) fail(c, l, "top layers (depth 0) must come before subsurface layers.");
            top_open = !json_get(l, "when");
        } else {
            if (ly->depth < last_depth) fail(c, l, "subsurface layer depths must not decrease.");
            last_depth = ly->depth;
            if (ly->depth > b->subsurface_depth) b->subsurface_depth = ly->depth;
        }
        b->layer_count++;
    }
    if (!top_open) fail(c, arr, "the last top layer (depth 0) must have no \"when\", so every column gets a surface block.");
}

static int habitat_id(Ctx *c, const Json *at, const char *name) {
    static const char *const names[] = {"dry", "any", "river", "lake", "shore", "wetland"};
    for (int i = 0; i < 6; i++) if (name && !strcmp(name, names[i])) return i;
    fail(c, at, "\"habitat\" must be dry, any, river, lake, shore or wetland.");
    return BIOME_HAB_DRY;
}

static void parse_plants(Ctx *c, const Json *arr, BiomeDef *b) {
    if (!arr) return;
    if (arr->type != JSON_ARRAY || arr->count > BIOME_MAX_PLANTS) { fail(c, arr, "\"plants\" must be an array of at most %d entries.", BIOME_MAX_PLANTS); return; }
    float total = 0;
    for (int i = 0; i < arr->count; i++) {
        const Json *p = arr->items[i];
        if (p->type != JSON_OBJECT) { fail(c, p, "each plant must be an object."); continue; }
        static const char *const keys[] = {"block", "chance", "on", "habitat", "water_dist", "density_noise", NULL};
        check_keys(c, p, keys, "a plant");
        BiomePlant *pl = &b->plants[b->plant_count++];
        pl->state = block_state(c, p, json_str(p, "block", NULL));
        if (pl->state == STATE_AIR) fail(c, p, "a plant cannot be air.");
        pl->chance = 1.0f;
        num_in(c, p, "chance", &pl->chance, 0.0f, 100.0f);
        total += pl->chance;
        pl->water_min = 0.0f; pl->water_max = 1e9f;
        pair_in(c, p, "water_dist", &pl->water_min, &pl->water_max, 0.0f, 100000.0f);
        const char *hab = json_str(p, "habitat", "dry");
        pl->habitat = habitat_id(c, p, hab);
        const Json *on = json_get(p, "on");
        if (!on || on->type != JSON_ARRAY || on->count == 0 || on->count > BIOME_MAX_ON) fail(c, on ? on : p, "\"on\" must list 1 to %d ground block ids.", BIOME_MAX_ON);
        else for (int k = 0; k < on->count; k++) {
            u16 st = block_state(c, on->items[k], json_as_str(on->items[k], NULL));
            if (st != STATE_MISSING) pl->on[pl->on_count++] = st;
        }
        const Json *dn = json_get(p, "density_noise");
        if (dn) {
            if (dn->type != JSON_OBJECT) fail(c, dn, "\"density_noise\" must be {\"noise\":\"a\"|\"b\",\"lo\":n,\"hi\":n}.");
            else {
                pl->has_noise = true;
                pl->noise = (u8)noise_id(c, dn, json_str(dn, "noise", NULL));
                if (pl->noise == 2) fail(c, dn, "plant density noise must be \"a\" or \"b\".");
                pl->noise_lo = (float)json_num(dn, "lo", -2.0);
                pl->noise_hi = (float)json_num(dn, "hi", 2.0);
            }
        }
    }
    if (total > 100.0f) fail(c, arr, "plant chances add up to %g percent; the total must be at most 100.", total);
}

static void parse_biome(const Json *root, const char *rel, const char *owner, BiomeExtraFn extra) {
    Ctx c = {owner, rel, true};
    if (root->type != JSON_OBJECT) { data_error(owner, rel, root->line, "a biome file must be a JSON object."); return; }
    static const char *const keys[] = {"id", "role", "priority", "climate", "surface", "plants", "tree_density_scale", "features", "ores", "structures", "tags", "description", NULL};
    check_keys(&c, root, keys, "a biome file");
    const char *id = json_str(root, "id", NULL);
    if (!id || !strchr(id, ':')) { data_error(owner, rel, root->line, "a biome needs an \"id\" such as \"base:taiga\"."); return; }
    if (biome_find(id) >= 0) { data_error(owner, rel, root->line, "biome \"%s\" is defined twice.", id); return; }
    if (g_n >= BIOME_MAX) { data_error(owner, rel, root->line, "too many biomes; the limit is %d.", BIOME_MAX); return; }
    BiomeDef b = {0};
    snprintf(b.id, sizeof b.id, "%s", id);
    b.tree_density_scale = 1.0f;
    b.height_min = -4096; b.height_max = 4096; b.weird_min = -1.0f; b.weird_max = 1.0f;
    b.temp_extent = b.humid_extent = 1.0f;
    const char *role = json_str(root, "role", "land");
    if (!strcmp(role, "land")) b.role = BIOME_ROLE_LAND;
    else if (!strcmp(role, "ocean")) b.role = BIOME_ROLE_OCEAN;
    else if (!strcmp(role, "shore")) b.role = BIOME_ROLE_SHORE;
    else fail(&c, root, "\"role\" must be land, ocean or shore.");
    b.priority = json_int(root, "priority", 0);
    const Json *cl = json_get(root, "climate");
    if (cl) {
        if (cl->type != JSON_OBJECT) fail(&c, cl, "\"climate\" must be an object.");
        else {
            static const char *const ck[] = {"temperature", "humidity", "height", "weirdness", NULL};
            check_keys(&c, cl, ck, "\"climate\"");
            const Json *t = json_get(cl, "temperature"), *h = json_get(cl, "humidity");
            if (t) { if (t->type == JSON_OBJECT) { num_in(&c, t, "center", &b.temp_center, -2, 2); if (!num_in(&c, t, "extent", &b.temp_extent, 0.02f, 4)) b.temp_extent = 0.3f; } else fail(&c, t, "\"temperature\" must be {\"center\":n,\"extent\":n}."); }
            if (h) { if (h->type == JSON_OBJECT) { num_in(&c, h, "center", &b.humid_center, -2, 2); if (!num_in(&c, h, "extent", &b.humid_extent, 0.02f, 4)) b.humid_extent = 0.3f; } else fail(&c, h, "\"humidity\" must be {\"center\":n,\"extent\":n}."); }
            pair_in(&c, cl, "height", &b.height_min, &b.height_max, -4096, 4096);
            pair_in(&c, cl, "weirdness", &b.weird_min, &b.weird_max, -1.0f, 1.0f);
        }
    }
    parse_layers(&c, json_get(root, "surface"), &b);
    parse_plants(&c, json_get(root, "plants"), &b);
    num_in(&c, root, "tree_density_scale", &b.tree_density_scale, 0.0f, 4.0f);
    const Json *tags = json_get(root, "tags");
    if (tags) {
        if (tags->type != JSON_ARRAY || tags->count > BIOME_MAX_TAGS) fail(&c, tags, "\"tags\" must be an array of at most %d strings.", BIOME_MAX_TAGS);
        else for (int i = 0; i < tags->count; i++) {
            const char *t = json_as_str(tags->items[i], NULL);
            if (!t || strlen(t) >= sizeof b.tags[0]) { fail(&c, tags->items[i], "tags must be strings under %d characters.", (int)sizeof b.tags[0]); continue; }
            snprintf(b.tags[b.tag_count++], sizeof b.tags[0], "%s", t);
        }
    }
    if (!c.ok) return;
    g_b[g_n++] = b;
    if (extra) extra(root, id, rel, owner);
}

static int cmp_file(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* Parsed biomes are collected in file-name order, then re-ordered so the legacy eight come first. */
int biome_load_all(BiomeExtraFn extra) {
    int before = data_error_count();
    biome_reset();
    StrList namespaces = {0};
    vfs_list("data", &namespaces);
    for (int n = 0; n < namespaces.n; n++) {
        char dir[160];
        snprintf(dir, sizeof dir, "data/%s/biomes", namespaces.d[n]);
        StrList files = {0};
        vfs_list(dir, &files);
        if (files.n > 1) qsort(files.d, (size_t)files.n, sizeof files.d[0], cmp_file);
        for (int f = 0; f < files.n; f++) {
            size_t flen = strlen(files.d[f]);
            if (flen < 6 || strcmp(files.d[f] + flen - 5, ".json")) continue;
            char rel[260];
            snprintf(rel, sizeof rel, "%s/%s", dir, files.d[f]);
            size_t size = 0;
            const char *owner = "?";
            u8 *text = vfs_read(rel, &size, &owner);
            if (!text) continue;
            char err[160];
            int line = 0;
            Json *root = json_parse((const char *)text, size, err, sizeof err, &line);
            free(text);
            if (!root) { data_error(owner, rel, line, "%s", err); continue; }
            parse_biome(root, rel, owner, extra);
            json_free(root);
        }
        strlist_free(&files);
    }
    strlist_free(&namespaces);
    /* Order: legacy ids at 0..7 (all required), the others by id. */
    if (g_n) {
        BiomeDef sorted[BIOME_MAX];
        int out = 0;
        bool legacy_ok = true;
        for (int i = 0; i < BIOME_LEGACY; i++) {
            int k = biome_find(LEGACY_IDS[i]);
            if (k < 0) { data_error("base", "data/<namespace>/biomes", 0, "biome \"%s\" is missing; the eight original biomes must all be defined.", LEGACY_IDS[i]); legacy_ok = false; continue; }
            sorted[out++] = g_b[k];
        }
        if (!legacy_ok) { biome_reset(); return data_error_count() - before; }
        for (;;) {
            int pick = -1;
            for (int i = 0; i < g_n; i++) {
                bool used = false;
                for (int k = 0; k < BIOME_LEGACY; k++) if (!strcmp(g_b[i].id, LEGACY_IDS[k])) used = true;
                for (int k = BIOME_LEGACY; k < out && !used; k++) if (!strcmp(g_b[i].id, sorted[k].id)) used = true;
                if (used) continue;
                if (pick < 0 || strcmp(g_b[i].id, g_b[pick].id) < 0) pick = i;
            }
            if (pick < 0) break;
            sorted[out++] = g_b[pick];
        }
        memcpy(g_b, sorted, sizeof sorted);
    }
    return data_error_count() - before;
}

static bool cond_ok(const BiomeCond *w, const BiomeEnv *e, float y) {
    if (e->slope < w->slope_min || e->slope >= w->slope_max) return false;
    if (e->water_dist > w->water_dist_max) return false;
    if (y < w->height_min || y > w->height_max) return false;
    if (e->river <= w->river_min) return false;
    if (w->above_snowline && !(y + e->speck_height > e->snowline)) return false;
    if (w->above_treeline && !(y + e->speck_height > e->treeline)) return false;
    for (int i = 0; i < w->patch_count; i++) {
        float v = e->patch[w->patch[i].noise];
        if (v < w->patch[i].lo || v >= w->patch[i].hi) return false;
    }
    if (w->stripe) {
        int m = e->y % w->stripe_period;
        if (m < 0) m += w->stripe_period;
        if (m < w->stripe_lo || m >= w->stripe_hi) return false;
    }
    return true;
}

u16 biome_surface_state(const BiomeDef *b, const BiomeEnv *e) {
    for (int i = 0; i < b->layer_count; i++) {
        const BiomeLayer *l = &b->layers[i];
        if (l->depth == 0 && cond_ok(&l->when, e, (float)e->y)) return l->state;
    }
    return STATE_MISSING;
}

u16 biome_sub_state(const BiomeDef *b, int depth, const BiomeEnv *e) {
    /* Consecutive layers with the same depth are alternatives for one tier, tried in order. */
    int tier_lo = 1, tier_depth = 0;
    for (int i = 0; i < b->layer_count; i++) {
        const BiomeLayer *l = &b->layers[i];
        if (l->depth == 0) continue;
        if (l->depth != tier_depth) { tier_lo = tier_depth + 1; tier_depth = l->depth; }
        if (depth >= tier_lo && depth <= l->depth && cond_ok(&l->when, e, (float)e->y)) return l->state;
    }
    return STATE_MISSING;
}
