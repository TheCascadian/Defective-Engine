/* Tree species, placement and shapes. See docs/TREES.md.
 *
 * Determinism rules for this file: no libm transcendentals (angles come from a constant table), no clock, no
 * thread-dependent state in any result, and the file is built with -ffp-contract=off so a fused multiply-add on some
 * CPU cannot change a rounded block position. Every random draw comes from a generator seeded by tree_seed(). */
#include "trees.h"

/* --------------------------------------------------------------------------- tables */

static const float SIN_TABLE[91] = {
    0.0f, 0.017452405765652657f, 0.03489949554204941f, 0.0523359552025795f, 0.06975647062063217f, 0.08715574443340302f,
    0.10452846437692642f, 0.12186934053897858f, 0.13917310535907745f, 0.15643446147441864f, 0.1736481785774231f, 0.1908089965581894f,
    0.2079116851091385f, 0.22495105862617493f, 0.24192190170288086f, 0.258819043636322f, 0.27563735842704773f, 0.2923716902732849f,
    0.30901700258255005f, 0.32556816935539246f, 0.3420201539993286f, 0.3583679497241974f, 0.37460657954216003f, 0.3907311260700226f,
    0.4067366421222687f, 0.4226182699203491f, 0.4383711516857147f, 0.45399048924446106f, 0.4694715738296509f, 0.48480960726737976f,
    0.5f, 0.5150380730628967f, 0.5299192667007446f, 0.5446390509605408f, 0.5591928958892822f, 0.5735764503479004f,
    0.5877852439880371f, 0.6018150448799133f, 0.6156615018844604f, 0.6293203830718994f, 0.6427876353263855f, 0.6560590267181396f,
    0.6691306233406067f, 0.6819983720779419f, 0.6946583986282349f, 0.7071067690849304f, 0.7193397879600525f, 0.7313537001609802f,
    0.7431448101997375f, 0.7547096014022827f, 0.7660444378852844f, 0.7771459817886353f, 0.7880107760429382f, 0.7986354827880859f,
    0.80901700258255f, 0.8191520571708679f, 0.8290375471115112f, 0.838670551776886f, 0.8480480909347534f, 0.8571673035621643f,
    0.8660253882408142f, 0.874619722366333f, 0.882947564125061f, 0.8910065293312073f, 0.8987940549850464f, 0.9063078165054321f,
    0.9135454297065735f, 0.9205048680305481f, 0.9271838665008545f, 0.9335803985595703f, 0.9396926164627075f, 0.9455185532569885f,
    0.9510565400123596f, 0.9563047289848328f, 0.9612616896629333f, 0.9659258127212524f, 0.9702957272529602f, 0.9743700623512268f,
    0.9781476259231567f, 0.9816271662712097f, 0.9848077297210693f, 0.9876883625984192f, 0.9902680516242981f, 0.9925461411476135f,
    0.9945219159126282f, 0.9961947202682495f, 0.9975640773773193f, 0.9986295104026794f, 0.9993908405303955f, 0.9998477101325989f,
    1.0f};

static float sin_deg(int d) {
    d %= 360;
    if (d < 0) d += 360;
    int q = d / 90, r = d % 90;
    switch (q) {
    case 0: return SIN_TABLE[r];
    case 1: return SIN_TABLE[90 - r];
    case 2: return -SIN_TABLE[r];
    default: return -SIN_TABLE[90 - r];
    }
}
static float cos_deg(int d) { return sin_deg(d + 90); }
static int iround(float v) { return (int)floorf(v + 0.5f); }
static int fdiv(int a, int b) { int q = a / b; return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q; }

static const char *const TYPE_NAMES[TS_TYPE_COUNT] = {"deciduous_broadleaf", "deciduous_slender", "conifer_pine", "conifer_fir", "palm", "willow", "acacia", "mangrove", "bush", "dead_snag", "fallen_log", "custom"};
static const char *const CANOPY_NAMES[CS_COUNT] = {"irregular_sphere", "conical", "umbrella", "drooping", "layered", "clustered", "frond", "none"};

/* ----------------------------------------------------------------------- species data */

static TreeSpecies g_sp[TREE_MAX_SPECIES];
static int g_n;
static bool g_enabled = true;
static u64 g_rev = 1; /* bumped whenever the species list changes, so cached sites cannot outlive their species */
static char g_bound[TREE_BIOME_SLOTS][64];
static int g_bound_n = -1;
static bool g_explicit_only; /* custom biome sets: only species that name their biomes grow */

bool trees_enabled(void) { return g_enabled && g_n > 0; }
void trees_set_enabled(bool on) { g_enabled = on; }
int trees_species_count(void) { return g_n; }
const TreeSpecies *trees_species(int i) { return i >= 0 && i < g_n ? &g_sp[i] : NULL; }
const TreeSpecies *trees_find(const char *id) {
    for (int i = 0; i < g_n; i++) if (!strcmp(g_sp[i].id, id)) return &g_sp[i];
    return NULL;
}
bool trees_is_leaf_state(u16 state) {
    if (state == STATE_AIR) return false;
    for (int i = 0; i < g_n; i++) if (g_sp[i].leaves == state) return true;
    return false;
}
int trees_max_reach(void) {
    int r = 2;
    for (int i = 0; i < g_n; i++) r = MAX(r, g_sp[i].reach);
    return r;
}

void trees_reset(void) {
    g_rev++;
    g_n = 0;
    g_bound_n = -1;
}

static void bind_one(TreeSpecies *sp) {
    for (int i = 0; i < TREE_BIOME_SLOTS; i++) {
        bool ok = sp->biome_count == 0 && !g_explicit_only;
        for (int b = 0; b < sp->biome_count && g_bound_n >= 0 && i < g_bound_n && !ok; b++) ok = !strcmp(sp->biomes[b], g_bound[i]);
        sp->biome_ok[i] = ok;
    }
}

void trees_bind_biomes(const char *const *ids, int count, bool explicit_only) {
    g_explicit_only = explicit_only;
    g_rev++;
    g_bound_n = MIN(count, TREE_BIOME_SLOTS);
    for (int i = 0; i < g_bound_n; i++) snprintf(g_bound[i], sizeof g_bound[i], "%s", ids[i]);
    for (int i = 0; i < g_n; i++) bind_one(&g_sp[i]);
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
        if (known) continue;
        char list[300] = "";
        for (int k = 0; allowed[k]; k++) { size_t used = strlen(list); snprintf(list + used, sizeof list - used, "%s%s", k ? ", " : "", allowed[k]); }
        fail(c, obj->items[i], "unknown field \"%s\" in %s. Known fields: %s.", obj->keys[i], where, list);
    }
}

static const Json *get_obj(Ctx *c, const Json *o, const char *key) {
    const Json *v = json_get(o, key);
    if (!v) return NULL;
    if (v->type != JSON_OBJECT) { fail(c, v, "\"%s\" must be an object.", key); return NULL; }
    return v;
}

static void read_float(Ctx *c, const Json *o, const char *key, float *out, float lo, float hi) {
    const Json *v = json_get(o, key);
    if (!v) return;
    if (v->type != JSON_NUMBER) { fail(c, v, "\"%s\" must be a number between %g and %g.", key, lo, hi); return; }
    if (v->num < lo || v->num > hi) { fail(c, v, "\"%s\" is %g; it must be between %g and %g.", key, v->num, lo, hi); return; }
    *out = (float)v->num;
}

static void read_int(Ctx *c, const Json *o, const char *key, int *out, int lo, int hi) {
    float f = (float)*out;
    const Json *v = json_get(o, key);
    if (v && v->type == JSON_NUMBER && v->num != floor(v->num)) { fail(c, v, "\"%s\" must be a whole number.", key); return; }
    read_float(c, o, key, &f, (float)lo, (float)hi);
    *out = (int)f;
}

static void read_bool(Ctx *c, const Json *o, const char *key, bool *out) {
    const Json *v = json_get(o, key);
    if (!v) return;
    if (v->type != JSON_BOOL) { fail(c, v, "\"%s\" must be true or false.", key); return; }
    *out = v->boolean;
}

/* A range is [lo, hi] or a single number. */
static void read_range(Ctx *c, const Json *o, const char *key, TRange *r, float lo, float hi) {
    const Json *v = json_get(o, key);
    if (!v) return;
    float a, b;
    if (v->type == JSON_NUMBER) a = b = (float)v->num;
    else if (v->type == JSON_ARRAY && v->count == 2 && v->items[0]->type == JSON_NUMBER && v->items[1]->type == JSON_NUMBER) { a = (float)v->items[0]->num; b = (float)v->items[1]->num; }
    else { fail(c, v, "\"%s\" must be a number or a [min, max] pair such as [%g, %g].", key, lo, hi); return; }
    if (a > b) { fail(c, v, "\"%s\" has min %g above max %g.", key, a, b); return; }
    if (a < lo || b > hi) { fail(c, v, "\"%s\" must stay between %g and %g (got [%g, %g]).", key, lo, hi, a, b); return; }
    r->lo = a;
    r->hi = b;
}

static bool is_understory(TreeShapeType t) { return t == TS_BUSH || t == TS_DEAD_SNAG || t == TS_FALLEN_LOG; }

/* Every shape type's own ranges; a species file only has to state what differs. */
static void shape_defaults(TreeSpecies *s, TreeShapeType t) {
    s->type = t;
    s->canopy = CS_IRREGULAR_SPHERE;
    s->height = (TRange){8, 14}; s->trunk_radius = (TRange){0.5f, 1.2f}; s->trunk_lean = (TRange){0, 0.08f}; s->trunk_taper = (TRange){0.2f, 0.4f};
    s->trunk_curve = (TRange){0, 1.5f}; s->branch_count = (TRange){3, 6}; s->branch_angle = (TRange){30, 60}; s->branch_length_ratio = (TRange){0.35f, 0.7f};
    s->canopy_radius = (TRange){2.5f, 4.2f}; s->canopy_density = (TRange){0.55f, 0.8f}; s->canopy_flatness = (TRange){0.75f, 1.0f};
    s->root_flare = (TRange){0, 0.25f}; s->buttress = (TRange){0, 0}; s->leaf_clumping = 0.6f; s->fork_chance = 0.3f;
    s->min_radius = 4;
    s->layer = is_understory(t) ? 1 : 0;
    switch (t) {
    case TS_DECIDUOUS_SLENDER:
        s->height = (TRange){10, 17}; s->trunk_radius = (TRange){0.4f, 0.6f}; s->trunk_lean = (TRange){0, 0.1f}; s->trunk_curve = (TRange){0, 2.0f};
        s->trunk_taper = (TRange){0.3f, 0.5f}; s->branch_count = (TRange){2, 5}; s->branch_angle = (TRange){20, 40}; s->branch_length_ratio = (TRange){0.2f, 0.4f};
        s->canopy_radius = (TRange){1.8f, 2.8f}; s->canopy_density = (TRange){0.6f, 0.85f}; s->canopy_flatness = (TRange){1.2f, 1.7f};
        s->root_flare = (TRange){0, 0.1f}; s->leaf_clumping = 0.5f; s->fork_chance = 0.12f; s->min_radius = 3;
        break;
    case TS_CONIFER_PINE:
        s->canopy = CS_LAYERED;
        s->height = (TRange){14, 26}; s->trunk_radius = (TRange){0.5f, 1.1f}; s->trunk_lean = (TRange){0, 0.04f}; s->trunk_curve = (TRange){0, 1.0f};
        s->trunk_taper = (TRange){0.5f, 0.7f}; s->branch_count = (TRange){4, 8}; s->branch_angle = (TRange){70, 85}; s->branch_length_ratio = (TRange){0.15f, 0.3f};
        s->canopy_radius = (TRange){2.5f, 4.0f}; s->canopy_density = (TRange){0.55f, 0.8f}; s->canopy_flatness = (TRange){1, 1};
        s->root_flare = (TRange){0, 0.1f}; s->fork_chance = 0.0f; s->min_radius = 4;
        break;
    case TS_CONIFER_FIR:
        s->canopy = CS_CONICAL;
        s->height = (TRange){12, 22}; s->trunk_radius = (TRange){0.5f, 0.9f}; s->trunk_lean = (TRange){0, 0.03f}; s->trunk_curve = (TRange){0, 0.8f};
        s->trunk_taper = (TRange){0.6f, 0.8f}; s->branch_count = (TRange){0, 0}; s->branch_angle = (TRange){70, 85}; s->branch_length_ratio = (TRange){0.15f, 0.3f};
        s->canopy_radius = (TRange){2.5f, 4.0f}; s->canopy_density = (TRange){0.7f, 0.95f}; s->canopy_flatness = (TRange){1, 1};
        s->root_flare = (TRange){0, 0.1f}; s->fork_chance = 0.0f; s->min_radius = 3;
        break;
    case TS_PALM:
        s->canopy = CS_FROND;
        s->height = (TRange){7, 14}; s->trunk_radius = (TRange){0.5f, 0.7f}; s->trunk_lean = (TRange){0.08f, 0.25f}; s->trunk_curve = (TRange){0.5f, 2.5f};
        s->trunk_taper = (TRange){0.1f, 0.2f}; s->branch_count = (TRange){6, 9}; s->canopy_radius = (TRange){3, 5}; s->canopy_density = (TRange){0.6f, 0.85f};
        s->root_flare = (TRange){0.1f, 0.3f}; s->fork_chance = 0.0f; s->min_radius = 4;
        break;
    case TS_WILLOW:
        s->canopy = CS_DROOPING;
        s->height = (TRange){8, 13}; s->trunk_radius = (TRange){0.8f, 1.5f}; s->trunk_lean = (TRange){0, 0.06f}; s->branch_count = (TRange){3, 5};
        s->branch_angle = (TRange){40, 65}; s->branch_length_ratio = (TRange){0.45f, 0.8f}; s->canopy_radius = (TRange){3.5f, 5.5f};
        s->canopy_density = (TRange){0.6f, 0.85f}; s->canopy_flatness = (TRange){0.55f, 0.8f}; s->root_flare = (TRange){0.1f, 0.3f}; s->min_radius = 5;
        break;
    case TS_ACACIA:
        s->canopy = CS_UMBRELLA;
        s->height = (TRange){6, 11}; s->trunk_radius = (TRange){0.5f, 1.0f}; s->trunk_lean = (TRange){0.02f, 0.12f}; s->trunk_curve = (TRange){0.5f, 2.0f};
        s->trunk_taper = (TRange){0.1f, 0.3f}; s->branch_count = (TRange){2, 3}; s->branch_angle = (TRange){30, 55}; s->branch_length_ratio = (TRange){0.45f, 0.8f};
        s->canopy_radius = (TRange){3.5f, 5.5f}; s->canopy_flatness = (TRange){0.4f, 0.6f}; s->root_flare = (TRange){0, 0.15f}; s->fork_chance = 1.0f; s->min_radius = 5;
        break;
    case TS_MANGROVE:
        s->height = (TRange){4, 8}; s->trunk_radius = (TRange){0.5f, 0.9f}; s->trunk_curve = (TRange){0, 1.2f}; s->trunk_taper = (TRange){0.1f, 0.3f};
        s->branch_count = (TRange){2, 4}; s->branch_angle = (TRange){35, 65}; s->branch_length_ratio = (TRange){0.3f, 0.6f}; s->canopy_radius = (TRange){2.2f, 3.4f};
        s->canopy_density = (TRange){0.7f, 0.9f}; s->canopy_flatness = (TRange){0.55f, 0.8f}; s->root_flare = (TRange){0.5f, 1.0f}; s->min_radius = 3;
        break;
    case TS_BUSH:
        s->height = (TRange){1, 2}; s->trunk_radius = (TRange){0.5f, 0.5f}; s->trunk_lean = (TRange){0, 0}; s->trunk_taper = (TRange){0, 0}; s->trunk_curve = (TRange){0, 0};
        s->branch_count = (TRange){0, 0}; s->canopy_radius = (TRange){1.2f, 2.4f}; s->canopy_density = (TRange){0.65f, 0.9f}; s->canopy_flatness = (TRange){0.7f, 1.0f};
        s->root_flare = (TRange){0, 0}; s->leaf_clumping = 0.5f; s->fork_chance = 0; s->min_radius = 1;
        break;
    case TS_DEAD_SNAG:
        s->canopy = CS_NONE;
        s->height = (TRange){4, 12}; s->trunk_radius = (TRange){0.5f, 1.0f}; s->trunk_lean = (TRange){0, 0.12f}; s->trunk_taper = (TRange){0.2f, 0.5f};
        s->branch_count = (TRange){0, 3}; s->branch_angle = (TRange){50, 85}; s->branch_length_ratio = (TRange){0.1f, 0.25f}; s->canopy_radius = (TRange){0, 0};
        s->canopy_density = (TRange){0, 0}; s->root_flare = (TRange){0, 0.15f}; s->fork_chance = 0; s->min_radius = 3;
        break;
    case TS_FALLEN_LOG:
        s->canopy = CS_NONE;
        s->height = (TRange){3, 9}; s->trunk_radius = (TRange){0.5f, 1.0f}; s->trunk_lean = (TRange){0, 0.3f}; s->trunk_taper = (TRange){0, 0};
        s->trunk_curve = (TRange){0, 0}; s->branch_count = (TRange){0, 0}; s->canopy_radius = (TRange){0, 0}; s->canopy_density = (TRange){0, 0};
        s->root_flare = (TRange){0, 0}; s->fork_chance = 0; s->min_radius = 3;
        break;
    default: break;
    }
}

/* Horizontal reach of the largest tree the ranges allow, so placement knows how far a trunk can matter. Generators
 * clip to this value, so an underestimate trims a tree and never breaks cross-chunk agreement. */
static int compute_reach(const TreeSpecies *s) {
    float H = s->height.hi, drift = s->trunk_lean.hi * H + s->trunk_curve.hi * 0.5f;
    float branch = H * s->branch_length_ratio.hi * sin_deg((int)s->branch_angle.hi) * 1.15f;
    float r;
    switch (s->type) {
    case TS_FALLEN_LOG: r = H * 0.5f + 2.0f; break;
    case TS_DEAD_SNAG: r = drift + branch + 2.0f; break;
    case TS_PALM: r = drift + s->canopy_radius.hi * 1.3f + 3.0f; break;
    case TS_CONIFER_PINE: case TS_CONIFER_FIR: r = drift + s->canopy_radius.hi * 1.3f + 2.0f; break;
    case TS_ACACIA: r = drift + H * 0.55f + s->canopy_radius.hi + 2.0f; break;
    default: r = drift + MAX(s->canopy_radius.hi * 1.25f, branch + s->canopy_radius.hi * 0.7f) + 2.0f; break;
    }
    if (s->type == TS_MANGROVE) r = MAX(r, 6.0f);
    return CLAMP((int)ceilf(r), 2, TREE_REACH);
}

static u16 find_block(Ctx *c, const Json *at, const char *field, const char *name) {
    BlockDef *b = block_find(name);
    if (!b) { fail(c, at, "\"%s\" names unknown block \"%s\". Define it in data/<namespace>/blocks or fix the name.", field, name); return STATE_MISSING; }
    return b->default_state;
}

static bool parse_species(const Json *root, const char *rel, const char *owner) {
    Ctx c = {owner, rel, true};
    static const char *const ROOT_KEYS[] = {"id", "log", "leaves", "sapling", "biomes", "climate", "substrate", "elevation", "water", "spacing", "shape",
                                            "density", "succession", "light", "layer", "kernel", NULL};
    check_keys(&c, root, ROOT_KEYS, "a tree species");
    TreeSpecies s;
    memset(&s, 0, sizeof s);
    snprintf(s.owner, sizeof s.owner, "%s", owner ? owner : "?");
    snprintf(s.file, sizeof s.file, "%s", rel);
    const char *id = json_str(root, "id", NULL);
    if (!id) fail(&c, root, "a tree species needs an \"id\" such as \"base:oak\".");
    else if (!strchr(id, ':') || strlen(id) >= sizeof s.id || strchr(id, ':') == id || id[strlen(id) - 1] == ':')
        fail(&c, json_get(root, "id"), "species id \"%s\" must be namespaced like \"base:oak\" and shorter than %d characters.", id, (int)sizeof s.id);
    else snprintf(s.id, sizeof s.id, "%s", id);

    /* Shape first: its type decides every default below. */
    const Json *shape = get_obj(&c, root, "shape");
    TreeShapeType type = TS_CUSTOM;
    if (!shape) { if (c.ok) fail(&c, root, "a tree species needs a \"shape\" object with a \"type\", for example {\"type\": \"deciduous_broadleaf\"}."); }
    else {
        const char *tn = json_str(shape, "type", NULL);
        int found = -1;
        for (int i = 0; tn && i < TS_TYPE_COUNT; i++) if (!strcmp(TYPE_NAMES[i], tn)) found = i;
        if (found < 0) {
            char list[300] = "";
            for (int i = 0; i < TS_TYPE_COUNT; i++) { size_t u = strlen(list); snprintf(list + u, sizeof list - u, "%s%s", i ? ", " : "", TYPE_NAMES[i]); }
            fail(&c, json_get(shape, "type") ? json_get(shape, "type") : shape, "shape type %s%s%s is not known. Use one of: %s.", tn ? "\"" : "(missing)", tn ? tn : "", tn ? "\"" : "", list);
        } else type = (TreeShapeType)found;
    }
    shape_defaults(&s, type);
    s.temperature = (TRange){0, 1}; s.moisture = (TRange){0, 1}; s.light = (TRange){0, 1};
    s.min_y = -4096; s.max_y = 4096; s.max_slope = 0.6f;
    s.water_min = 1.5f; s.water_max = 1.0e9f;
    s.cluster_size = 1; s.clustering = 0.0f; s.density = 1.0f; s.succession = 1;

    if (shape) {
        static const char *const SHAPE_KEYS[] = {"type", "height", "trunk_radius", "trunk_lean", "trunk_taper", "trunk_curve", "branch_count", "branch_angle", "branch_length_ratio",
                                                 "canopy_shape", "canopy_radius", "canopy_density", "canopy_flatness", "root_flare", "buttress", "leaf_clumping", "fork_chance", NULL};
        check_keys(&c, shape, SHAPE_KEYS, "\"shape\"");
        const char *cn = json_str(shape, "canopy_shape", NULL);
        if (cn) {
            int f = -1;
            for (int i = 0; i < CS_COUNT; i++) if (!strcmp(CANOPY_NAMES[i], cn)) f = i;
            if (f < 0) fail(&c, json_get(shape, "canopy_shape"), "canopy_shape \"%s\" is not known. Use one of: irregular_sphere, conical, umbrella, drooping, layered, clustered, frond, none.", cn);
            else s.canopy = (CanopyShape)f;
        } else if (type == TS_CUSTOM) fail(&c, shape, "a \"custom\" shape must name its \"canopy_shape\" (irregular_sphere, conical, umbrella, drooping, layered, clustered, frond or none).");
        read_range(&c, shape, "height", &s.height, 1, TREE_MAX_HEIGHT);
        read_range(&c, shape, "trunk_radius", &s.trunk_radius, 0.3f, 3.0f);
        read_range(&c, shape, "trunk_lean", &s.trunk_lean, 0, 0.6f);
        read_range(&c, shape, "trunk_taper", &s.trunk_taper, 0, 0.9f);
        read_range(&c, shape, "trunk_curve", &s.trunk_curve, 0, 6);
        read_range(&c, shape, "branch_count", &s.branch_count, 0, 12);
        read_range(&c, shape, "branch_angle", &s.branch_angle, 5, 90);
        read_range(&c, shape, "branch_length_ratio", &s.branch_length_ratio, 0.05f, 1.2f);
        read_range(&c, shape, "canopy_radius", &s.canopy_radius, 0, 12);
        read_range(&c, shape, "canopy_density", &s.canopy_density, 0, 1);
        read_range(&c, shape, "canopy_flatness", &s.canopy_flatness, 0.3f, 2.5f);
        read_range(&c, shape, "root_flare", &s.root_flare, 0, 1);
        read_range(&c, shape, "buttress", &s.buttress, 0, 6);
        read_float(&c, shape, "leaf_clumping", &s.leaf_clumping, 0, 1);
        read_float(&c, shape, "fork_chance", &s.fork_chance, 0, 1);
    }

    const char *needs_leaves = NULL;
    bool foliage = s.canopy != CS_NONE;
    const char *logn = json_str(root, "log", NULL), *leafn = json_str(root, "leaves", NULL), *sapn = json_str(root, "sapling", NULL);
    if (!logn) fail(&c, root, "a tree species needs a \"log\" block such as \"base:log\".");
    else { snprintf(s.log_name, sizeof s.log_name, "%s", logn); s.log = find_block(&c, json_get(root, "log"), "log", logn); }
    if (leafn) { snprintf(s.leaves_name, sizeof s.leaves_name, "%s", leafn); s.leaves = find_block(&c, json_get(root, "leaves"), "leaves", leafn); }
    else if (foliage && !is_understory(type)) needs_leaves = "a tree species with a canopy needs a \"leaves\" block such as \"base:leaves\".";
    else if (foliage) needs_leaves = "a species with leaves needs a \"leaves\" block such as \"base:leaves\".";
    if (needs_leaves) fail(&c, root, "%s", needs_leaves);
    if (sapn) { snprintf(s.sapling_name, sizeof s.sapling_name, "%s", sapn); find_block(&c, json_get(root, "sapling"), "sapling", sapn); }

    const Json *b = json_get(root, "biomes");
    if (b) {
        if (b->type != JSON_ARRAY || b->count > 8) fail(&c, b, "\"biomes\" must be an array of at most 8 biome ids such as [\"base:forest\"].");
        else for (int i = 0; i < b->count; i++) {
            if (b->items[i]->type != JSON_STRING) { fail(&c, b->items[i], "\"biomes\" entries must be biome id strings."); break; }
            snprintf(s.biomes[s.biome_count++], sizeof s.biomes[0], "%s", b->items[i]->str);
        }
    }
    const Json *cl = get_obj(&c, root, "climate");
    if (cl) {
        static const char *const K[] = {"temperature", "moisture", NULL};
        check_keys(&c, cl, K, "\"climate\"");
        read_range(&c, cl, "temperature", &s.temperature, 0, 1);
        read_range(&c, cl, "moisture", &s.moisture, 0, 1);
    }
    const Json *sub = json_get(root, "substrate");
    if (sub) {
        if (sub->type != JSON_ARRAY || sub->count > 12) fail(&c, sub, "\"substrate\" must be an array of at most 12 block ids such as [\"base:grass_block\"].");
        else for (int i = 0; i < sub->count; i++) {
            if (sub->items[i]->type != JSON_STRING) { fail(&c, sub->items[i], "\"substrate\" entries must be block id strings."); break; }
            u16 st = find_block(&c, sub->items[i], "substrate", sub->items[i]->str);
            if (st != STATE_MISSING) s.substrate[s.substrate_count++] = st;
        }
    }
    const Json *el = get_obj(&c, root, "elevation");
    if (el) {
        static const char *const K[] = {"min_y", "max_y", "max_slope", NULL};
        check_keys(&c, el, K, "\"elevation\"");
        read_int(&c, el, "min_y", &s.min_y, -4096, 4096);
        read_int(&c, el, "max_y", &s.max_y, -4096, 4096);
        read_float(&c, el, "max_slope", &s.max_slope, 0, 4);
        if (s.min_y > s.max_y) fail(&c, el, "elevation min_y %d is above max_y %d.", s.min_y, s.max_y);
    }
    const Json *wt = get_obj(&c, root, "water");
    if (wt) {
        static const char *const K[] = {"min_distance", "max_distance", "prefer_near", "allow_shallow", NULL};
        check_keys(&c, wt, K, "\"water\"");
        read_float(&c, wt, "min_distance", &s.water_min, 0, 64);
        read_float(&c, wt, "max_distance", &s.water_max, 0, 1.0e9f);
        read_bool(&c, wt, "prefer_near", &s.prefer_near);
        read_bool(&c, wt, "allow_shallow", &s.allow_shallow);
        if (s.water_min > s.water_max) fail(&c, wt, "water min_distance %g is above max_distance %g.", s.water_min, s.water_max);
    }
    const Json *sp = get_obj(&c, root, "spacing");
    if (sp) {
        static const char *const K[] = {"min_radius", "cluster_size", "clustering", NULL};
        check_keys(&c, sp, K, "\"spacing\"");
        read_int(&c, sp, "min_radius", &s.min_radius, 1, 12);
        read_int(&c, sp, "cluster_size", &s.cluster_size, 1, 8);
        read_float(&c, sp, "clustering", &s.clustering, 0, 1);
    }
    read_float(&c, root, "density", &s.density, 0, 4);
    const char *succ = json_str(root, "succession", NULL);
    if (json_get(root, "succession")) {
        if (!succ || (strcmp(succ, "pioneer") && strcmp(succ, "mid") && strcmp(succ, "climax"))) fail(&c, json_get(root, "succession"), "\"succession\" must be \"pioneer\", \"mid\" or \"climax\".");
        else s.succession = !strcmp(succ, "pioneer") ? 0 : (!strcmp(succ, "mid") ? 1 : 2);
    }
    const Json *li = get_obj(&c, root, "light");
    if (li) {
        static const char *const K[] = {"min", "max", NULL};
        check_keys(&c, li, K, "\"light\"");
        read_float(&c, li, "min", &s.light.lo, 0, 1);
        read_float(&c, li, "max", &s.light.hi, 0, 1);
        if (s.light.lo > s.light.hi) fail(&c, li, "light min %g is above max %g.", s.light.lo, s.light.hi);
    }
    read_int(&c, root, "kernel", &s.kernel, 0, 1);
    const char *layer = json_str(root, "layer", NULL);
    if (json_get(root, "layer")) {
        if (!layer || (strcmp(layer, "canopy") && strcmp(layer, "understory"))) fail(&c, json_get(root, "layer"), "\"layer\" must be \"canopy\" or \"understory\".");
        else s.layer = !strcmp(layer, "understory");
    }
    if (c.ok && s.cluster_size > 1 && s.clustering <= 0.0f) s.clustering = 0.5f;
    if (c.ok && s.trunk_radius.lo > s.trunk_radius.hi) fail(&c, root, "trunk_radius is inverted.");
    if (!c.ok) return false;
    s.reach = compute_reach(&s);
    bind_one(&s);
    for (int i = 0; i < g_n; i++)
        if (!strcmp(g_sp[i].id, s.id)) { g_sp[i] = s; bind_one(&g_sp[i]); g_rev++; return true; } /* a later mod replaces the species */
    if (g_n >= TREE_MAX_SPECIES) { data_error(owner, rel, 1, "too many tree species; the limit is %d. Remove some or merge them.", TREE_MAX_SPECIES); return false; }
    g_sp[g_n++] = s;
    g_rev++;
    return true;
}

static int species_cmp(const void *a, const void *b) { return strcmp(((const TreeSpecies *)a)->id, ((const TreeSpecies *)b)->id); }

/* Species are kept sorted by id so selection never depends on directory listing order. */
static void sort_species(void) {
    qsort(g_sp, (size_t)g_n, sizeof g_sp[0], species_cmp);
    int rank[2] = {0, 0};
    for (int i = 0; i < g_n; i++) g_sp[i].salt = g_sp[i].kernel ? 1000 + rank[1]++ : rank[0]++;
}

bool trees_load_text(const char *text, size_t len, const char *rel, const char *owner) {
    char err[200];
    int line = 0;
    Json *root = json_parse(text, len, err, sizeof err, &line);
    if (!root) { data_error(owner, rel, line, "%s. Fix the JSON syntax at that line.", err); return false; }
    bool ok = false;
    if (root->type != JSON_OBJECT) data_error(owner, rel, 1, "a tree species file must contain one JSON object like {\"id\": \"base:oak\", \"log\": \"base:log\", ...}; see docs/TREES.md.");
    else ok = parse_species(root, rel, owner);
    json_free(root);
    sort_species();
    return ok;
}

int trees_load(void) {
    int before = data_error_count();
    g_n = 0;
    StrList namespaces = {0};
    vfs_list("data", &namespaces);
    for (int n = 0; n < namespaces.n; n++) {
        char dir[160];
        snprintf(dir, sizeof dir, "data/%s/trees", namespaces.d[n]);
        StrList files = {0};
        vfs_list(dir, &files);
        for (int f = 0; f < files.n; f++) {
            size_t flen = strlen(files.d[f]);
            if (flen < 6 || strcmp(files.d[f] + flen - 5, ".json")) continue;
            char rel[260];
            snprintf(rel, sizeof rel, "%s/%s", dir, files.d[f]);
            size_t size = 0;
            const char *owner = "?";
            u8 *text = vfs_read(rel, &size, &owner);
            if (!text) continue;
            trees_load_text((const char *)text, size, rel, owner);
            free(text);
        }
        strlist_free(&files);
    }
    strlist_free(&namespaces);
    return data_error_count() - before;
}

/* ------------------------------------------------------------------------ the grid */

#define GX (2 * TREE_REACH + 1)
#define GZ (2 * TREE_REACH + 1)
#define GY (TREE_BELOW + TREE_MAX_HEIGHT + TREE_ABOVE)
#define CELL_MAX 16384
#define MAX_ANCHORS 96

static _Thread_local u8 g_grid[GX * GY * GZ];
static _Thread_local int g_touch[CELL_MAX], g_queue[CELL_MAX];
static _Thread_local int g_ntouch;
static _Thread_local TreeVoxel g_vox[CELL_MAX];
static _Thread_local TreeShape g_shape;

typedef struct TG {
    const TreeSpecies *sp;
    u64 seed;
    Rng rng;
    int reach, clipped, ycap;
    int n_anchor;
    int ax[MAX_ANCHORS], ay[MAX_ANCHORS], az[MAX_ANCHORS];
    int H, nb;
    float tr, taper, lean, curve, cr, cd, flat, flare, butt;
    int cx[TREE_MAX_HEIGHT + 2], cz[TREE_MAX_HEIGHT + 2];
} TG;

static float rf(TG *g) { return rng_float(&g->rng); }
static int ri(TG *g, int lo, int hi) { return rng_range(&g->rng, lo, hi); }
static float rr(TG *g, TRange r) { return r.lo + (r.hi - r.lo) * rf(g); }

static int gidx(int x, int y, int z) { return ((y + TREE_BELOW) * GZ + (z + TREE_REACH)) * GX + (x + TREE_REACH); }

static void cell_set(TG *g, int x, int y, int z, u8 kind) {
    if (x < -g->reach || x > g->reach || z < -g->reach || z > g->reach || y < -TREE_BELOW || y >= GY - TREE_BELOW) { g->clipped++; return; }
    u8 *c = &g_grid[gidx(x, y, z)];
    if (*c == 0) {
        if (g_ntouch >= CELL_MAX) { g->clipped++; return; }
        g_touch[g_ntouch++] = gidx(x, y, z);
        *c = kind;
    } else if (kind > *c) *c = kind; /* a log replaces a leaf, and a root-type log replaces a plain one */
}

static u8 cell_get(int x, int y, int z) {
    if (x < -TREE_REACH || x > TREE_REACH || z < -TREE_REACH || z > TREE_REACH || y < -TREE_BELOW || y >= GY - TREE_BELOW) return 0;
    return g_grid[gidx(x, y, z)] & 0x0F;
}

/* Six-connected line: every step changes one coordinate, so consecutive cells share a face. */
static void line6(TG *g, int x0, int y0, int z0, int x1, int y1, int z1, u8 kind) {
    int p[3] = {x0, y0, z0}, n[3] = {abs(x1 - x0), abs(y1 - y0), abs(z1 - z0)}, step[3] = {x1 > x0 ? 1 : -1, y1 > y0 ? 1 : -1, z1 > z0 ? 1 : -1}, c[3] = {0, 0, 0};
    cell_set(g, p[0], p[1], p[2], kind);
    while (c[0] < n[0] || c[1] < n[1] || c[2] < n[2]) {
        int best = -1;
        for (int a = 0; a < 3; a++) {
            if (c[a] >= n[a]) continue;
            if (best < 0 || (long)(2 * c[a] + 1) * n[best] < (long)(2 * c[best] + 1) * n[a]) best = a;
        }
        p[best] += step[best];
        c[best]++;
        cell_set(g, p[0], p[1], p[2], kind);
    }
}

static void disc(TG *g, int cx, int y, int cz, float r, u8 kind) {
    float r2 = MAX(r, 0.5f) * MAX(r, 0.5f) + 0.01f;
    int ir = (int)ceilf(r);
    for (int dz = -ir; dz <= ir; dz++)
        for (int dx = -ir; dx <= ir; dx++)
            if ((float)(dx * dx + dz * dz) <= r2) cell_set(g, cx + dx, y, cz + dz, kind);
}

static u64 cell_hash(u64 seed, int x, int y, int z) {
    return hash64(seed ^ ((u64)(i64)x * 0x9E3779B97F4A7C15ull) ^ ((u64)(i64)y * 0xC2B2AE3D27D4EB4Full) ^ ((u64)(i64)z * 0x165667B19E3779F9ull));
}

/* Leaf keep test: white noise, biased by a coarse noise so leaves come in clumps rather than as salt and pepper. */
static bool leaf_roll(TG *g, int x, int y, int z, float p) {
    float u = hash_to_unit(cell_hash(g->seed, x, y, z));
    float clump = g->sp->leaf_clumping;
    if (clump > 0.0f) {
        float coarse = hash_to_unit(cell_hash(g->seed ^ 0xC1u, x >> 1, y >> 1, z >> 1));
        p += clump * (coarse - 0.5f) * 1.2f;
    }
    return u < p;
}

static void leaf_blob(TG *g, int ox, int oy, int oz, float rx, float ry, float rz, float dens) {
    rx = MAX(rx, 0.6f); ry = MAX(ry, 0.6f); rz = MAX(rz, 0.6f);
    int ix = (int)ceilf(rx), iy = (int)ceilf(ry), iz = (int)ceilf(rz);
    for (int y = -iy; y <= iy; y++)
        for (int z = -iz; z <= iz; z++)
            for (int x = -ix; x <= ix; x++) {
                float fx = (float)x / rx, fy = (float)y / ry, fz = (float)z / rz, d2 = fx * fx + fy * fy + fz * fz;
                if (d2 > 1.0f) continue;
                if (leaf_roll(g, ox + x, oy + y, oz + z, dens * (1.2f - 0.8f * d2))) cell_set(g, ox + x, oy + y, oz + z, TV_LEAF);
            }
}

static void leaf_disc(TG *g, int ox, int oy, int oz, float r, float dens) {
    r = MAX(r, 0.6f);
    int ir = (int)ceilf(r);
    for (int z = -ir; z <= ir; z++)
        for (int x = -ir; x <= ir; x++) {
            float d2 = (float)(x * x + z * z) / (r * r);
            if (d2 > 1.0f) continue;
            if (leaf_roll(g, ox + x, oy, oz + z, dens * (1.15f - 0.7f * d2))) cell_set(g, ox + x, oy, oz + z, TV_LEAF);
        }
}

static void add_anchor(TG *g, int x, int y, int z) {
    if (g->n_anchor >= MAX_ANCHORS) return;
    g->ax[g->n_anchor] = x; g->ay[g->n_anchor] = y; g->az[g->n_anchor] = z;
    g->n_anchor++;
}

/* ------------------------------------------------------------------- structure parts */

static void build_trunk(TG *g, int H) {
    int az = ri(g, 0, 359);
    float drift = g->lean * (float)H, curve = g->curve;
    float ldx = drift * cos_deg(az), ldz = drift * sin_deg(az), cdx = curve * cos_deg(az + 90), cdz = curve * sin_deg(az + 90);
    for (int y = 0; y < H; y++) {
        float s = H > 1 ? (float)y / (float)(H - 1) : 0.0f, bend = 4.0f * s * (1.0f - s);
        g->cx[y] = iround(ldx * s + cdx * bend);
        g->cz[y] = iround(ldz * s + cdz * bend);
    }
    for (int y = 0; y < H; y++) {
        float s = H > 1 ? (float)y / (float)(H - 1) : 0.0f, r = MAX(g->tr * (1.0f - g->taper * s), 0.5f);
        disc(g, g->cx[y], y, g->cz[y], r, y == 0 ? TV_ROOT : TV_LOG);
        if (y > 0) line6(g, g->cx[y - 1], y - 1, g->cz[y - 1], g->cx[y], y, g->cz[y], TV_LOG);
    }
}

static void root_flare(TG *g) {
    int len = (int)(g->flare * (float)g->H + 0.5f);
    if (len < 1) return;
    int n = ri(g, 3, 5), a0 = ri(g, 0, 359);
    for (int i = 0; i < n; i++) {
        int a = a0 + i * 360 / n + ri(g, -25, 25);
        int l = MAX(1, len + ri(g, -1, 0));
        line6(g, g->cx[0], 0, g->cz[0], g->cx[0] + iround((float)l * cos_deg(a)), 0, g->cz[0] + iround((float)l * sin_deg(a)), TV_ROOT);
    }
}

static void buttress_fins(TG *g) {
    int n = (int)(g->butt + 0.5f);
    if (n < 1) return;
    n = MIN(n, 6);
    int a0 = ri(g, 0, 359), hb = 2 + g->H / 6;
    for (int i = 0; i < n; i++) {
        int a = a0 + i * 360 / n + ri(g, -20, 20), L = 2 + (g->H >= 12);
        int px = g->cx[0], pz = g->cz[0];
        for (int d = 1; d <= L; d++) {
            int x = g->cx[0] + iround((float)d * cos_deg(a)), z = g->cz[0] + iround((float)d * sin_deg(a)), h = MAX(0, hb - (d - 1) * hb / L);
            for (int y = 0; y <= h; y++) {
                int py = y;
                line6(g, px, py, pz, x, py, z, y == 0 ? TV_ROOT : TV_LOG);
            }
            px = x; pz = z;
        }
    }
}

static void grow_branch(TG *g, float x, float y, float z, float dx, float dy, float dz, int len, int depth, float droop) {
    int px = iround(x), py = iround(y), pz = iround(z);
    for (int i = 1; i <= len; i++) {
        x += dx; y += dy; z += dz;
        dy -= droop;
        if ((i & 1) == 0) { dx += (rf(g) - 0.5f) * 0.3f; dz += (rf(g) - 0.5f) * 0.3f; }
        int cx = iround(x), cy = iround(y), cz = iround(z);
        if (cy > g->ycap) { cy = g->ycap; y = (float)cy; dy = 0.0f; }
        if (cy < 0) { cy = 0; y = 0.0f; dy = 0.0f; }
        line6(g, px, py, pz, cx, cy, cz, TV_LOG);
        px = cx; py = cy; pz = cz;
        if (depth < 2 && i == len / 2 && len >= 5 && rf(g) < g->sp->fork_chance * 0.5f + 0.2f) {
            int a = (rf(g) < 0.5f ? 1 : -1) * (25 + ri(g, 0, 20));
            float c = cos_deg(a), s = sin_deg(a);
            grow_branch(g, x, y, z, dx * c - dz * s, dy + 0.15f, dx * s + dz * c, (len - i) * 3 / 4, depth + 1, droop);
        }
    }
    add_anchor(g, px, py, pz);
}

static void make_branches(TG *g, float start_frac, float droop) {
    int nb = g->nb;
    if (nb <= 0) return;
    int y0 = (int)ceilf((float)g->H * start_frac), y1 = g->H - 2;
    y0 = CLAMP(y0, 1, MAX(1, g->H - 1));
    if (y1 < y0) y1 = y0;
    int a0 = ri(g, 0, 359);
    for (int i = 0; i < nb; i++) {
        float t = ((float)i + rf(g)) / (float)nb;
        int yb = CLAMP(y0 + (int)((float)(y1 - y0) * t + 0.5f), 1, MAX(1, g->H - 1)), az = a0 + i * 137 + ri(g, -20, 20);
        int ang = iround(rr(g, g->sp->branch_angle));
        int len = MAX(2, iround(rr(g, g->sp->branch_length_ratio) * (float)g->H * (1.0f - 0.45f * (float)yb / (float)g->H)));
        float sa = sin_deg(ang);
        grow_branch(g, (float)g->cx[yb], (float)yb, (float)g->cz[yb], sa * cos_deg(az), cos_deg(ang), sa * sin_deg(az), len, 0, droop);
    }
}

/* ----------------------------------------------------------------------- canopies */

static void canopy_irregular(TG *g, bool clustered) {
    float R = g->cr, fy = g->flat;
    int tx = g->cx[g->H - 1], tz = g->cz[g->H - 1], cy = g->H - 1 + iround(R * fy * 0.3f);
    if (!clustered || g->n_anchor == 0) {
        leaf_blob(g, tx, cy, tz, R, R * fy, R, g->cd);
        int nl = 2 + ri(g, 0, 2);
        for (int l = 0; l < nl; l++) {
            int a = ri(g, 0, 359);
            float dist = R * (0.45f + 0.25f * rf(g)), dy = (rf(g) - 0.5f) * 0.7f * R * fy, r = R * (0.5f + 0.3f * rf(g));
            leaf_blob(g, tx + iround(dist * cos_deg(a)), cy + iround(dy), tz + iround(dist * sin_deg(a)), r, r * fy, r, g->cd);
        }
    } else leaf_blob(g, tx, cy, tz, R * 0.55f, R * 0.55f * fy, R * 0.55f, g->cd);
    for (int i = 0; i < g->n_anchor; i++) {
        float r = R * ((clustered ? 0.55f : 0.4f) + (clustered ? 0.3f : 0.25f) * rf(g));
        leaf_blob(g, g->ax[i], g->ay[i] + iround(r * 0.3f), g->az[i], r, r * fy, r, g->cd);
    }
}

static void canopy_conical(TG *g) {
    int start = MAX(1, (int)((float)g->H * 0.18f)), top = g->H + 1, span = MAX(1, top - start);
    for (int y = start; y <= top; y++) {
        float u = (float)(y - start) / (float)span, r = g->cr * (1.0f - u) + 0.6f;
        if ((y - start) % 3 == 0) r += 0.9f;
        int ci = MIN(y, g->H - 1);
        leaf_disc(g, g->cx[ci], y, g->cz[ci], r, g->cd);
    }
}

static void canopy_layered(TG *g) {
    int start = (int)((float)g->H * 0.45f), top = g->H;
    int arms = MAX(2, g->nb / 2);
    for (int y = start; y <= top; y += 3) {
        float u = (float)(y - start) / (float)MAX(1, top - start + 1), rw = g->cr * (0.45f + 0.55f * (1.0f - u)) * (0.75f + 0.5f * rf(g));
        int ci = MIN(y, g->H - 1);
        leaf_disc(g, g->cx[ci], y, g->cz[ci], rw, g->cd);
        leaf_disc(g, g->cx[ci], MIN(y + 1, top + 1), g->cz[ci], rw * 0.6f, g->cd);
        int a0 = ri(g, 0, 359);
        for (int k = 0; k < arms; k++) {
            int a = a0 + k * 360 / arms, l = MAX(1, (int)(rw * 0.6f));
            line6(g, g->cx[ci], y, g->cz[ci], g->cx[ci] + iround((float)l * cos_deg(a)), y, g->cz[ci] + iround((float)l * sin_deg(a)), TV_LOG);
        }
    }
    int ct = g->H - 1;
    leaf_disc(g, g->cx[ct], g->H, g->cz[ct], 1.6f, g->cd);
    leaf_disc(g, g->cx[ct], g->H + 1, g->cz[ct], 0.9f, 1.0f);
}

static void canopy_umbrella(TG *g) {
    if (g->n_anchor == 0) add_anchor(g, g->cx[g->H - 1], g->H - 1, g->cz[g->H - 1]);
    float shrink = g->n_anchor > 3 ? 0.8f : 1.0f;
    for (int i = 0; i < g->n_anchor; i++) {
        float ra = g->cr * (0.65f + 0.35f * rf(g)) * shrink;
        leaf_disc(g, g->ax[i], g->ay[i] + 1, g->az[i], ra, g->cd);
        leaf_disc(g, g->ax[i], g->ay[i], g->az[i], ra * 0.7f, g->cd);
        leaf_disc(g, g->ax[i], g->ay[i] + 2, g->az[i], ra * 0.4f, g->cd);
    }
}

static void canopy_drooping(TG *g) {
    float R = g->cr, fy = g->flat;
    int tx = g->cx[g->H - 1], tz = g->cz[g->H - 1], cy = g->H - 1 + iround(R * fy * 0.2f);
    leaf_blob(g, tx, cy, tz, R * 0.8f, R * 0.8f * fy, R * 0.8f, g->cd);
    for (int i = 0; i < g->n_anchor; i++) {
        float r = R * (0.5f + 0.2f * rf(g));
        leaf_blob(g, g->ax[i], g->ay[i], g->az[i], r, r * fy, r, g->cd);
    }
    int snap = g_ntouch;
    for (int t = 0; t < snap; t++) {
        int idx = g_touch[t], x = idx % GX - TREE_REACH, z = (idx / GX) % GZ - TREE_REACH, y = idx / (GX * GZ) - TREE_BELOW;
        if ((g_grid[idx] & 0x0F) != TV_LEAF || cell_get(x, y - 1, z) != 0) continue;
        int dx = x - tx, dz = z - tz;
        if ((float)(dx * dx + dz * dz) < 0.2f * R * R || rf(g) >= 0.4f) continue;
        int len = ri(g, 2, 2 + (int)(R * 1.3f));
        for (int k = 1; k <= len && cell_get(x, y - k, z) == 0; k++) cell_set(g, x, y - k, z, TV_LEAF);
    }
}

static void canopy_frond(TG *g) {
    int n = MAX(3, g->nb), tx = g->cx[g->H - 1], ty = g->H - 1, tz = g->cz[g->H - 1], Lf = MAX(3, iround(g->cr * 1.3f)), a0 = ri(g, 0, 359);
    for (int i = 0; i < n; i++) {
        int a = a0 + i * 360 / n + ri(g, -15, 15);
        float c = cos_deg(a), s = sin_deg(a), up = 0.9f + 0.3f * rf(g), b = up / (0.7f * (float)Lf + 1.0f);
        int px = tx, py = ty, pz = tz;
        for (int t = 1; t <= Lf; t++) {
            int x = tx + iround(c * (float)t), z = tz + iround(s * (float)t), y = ty + iround(up * (float)t - b * (float)(t * t));
            line6(g, px, py, pz, x, y, z, TV_LEAF);
            if (t >= 2 && rf(g) < 0.75f * g->cd + 0.2f) {
                int sx = fabsf(c) > fabsf(s) ? 0 : 1, sz = 1 - sx;
                cell_set(g, x + sx, y, z + sz, TV_LEAF);
                if (rf(g) < 0.6f) cell_set(g, x - sx, y, z - sz, TV_LEAF);
            }
            px = x; py = y; pz = z;
        }
        cell_set(g, px, py - 1, pz, TV_LEAF);
    }
}

static void canopy(TG *g) {
    switch (g->sp->canopy) {
    case CS_IRREGULAR_SPHERE: canopy_irregular(g, false); break;
    case CS_CLUSTERED: canopy_irregular(g, true); break;
    case CS_CONICAL: canopy_conical(g); break;
    case CS_LAYERED: canopy_layered(g); break;
    case CS_UMBRELLA: canopy_umbrella(g); break;
    case CS_DROOPING: canopy_drooping(g); break;
    case CS_FROND: canopy_frond(g); break;
    default: break;
    }
}

/* ---------------------------------------------------------------- one tree, by type */

static void shape_fallen_log(TG *g) {
    int L = MAX(2, g->H), a = ri(g, 0, 359), rise = rf(g) < g->lean ? 1 : 0;
    float hl = (float)L * 0.5f;
    int ex = iround(hl * cos_deg(a)), ez = iround(hl * sin_deg(a));
    float r = MAX(g->tr, 0.5f);
    line6(g, 0, 0, 0, ex, rise, ez, TV_FALLEN);
    line6(g, 0, 0, 0, -ex, 0, -ez, TV_FALLEN);
    if (r >= 1.0f) {
        int snap = g_ntouch;
        for (int t = 0; t < snap; t++) {
            int idx = g_touch[t], x = idx % GX - TREE_REACH, z = (idx / GX) % GZ - TREE_REACH, y = idx / (GX * GZ) - TREE_BELOW;
            disc(g, x, y, z, r, TV_FALLEN);
        }
    }
    g->H = 1;
}

static void shape_snag(TG *g) {
    build_trunk(g, g->H);
    root_flare(g);
    int nb = g->nb;
    int a0 = ri(g, 0, 359);
    for (int i = 0; i < nb; i++) {
        int yb = CLAMP((int)((float)g->H * (0.35f + 0.5f * rf(g))), 1, MAX(1, g->H - 1)), a = a0 + i * 120 + ri(g, -30, 30), ang = iround(rr(g, g->sp->branch_angle));
        int len = CLAMP(iround(rr(g, g->sp->branch_length_ratio) * (float)g->H), 1, 3);
        float sa = sin_deg(ang);
        grow_branch(g, (float)g->cx[yb], (float)yb, (float)g->cz[yb], sa * cos_deg(a), cos_deg(ang), sa * sin_deg(a), len, 2, 0.0f);
    }
    if (g->H > 3 && rf(g) < 0.6f) { /* a splintered top: one extra stub beside the broken end */
        int a = ri(g, 0, 359);
        cell_set(g, g->cx[g->H - 1] + iround(cos_deg(a)), MAX(0, g->H - 2), g->cz[g->H - 1] + iround(sin_deg(a)), TV_LOG);
    }
}

static void shape_mangrove(TG *g) {
    build_trunk(g, g->H);
    int n = 3 + (int)(g->flare * 4.0f + 0.5f), a0 = ri(g, 0, 359);
    for (int i = 0; i < n; i++) {
        int a = a0 + i * 360 / n + ri(g, -20, 20), yr = 1 + ri(g, 1, MAX(1, g->H / 2)), L = ri(g, 2, 4);
        float c = cos_deg(a), s = sin_deg(a);
        int mx = iround(0.7f * (float)L * c), mz = iround(0.7f * (float)L * s), ex = iround((float)L * c), ez = iround((float)L * s);
        line6(g, g->cx[MIN(yr, g->H - 1)], MIN(yr, g->H - 1), g->cz[MIN(yr, g->H - 1)], g->cx[MIN(yr, g->H - 1)] + mx, MIN(yr, g->H - 1), g->cz[MIN(yr, g->H - 1)] + mz, TV_LOG);
        line6(g, g->cx[MIN(yr, g->H - 1)] + mx, MIN(yr, g->H - 1), g->cz[MIN(yr, g->H - 1)] + mz, g->cx[0] + ex, 0, g->cz[0] + ez, TV_LOG);
        cell_set(g, g->cx[0] + ex, 0, g->cz[0] + ez, TV_ROOT);
    }
    make_branches(g, 0.5f, 0.0f);
    canopy(g);
}

static void shape_acacia(TG *g) {
    int hf = MAX(2, iround((float)g->H * 0.55f)), H = g->H;
    g->H = hf;
    build_trunk(g, hf);
    root_flare(g);
    int n = MAX(2, g->nb), a0 = ri(g, 0, 359);
    for (int i = 0; i < n; i++) {
        int a = a0 + i * 360 / n + ri(g, -25, 25), ang = iround(rr(g, g->sp->branch_angle));
        float ca = MAX(cos_deg(ang), 0.3f), sa = sin_deg(ang);
        int len = (int)ceilf((float)(H - 1 - hf) / ca) + 1;
        len = MAX(len, 3);
        grow_branch(g, (float)g->cx[hf - 1], (float)(hf - 1), (float)g->cz[hf - 1], sa * cos_deg(a), ca, sa * sin_deg(a), len, 2, 0.0f);
    }
    g->H = H;
    /* canopy() reads the trunk top for single-umbrella fallback only; the anchors carry the real crowns. */
    g->cx[H - 1] = g->cx[hf - 1];
    g->cz[H - 1] = g->cz[hf - 1];
    canopy(g);
}

static void shape_generic(TG *g) {
    TreeShapeType t = g->sp->type;
    build_trunk(g, g->H);
    root_flare(g);
    buttress_fins(g);
    if (t == TS_DECIDUOUS_BROADLEAF || t == TS_DECIDUOUS_SLENDER || t == TS_CUSTOM || t == TS_WILLOW) {
        if (g->H >= 6 && rf(g) < g->sp->fork_chance) {
            int yf = MAX(2, iround((float)g->H * 0.55f)), a = ri(g, 0, 359), ang = 15 + ri(g, 0, 10);
            float sa = sin_deg(ang);
            grow_branch(g, (float)g->cx[yf], (float)yf, (float)g->cz[yf], sa * cos_deg(a), cos_deg(ang), sa * sin_deg(a), g->H - yf, 2, 0.0f);
        }
        make_branches(g, t == TS_DECIDUOUS_SLENDER ? 0.5f : 0.4f, t == TS_WILLOW ? 0.05f : 0.0f);
    }
    canopy(g);
}

/* ----------------------------------------------------------------- pruning and output */

static void prune_and_emit(TG *g, TreeShape *out) {
    static const int D[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    /* 1: logs not joined to the root cell are dropped. */
    int head = 0, tail = 0;
    int origin = gidx(0, 0, 0);
    if ((g_grid[origin] & 0x0F) >= TV_LOG) { g_grid[origin] |= 0x20; g_queue[tail++] = origin; }
    while (head < tail) {
        int idx = g_queue[head++], x = idx % GX - TREE_REACH, z = (idx / GX) % GZ - TREE_REACH, y = idx / (GX * GZ) - TREE_BELOW;
        for (int k = 0; k < 6; k++) {
            int nx = x + D[k][0], ny = y + D[k][1], nz = z + D[k][2];
            if (cell_get(nx, ny, nz) < TV_LOG) continue;
            int ni = gidx(nx, ny, nz);
            if (g_grid[ni] & 0x20) continue;
            g_grid[ni] |= 0x20;
            if (tail < CELL_MAX) g_queue[tail++] = ni;
        }
    }
    for (int t = 0; t < g_ntouch; t++) {
        u8 *c = &g_grid[g_touch[t]];
        if ((*c & 0x0F) >= TV_LOG && !(*c & 0x20)) *c = 0;
    }
    /* 2: leaves that cannot walk to a log through other leaves are dropped, so nothing floats. */
    head = tail = 0;
    for (int t = 0; t < g_ntouch; t++)
        if (g_grid[g_touch[t]] & 0x20) g_queue[tail++] = g_touch[t];
    while (head < tail) {
        int idx = g_queue[head++], x = idx % GX - TREE_REACH, z = (idx / GX) % GZ - TREE_REACH, y = idx / (GX * GZ) - TREE_BELOW;
        for (int k = 0; k < 6; k++) {
            int nx = x + D[k][0], ny = y + D[k][1], nz = z + D[k][2];
            if (cell_get(nx, ny, nz) != TV_LEAF) continue;
            int ni = gidx(nx, ny, nz);
            if (g_grid[ni] & 0x10) continue;
            g_grid[ni] |= 0x10;
            if (tail < CELL_MAX) g_queue[tail++] = ni;
        }
    }
    memset(out, 0, sizeof *out);
    out->min_x = out->min_y = out->min_z = 127;
    out->max_x = out->max_y = out->max_z = -127;
    int top_log = -1000;
    for (int t = 0; t < g_ntouch; t++) {
        u8 v = g_grid[g_touch[t]];
        u8 kind = v & 0x0F;
        if (kind == 0) continue;
        if (kind == TV_LEAF && !(v & 0x10)) { g_grid[g_touch[t]] = 0; continue; }
        int idx = g_touch[t], x = idx % GX - TREE_REACH, z = (idx / GX) % GZ - TREE_REACH, y = idx / (GX * GZ) - TREE_BELOW;
        g_vox[out->n++] = (TreeVoxel){(i8)x, (i8)y, (i8)z, kind};
        out->min_x = MIN(out->min_x, x); out->max_x = MAX(out->max_x, x);
        out->min_y = MIN(out->min_y, y); out->max_y = MAX(out->max_y, y);
        out->min_z = MIN(out->min_z, z); out->max_z = MAX(out->max_z, z);
        if (kind == TV_LEAF) out->leaves++;
        else { out->logs++; top_log = MAX(top_log, y); }
    }
    out->v = g_vox;
    out->measured_height = top_log + 1;
    out->clipped = g->clipped;
}

u64 tree_seed(u64 world_seed, int gx, int gz, int layer) { return hash3((i64)(world_seed ^ 0x7EE50003ull), gx, layer, gz); }

const TreeShape *tree_generate(const TreeSpecies *sp, u64 seed) {
    for (int t = 0; t < g_ntouch; t++) g_grid[g_touch[t]] = 0;
    g_ntouch = 0;
    TG g;
    memset(&g, 0, sizeof g);
    g.sp = sp;
    g.seed = seed;
    g.rng.s = seed ^ 0x5851F42D4C957F2Dull;
    g.reach = sp->reach;
    g.H = CLAMP(iround(rr(&g, sp->height)), 1, TREE_MAX_HEIGHT);
    g.tr = rr(&g, sp->trunk_radius);
    g.lean = rr(&g, sp->trunk_lean);
    g.taper = rr(&g, sp->trunk_taper);
    g.curve = rr(&g, sp->trunk_curve);
    g.nb = iround(rr(&g, sp->branch_count));
    g.cr = rr(&g, sp->canopy_radius);
    g.cd = rr(&g, sp->canopy_density);
    g.flat = rr(&g, sp->canopy_flatness);
    g.flare = rr(&g, sp->root_flare);
    g.butt = rr(&g, sp->buttress);
    g.ycap = g.H - 1;
    TreeShape *out = &g_shape;
    int H = g.H;
    switch (sp->type) {
    case TS_FALLEN_LOG: shape_fallen_log(&g); break;
    case TS_DEAD_SNAG: shape_snag(&g); break;
    case TS_MANGROVE: shape_mangrove(&g); break;
    case TS_ACACIA: shape_acacia(&g); break;
    default: shape_generic(&g); break;
    }
    prune_and_emit(&g, out);
    out->height = H;
    out->branch_count = g.nb;
    out->trunk_radius = g.tr;
    out->lean = g.lean;
    out->canopy_radius = g.cr;
    out->canopy_density = g.cd;
    out->root_flare = g.flare;
    return out;
}

/* A tree is built by every chunk it overhangs, so recent shapes are kept per thread. Shapes are pure in (species, seed),
 * which makes a hit exactly equal to a rebuild. */
#define SHAPE_CACHE 256
typedef struct ShapeEnt { u64 seed, rev; int species; TreeShape sh; TreeVoxel *v; size_t cap; } ShapeEnt;
static _Thread_local ShapeEnt g_shape_cache[SHAPE_CACHE];

const TreeShape *tree_generate_cached(const TreeSpecies *sp, u64 seed) {
    ShapeEnt *e = &g_shape_cache[hash64(seed ^ (u64)(sp - g_sp) * 0x9E3779B97F4A7C15ull) & (SHAPE_CACHE - 1)];
    int si = (int)(sp - g_sp);
    if (e->rev == g_rev && e->seed == seed && e->species == si && e->v) return &e->sh;
    const TreeShape *sh = tree_generate(sp, seed);
    if (e->cap < (size_t)sh->n) { e->cap = (size_t)sh->n + 64; e->v = xrealloc(e->v, e->cap * sizeof *e->v); }
    memcpy(e->v, sh->v, (size_t)sh->n * sizeof *e->v);
    e->sh = *sh;
    e->sh.v = e->v;
    e->seed = seed; e->rev = g_rev; e->species = si;
    return &e->sh;
}

/* ---------------------------------------------------------------------- placement */

typedef struct Site { int x, z, gx, gz, ground; i16 species; u32 prio; } Site;
#define SITE_UNKNOWN (-2)
#define SITE_NONE (-1)

#define SITE_CACHE 8192 /* direct mapped; a site is evaluated by every chunk near it, so neighbours reuse the answer */
typedef struct CacheEnt { Site s; u64 tag; } CacheEnt;

struct TreeScratch {
    Site *memo[TREE_LAYERS];
    size_t cap[TREE_LAYERS];
    CacheEnt *cache[TREE_LAYERS];
};


static const int CELL[TREE_LAYERS] = {6, 8};
static const u64 SALT_SITE[TREE_LAYERS] = {0x51a7e0ull, 0x51a7e1ull};
static u64 g_sites_evaluated;
u64 trees_sites_evaluated(void) { return g_sites_evaluated; }

TreeScratch *trees_scratch_create(void) { return xcalloc(1, sizeof(TreeScratch)); }
void trees_scratch_destroy(TreeScratch *ts) {
    if (!ts) return;
    for (int l = 0; l < TREE_LAYERS; l++) { free(ts->memo[l]); free(ts->cache[l]); }
    free(ts);
}
void trees_plan_free(TreePlan *p) { free(p->t); memset(p, 0, sizeof *p); }

static void plan_push(TreePlan *p, const TreeInst *t) {
    if (p->n >= p->cap) { p->cap = p->cap ? p->cap * 2 : 64; p->t = xrealloc(p->t, (size_t)p->cap * sizeof *p->t); }
    p->t[p->n++] = *t;
}

static float vnoise(u64 seed, u64 salt, float x, float z, float scale) {
    float fx = x / scale, fz = z / scale;
    int ix = (int)floorf(fx), iz = (int)floorf(fz);
    float tx = fx - (float)ix, tz = fz - (float)iz;
    tx = tx * tx * (3.0f - 2.0f * tx);
    tz = tz * tz * (3.0f - 2.0f * tz);
    u64 s = seed ^ salt;
    float a = hash_to_unit(hash3((i64)s, ix, 0, iz)), b = hash_to_unit(hash3((i64)s, ix + 1, 0, iz));
    float c = hash_to_unit(hash3((i64)s, ix, 0, iz + 1)), d = hash_to_unit(hash3((i64)s, ix + 1, 0, iz + 1));
    float top = a + (b - a) * tx, bot = c + (d - c) * tx;
    return top + (bot - top) * tz;
}

static bool in_range(TRange r, float v) { return v >= r.lo && v <= r.hi; }

typedef struct PlanCtx {
    u64 seed;
    const TreeSampler *smp;
    int layer_species[TREE_LAYERS][TREE_MAX_SPECIES], layer_n[TREE_LAYERS];
    int reach_max, rad_max, clear_max;
    u64 tag;
    int gx0[TREE_LAYERS], gz0[TREE_LAYERS], w[TREE_LAYERS], h[TREE_LAYERS];
    TreeScratch *ts;
} PlanCtx;

static void eval_site(PlanCtx *pc, int L, int gx, int gz, Site *s) {
    g_sites_evaluated++;
    int cs = CELL[L];
    u64 h = hash3((i64)(pc->seed ^ SALT_SITE[L]), gx, L, gz);
    s->gx = gx; s->gz = gz;
    s->x = gx * cs + (int)(h % (u64)cs);
    s->z = gz * cs + (int)((h >> 20) % (u64)cs);
    s->prio = (u32)(h >> 32);
    s->species = SITE_NONE;
    TreeColumn col;
    memset(&col, 0, sizeof col);
    pc->smp->column(pc->smp->ctx, s->x, s->z, &col);
    s->ground = col.ground;
    if (!col.ok || col.blocked || col.alpine || col.scale <= 0.0f) return;
    float cover = 0.65f * vnoise(pc->seed, 0xC0FE01ull, (float)s->x, (float)s->z, 96.0f) + 0.35f * vnoise(pc->seed, 0xC0FE02ull, (float)s->x, (float)s->z, 24.0f);
    float light = 1.0f - cover;
    float region = 0.55f + 0.9f * vnoise(pc->seed, 0xDE5171ull, (float)s->x, (float)s->z, 256.0f);
    float stage = vnoise(pc->seed, 0x5ACCE5ull, (float)s->x, (float)s->z, 192.0f);
    int cand[TREE_MAX_SPECIES], nc = 0;
    float q[TREE_MAX_SPECIES];
    bool detailed = false;
    for (int k = 0; k < pc->layer_n[L]; k++) {
        int si = pc->layer_species[L][k];
        const TreeSpecies *sp = &g_sp[si];
        if (sp->kernel > pc->smp->kernel) continue;
        if (col.biome < 0 || col.biome >= TREE_BIOME_SLOTS || !sp->biome_ok[col.biome]) continue;
        if (!in_range(sp->temperature, col.temperature) || !in_range(sp->moisture, col.moisture) || !in_range(sp->light, light)) continue;
        if (col.ground < sp->min_y || col.ground > sp->max_y) continue;
        float dist = col.wet ? 0.0f : col.water_dist;
        if (col.wet) { if (!sp->allow_shallow || col.water_depth > 2.0f) continue; }
        else if (col.ground_y <= (float)pc->smp->sea_level + 0.5f && !sp->allow_shallow) continue;
        if (dist < sp->water_min && !col.wet) continue;
        if (dist > sp->water_max) continue;
        if (!detailed) { pc->smp->detail(pc->smp->ctx, s->x, s->z, &col); detailed = true; }
        if (col.slope > sp->max_slope) continue;
        if (sp->substrate_count > 0) {
            bool ok = false;
            for (int u = 0; u < sp->substrate_count && !ok; u++) ok = sp->substrate[u] == col.surface;
            if (!ok) continue;
        }
        float w = sp->density * region * col.scale;
        if (sp->clustering > 0.0f) {
            float grove = vnoise(pc->seed, 0x6a0e00ull + (u64)sp->salt, (float)s->x, (float)s->z, 8.0f * (float)sp->cluster_size);
            w *= (1.0f - sp->clustering) + sp->clustering * 2.0f * grove;
        }
        w *= sp->succession == 0 ? 1.4f - 1.05f * stage : (sp->succession == 2 ? 0.4f + 1.0f * stage : 1.0f);
        if (sp->prefer_near) w *= dist < 8.0f ? 1.6f : 0.5f;
        if (w <= 0.0f) continue;
        cand[nc] = si; q[nc] = w; nc++;
    }
    if (!nc) return;
    float total = 0.0f;
    for (int k = 0; k < nc; k++) total += q[k];
    float u = hash_to_unit(hash3((i64)(pc->seed ^ 0x91c4ull), gx, L, gz)), acc = 0.0f;
    float norm = total > 1.0f ? 1.0f / total : 1.0f;
    for (int k = 0; k < nc; k++) {
        acc += q[k] * norm;
        if (u < acc) { s->species = (i16)cand[k]; return; }
    }
}

static Site *site_get(PlanCtx *pc, int L, int gx, int gz) {
    int ix = gx - pc->gx0[L], iz = gz - pc->gz0[L];
    Site *s = &pc->ts->memo[L][(size_t)iz * (size_t)pc->w[L] + (size_t)ix];
    if (s->species != SITE_UNKNOWN) return s;
    u64 tag = pc->tag;
    if (!tag) { eval_site(pc, L, gx, gz, s); return s; }
    if (!pc->ts->cache[L]) pc->ts->cache[L] = xcalloc(SITE_CACHE, sizeof(CacheEnt));
    CacheEnt *e = &pc->ts->cache[L][hash3((i64)tag, gx, L, gz) & (SITE_CACHE - 1)];
    if (e->tag != tag || e->s.gx != gx || e->s.gz != gz) { eval_site(pc, L, gx, gz, s); e->s = *s; e->tag = tag; }
    else *s = e->s;
    return s;
}

static bool outranks(const Site *b, const Site *a) {
    if (b->prio != a->prio) return b->prio > a->prio;
    if (b->gz != a->gz) return b->gz > a->gz;
    return b->gx > a->gx;
}

static bool survives(PlanCtx *pc, int L, const Site *a) {
    const TreeSpecies *sa = &g_sp[a->species];
    int cs = CELL[L], rc = pc->rad_max;
    for (int gz = fdiv(a->z - rc, cs); gz <= fdiv(a->z + rc, cs); gz++)
        for (int gx = fdiv(a->x - rc, cs); gx <= fdiv(a->x + rc, cs); gx++) {
            if (gx == a->gx && gz == a->gz) continue;
            const Site *b = site_get(pc, L, gx, gz);
            if (b->species < 0 || !outranks(b, a)) continue;
            int r = MAX(sa->min_radius, g_sp[b->species].min_radius), dx = b->x - a->x, dz = b->z - a->z;
            if (dx * dx + dz * dz < r * r) return false;
        }
    if (L == 1) { /* understory stays out of a canopy tree's trunk */
        int c0 = CELL[0], rc0 = pc->clear_max;
        for (int gz = fdiv(a->z - rc0, c0); gz <= fdiv(a->z + rc0, c0); gz++)
            for (int gx = fdiv(a->x - rc0, c0); gx <= fdiv(a->x + rc0, c0); gx++) {
                const Site *b = site_get(pc, 0, gx, gz);
                if (b->species < 0) continue;
                float clear = 1.5f + g_sp[b->species].trunk_radius.hi;
                int dx = b->x - a->x, dz = b->z - a->z;
                if ((float)(dx * dx + dz * dz) < clear * clear) return false;
            }
    }
    return true;
}

void trees_plan(TreeScratch *ts, u64 seed, const TreeSampler *smp, int x0, int z0, int x1, int z1, TreePlan *out) {
    out->n = 0;
    if (!g_n) return;
    PlanCtx pc;
    memset(&pc, 0, sizeof pc);
    pc.seed = seed; pc.smp = smp; pc.ts = ts;
    pc.tag = smp->tag ? (hash64(smp->tag ^ seed) ^ hash64(g_rev)) | 1u : 0;
    pc.rad_max = 1;
    for (int i = 0; i < g_n; i++) {
        const TreeSpecies *sp = &g_sp[i];
        pc.layer_species[sp->layer][pc.layer_n[sp->layer]++] = i;
        pc.reach_max = MAX(pc.reach_max, sp->reach);
        pc.rad_max = MAX(pc.rad_max, sp->min_radius);
        pc.clear_max = MAX(pc.clear_max, (int)ceilf(1.5f + sp->trunk_radius.hi));
    }
    int margin = pc.reach_max + MAX(pc.rad_max, pc.clear_max) + 1;
    for (int L = 0; L < TREE_LAYERS; L++) {
        int cs = CELL[L];
        pc.gx0[L] = fdiv(x0 - margin, cs) - 1;
        pc.gz0[L] = fdiv(z0 - margin, cs) - 1;
        pc.w[L] = fdiv(x1 + margin, cs) + 2 - pc.gx0[L];
        pc.h[L] = fdiv(z1 + margin, cs) + 2 - pc.gz0[L];
        size_t cells = (size_t)pc.w[L] * (size_t)pc.h[L];
        if (cells > ts->cap[L]) { ts->memo[L] = xrealloc(ts->memo[L], cells * sizeof(Site)); ts->cap[L] = cells; }
        for (size_t i = 0; i < cells; i++) ts->memo[L][i].species = SITE_UNKNOWN;
    }
    for (int L = 0; L < TREE_LAYERS; L++) {
        if (!pc.layer_n[L]) continue;
        int cs = CELL[L];
        for (int gz = fdiv(z0 - pc.reach_max, cs); gz <= fdiv(z1 + pc.reach_max, cs); gz++)
            for (int gx = fdiv(x0 - pc.reach_max, cs); gx <= fdiv(x1 + pc.reach_max, cs); gx++) {
                const Site *s = site_get(&pc, L, gx, gz);
                if (s->species < 0) continue;
                int reach = g_sp[s->species].reach;
                if (s->x < x0 - reach || s->x > x1 + reach || s->z < z0 - reach || s->z > z1 + reach) continue;
                if (!survives(&pc, L, s)) continue;
                TreeInst t = {s->x, s->z, s->ground, gx, gz, (u16)s->species, (u8)L};
                plan_push(out, &t);
            }
    }
}
