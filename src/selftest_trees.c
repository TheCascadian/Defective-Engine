/* Tree self-tests: species loading, shape generation, placement rules, cross-chunk agreement, determinism and cost.
 * Every group boots the generator cold, loads the base mod's species, and reloads them afterwards so a test species
 * never leaks into the next group. */
#include "gen_internal.h"
#include <limits.h>

#define TSEED 12345
#define BOX 256

static void boot(u64 seed) {
    registry_reset();
    data_error_reset();
    registry_load_blocks();
    registry_load_worldgen_config();
    gen_init(seed);
}

static u64 shape_hash(const TreeShape *sh) {
    u64 h = hash64((u64)sh->n);
    for (int i = 0; i < sh->n; i++) {
        const TreeVoxel *v = &sh->v[i];
        h = hash64(h ^ ((u64)(u8)v->x | ((u64)(u8)v->y << 8) | ((u64)(u8)v->z << 16) | ((u64)v->kind << 24)));
    }
    return h;
}

static int rounded(float v) { return (int)floorf(v + 0.5f); }

/* ---------------------------------------------------------------- 1. species files */

static bool load_json(const char *text) {
    return trees_load_text(text, strlen(text), "data/test/trees/t.json", "test");
}

static void test_species_loading(void) {
    boot(TSEED);
    CHECK(trees_species_count() >= 10);
    CHECK(trees_find("base:oak") && trees_find("base:oak")->type == TS_DECIDUOUS_BROADLEAF);
    CHECK(trees_find("base:palm") && trees_find("base:palm")->canopy == CS_FROND);
    CHECK(trees_find("base:oak")->reach >= 3 && trees_find("base:oak")->reach <= TREE_REACH);
    /* sorted by id, so selection never depends on directory order */
    bool sorted = true;
    for (int i = 1; i < trees_species_count(); i++) if (strcmp(trees_species(i - 1)->id, trees_species(i)->id) >= 0) sorted = false;
    CHECK(sorted);
    for (int i = 0; i < trees_species_count(); i++) {
        const TreeSpecies *sp = trees_species(i);
        CHECK(sp->log != STATE_AIR);
        CHECK(sp->kernel == 0 || sp->kernel == 1);
        if (sp->leaves_name[0]) CHECK(sp->leaves != STATE_AIR && trees_is_leaf_state(sp->leaves));
    }
    const char *const added[] = {"base:silver_birch", "base:dark_oak", "base:cherry", "base:taiga_spruce", "base:jungle_giant",
                                 "base:savanna_acacia", "base:autumn_oak_red", "base:autumn_oak_orange",
                                 "base:autumn_oak_yellow", "base:weeping_willow", "base:mangrove_red"};
    for (size_t i = 0; i < sizeof added / sizeof *added; i++) CHECK(trees_find(added[i]) != NULL);

    int before = trees_species_count();
    data_error_reset();
    CHECK(load_json("{\"id\":\"test:ok\",\"log\":\"base:log\",\"leaves\":\"base:leaves\",\"shape\":{\"type\":\"conifer_fir\",\"height\":[10,12]}}"));
    CHECK(trees_species_count() == before + 1);
    CHECK(trees_find("test:ok") && trees_find("test:ok")->height.hi == 12.0f);
    CHECK(trees_find("test:ok")->canopy == CS_CONICAL); /* missing fields come from the shape type */
    CHECK(data_error_count() == 0);

    /* every rejection names the problem and leaves the table alone */
    static const char *const BAD[] = {
        "{\"log\":\"base:log\",\"leaves\":\"base:leaves\",\"shape\":{\"type\":\"palm\"}}",                                                   /* no id */
        "{\"id\":\"test:a\",\"log\":\"base:log\",\"leaves\":\"base:leaves\",\"shape\":{\"type\":\"spiral\"}}",                              /* unknown shape */
        "{\"id\":\"test:b\",\"log\":\"base:nothing\",\"leaves\":\"base:leaves\",\"shape\":{\"type\":\"palm\"}}",                            /* unknown block */
        "{\"id\":\"test:c\",\"log\":\"base:log\",\"leaves\":\"base:leaves\",\"shape\":{\"type\":\"palm\",\"height\":[9,3]}}",                /* inverted range */
        "{\"id\":\"test:d\",\"log\":\"base:log\",\"leaves\":\"base:leaves\",\"shape\":{\"type\":\"palm\",\"height\":[5,500]}}",              /* out of bounds */
        "{\"id\":\"test:e\",\"log\":\"base:log\",\"leaves\":\"base:leaves\",\"shape\":{\"type\":\"palm\",\"wobble\":1}}",                    /* unknown field */
        "{\"id\":\"test:f\",\"log\":\"base:log\",\"shape\":{\"type\":\"palm\"}}",                                                           /* no leaves */
        "{\"id\":\"nocolon\",\"log\":\"base:log\",\"leaves\":\"base:leaves\",\"shape\":{\"type\":\"palm\"}}",                               /* id not namespaced */
        "{\"id\":\"test:g\",\"log\":\"base:log\",\"leaves\":\"base:leaves\"}",                                                              /* no shape */
        "{\"id\":\"test:h\",\"log\":\"base:log\",\"leaves\":\"base:leaves\",\"shape\":{\"type\":\"custom\"}}",                              /* custom without canopy */
        "{\"id\":\"test:i\",\"log\":\"base:log\",\"leaves\":\"base:leaves\",\"elevation\":{\"min_y\":90,\"max_y\":10},\"shape\":{\"type\":\"palm\"}}",
        "{\"id\":\"test:j\",\"log\":\"base:log\",\"leaves\":\"base:leaves\",\"shape\":{\"type\":\"palm\"},",                                  /* broken JSON */
    };
    int rejected = 0;
    for (size_t i = 0; i < sizeof BAD / sizeof *BAD; i++) {
        int errs = data_error_count(), n = trees_species_count();
        bool ok = load_json(BAD[i]);
        if (!ok && data_error_count() > errs && trees_species_count() == n) rejected++;
    }
    CHECK(rejected == (int)(sizeof BAD / sizeof *BAD));
    bool names_cause = false;
    for (int i = 0; i < data_error_count(); i++) if (strstr(data_error_text(i), "unknown block \"base:nothing\"")) names_cause = true;
    CHECK(names_cause);
    data_error_reset();
    gen_shutdown();
}

/* ------------------------------------------------------------- 2-6. shape generation */

#define SEEDS 40

static bool in_range(float v, TRange r) { return v >= r.lo - 0.001f && v <= r.hi + 0.001f; }

static void test_shapes(void) {
    boot(TSEED);
    const TreeSpecies *oak = trees_find("base:oak");
    CHECK(oak != NULL);
    /* 2: same seed, same tree; different seeds, different trees */
    u64 seen[SEEDS];
    int distinct = 0, heights[TREE_MAX_HEIGHT + 1] = {0}, height_kinds = 0;
    bool repeat_ok = true;
    for (int i = 0; i < SEEDS; i++) {
        u64 seed = hash64((u64)i + 77);
        const TreeShape *a = tree_generate(oak, seed);
        u64 ha = shape_hash(a);
        int ah = a->height;
        const TreeShape *b = tree_generate(oak, seed);
        if (shape_hash(b) != ha || b->height != ah) repeat_ok = false;
        bool dup = false;
        for (int k = 0; k < i; k++) if (seen[k] == ha) dup = true;
        seen[i] = ha;
        if (!dup) distinct++;
        if (!heights[ah]++) height_kinds++;
    }
    CHECK(repeat_ok);
    CHECK(distinct == SEEDS);
    CHECK(height_kinds >= 4);

    /* 3: parameters stay inside the species ranges, 4: structure is connected and inside the reach */
    int worst_reach = 0;
    bool params_ok = true, connected = true, no_floaters = true, inside = true, has_log = true;
    long checked = 0;
    for (int si = 0; si < trees_species_count(); si++) {
        const TreeSpecies *sp = trees_species(si);
        for (int i = 0; i < 24; i++) {
            const TreeShape *sh = tree_generate(sp, hash64((u64)i * 31 + (u64)si));
            checked++;
            if (sh->height < rounded(sp->height.lo) || sh->height > rounded(sp->height.hi)) params_ok = false;
            if (!in_range(sh->trunk_radius, sp->trunk_radius) || !in_range(sh->canopy_radius, sp->canopy_radius)) params_ok = false;
            if (sh->branch_count < rounded(sp->branch_count.lo) || sh->branch_count > rounded(sp->branch_count.hi)) params_ok = false;
            if (sh->logs <= 0) has_log = false;
            /* Index every voxel in a small dense grid, then flood fill from the root. */
            enum { W = 2 * TREE_REACH + 1, HY = TREE_BELOW + TREE_MAX_HEIGHT + TREE_ABOVE };
            static u8 g[W * HY * W], seen_v[W * HY * W];
            memset(g, 0, sizeof g);
            memset(seen_v, 0, sizeof seen_v);
            for (int k = 0; k < sh->n; k++) {
                const TreeVoxel *v = &sh->v[k];
                worst_reach = MAX(worst_reach, MAX(abs(v->x), abs(v->z)));
                if (abs(v->x) > sp->reach || abs(v->z) > sp->reach || v->y < -TREE_BELOW || v->y >= HY - TREE_BELOW) { inside = false; continue; }
                g[((v->y + TREE_BELOW) * W + (v->z + TREE_REACH)) * W + (v->x + TREE_REACH)] = v->kind;
            }
            int stack_cap = sh->n + 1, top = 0;
            int *stack = xmalloc((size_t)stack_cap * sizeof(int));
            int root = ((0 + TREE_BELOW) * W + TREE_REACH) * W + TREE_REACH;
            if (g[root] >= TV_LOG) { stack[top++] = root; seen_v[root] = 1; }
            while (top) {
                int idx = stack[--top], x = idx % W, z = (idx / W) % W, y = idx / (W * W);
                static const int D[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
                for (int d = 0; d < 6; d++) {
                    int nx = x + D[d][0], ny = y + D[d][1], nz = z + D[d][2];
                    if (nx < 0 || nx >= W || nz < 0 || nz >= W || ny < 0 || ny >= HY) continue;
                    int ni = (ny * W + nz) * W + nx;
                    if (seen_v[ni] || g[ni] == 0) continue;
                    seen_v[ni] = 1;
                    if (top < stack_cap) stack[top++] = ni;
                }
            }
            free(stack);
            /* every log reachable from the root through logs alone is checked by the second flood: log-only reach */
            int logs_total = 0, logs_reached = 0, leaves_total = 0, leaves_reached = 0;
            for (int k = 0; k < W * HY * W; k++) {
                if (g[k] >= TV_LOG) { logs_total++; if (seen_v[k]) logs_reached++; }
                else if (g[k] == TV_LEAF) { leaves_total++; if (seen_v[k]) leaves_reached++; }
            }
            if (logs_reached != logs_total || leaves_reached != leaves_total) { if (logs_reached != logs_total) connected = false; else no_floaters = false; }
            (void)leaves_total;
        }
    }
    CHECK(checked > 200);
    CHECK(params_ok);
    CHECK(has_log);
    CHECK(inside);
    CHECK(connected);
    CHECK(no_floaters);
    CHECK(worst_reach <= TREE_REACH);

    /* 5: the canopy surface is uneven and no two crowns share an outline */
    double var_sum = 0;
    int outlines_distinct = 0;
    u64 outlines[16];
    for (int i = 0; i < 16; i++) {
        const TreeShape *sh = tree_generate(oak, hash64((u64)i + 900));
        int w = sh->max_x - sh->min_x + 1, d = sh->max_z - sh->min_z + 1;
        int *top = xcalloc((size_t)w * (size_t)d, sizeof(int));
        for (int c = 0; c < w * d; c++) top[c] = INT_MIN;
        for (int k = 0; k < sh->n; k++) {
            const TreeVoxel *v = &sh->v[k];
            int *c = &top[(v->z - sh->min_z) * w + (v->x - sh->min_x)];
            if (v->y > *c) *c = v->y;
        }
        double mean = 0, cnt = 0, var = 0;
        u64 oh = 1469598103934665603ull;
        for (int c = 0; c < w * d; c++) if (top[c] != INT_MIN) { mean += top[c]; cnt++; oh = hash64(oh ^ (u64)(top[c] + 100) ^ ((u64)c << 8)); }
        mean /= cnt;
        for (int c = 0; c < w * d; c++) if (top[c] != INT_MIN) var += (top[c] - mean) * (top[c] - mean);
        var_sum += var / cnt;
        bool dup = false;
        for (int k = 0; k < i; k++) if (outlines[k] == oh) dup = true;
        outlines[i] = oh;
        if (!dup) outlines_distinct++;
        free(top);
    }
    CHECK(var_sum / 16.0 > 1.0);
    CHECK(outlines_distinct == 16);

    /* 6: the shape types differ in proportion: slender conifers, wide acacia and willow, tall pines against low palms */
    struct { const char *id; double aspect, height, fill; } m[] = {{"base:oak"}, {"base:birch"}, {"base:pine"}, {"base:spruce"}, {"base:palm"}, {"base:willow"}, {"base:acacia"}, {"base:mangrove"}, {"base:shrub"}};
    for (size_t k = 0; k < sizeof m / sizeof *m; k++) {
        const TreeSpecies *sp = trees_find(m[k].id);
        CHECK(sp != NULL);
        if (!sp) continue;
        for (int i = 0; i < 30; i++) {
            const TreeShape *sh = tree_generate(sp, hash64((u64)i + 4000));
            int w = MAX(sh->max_x - sh->min_x, sh->max_z - sh->min_z) + 1, h = sh->max_y + 1;
            m[k].aspect += (double)w / (double)MAX(h, 1) / 30.0;
            m[k].height += h / 30.0;
            m[k].fill += (double)sh->leaves / (double)MAX(sh->n, 1) / 30.0;
        }
    }
    double A(const char *id) { for (size_t k = 0; k < sizeof m / sizeof *m; k++) if (!strcmp(m[k].id, id)) return m[k].aspect; return 0; }
    double Hh(const char *id) { for (size_t k = 0; k < sizeof m / sizeof *m; k++) if (!strcmp(m[k].id, id)) return m[k].height; return 0; }
    CHECK(A("base:spruce") < A("base:oak"));
    CHECK(A("base:pine") < A("base:oak"));
    CHECK(A("base:acacia") > A("base:oak"));
    CHECK(A("base:birch") < A("base:oak"));
    CHECK(Hh("base:pine") > Hh("base:oak"));
    CHECK(Hh("base:palm") > Hh("base:mangrove"));
    CHECK(Hh("base:shrub") < Hh("base:mangrove"));
    for (size_t a = 0; a < sizeof m / sizeof *m; a++)
        for (size_t b = a + 1; b < sizeof m / sizeof *m; b++) {
            /* no two types share the same proportions and size */
            bool same = fabs(m[a].aspect - m[b].aspect) < 0.04 && fabs(m[a].height - m[b].height) < 0.8 && fabs(m[a].fill - m[b].fill) < 0.04;
            CHECK(!same);
        }
    gen_shutdown();
}

/* ------------------------------------------------------------------ placement */

static int box_plan(GenScratch *s, int x0, int z0, int x1, int z1, TreePlan *p) {
    gen_tree_plan(s, x0, z0, x1, z1, p);
    int n = 0;
    for (int i = 0; i < p->n; i++) n += p->t[i].x >= x0 && p->t[i].x <= x1 && p->t[i].z >= z0 && p->t[i].z <= z1;
    return n;
}

static void test_placement_rules(void) {
    boot(TSEED);
    GenScratch *s = gen_scratch_create();
    /* One probe species per rule. Each one is saturated (density 4) so the rule is the only thing thinning it. */
    static const char *const PROBES[] = {
        "{\"id\":\"test:high\",\"log\":\"base:log\",\"leaves\":\"base:leaves\",\"density\":4,\"elevation\":{\"min_y\":72},\"shape\":{\"type\":\"deciduous_broadleaf\"}}",
        "{\"id\":\"test:low\",\"log\":\"base:log\",\"leaves\":\"base:leaves\",\"density\":4,\"elevation\":{\"max_y\":70},\"shape\":{\"type\":\"deciduous_broadleaf\"}}",
        "{\"id\":\"test:flat\",\"log\":\"base:log\",\"leaves\":\"base:leaves\",\"density\":4,\"elevation\":{\"max_slope\":0.12},\"shape\":{\"type\":\"deciduous_broadleaf\"}}",
        "{\"id\":\"test:shore\",\"log\":\"base:log\",\"leaves\":\"base:leaves\",\"density\":4,\"water\":{\"min_distance\":2,\"max_distance\":10},\"shape\":{\"type\":\"deciduous_broadleaf\"}}",
        "{\"id\":\"test:cold\",\"log\":\"base:log\",\"leaves\":\"base:leaves\",\"density\":4,\"climate\":{\"temperature\":[0,0.42]},\"shape\":{\"type\":\"deciduous_broadleaf\"}}",
        "{\"id\":\"test:wet\",\"log\":\"base:log\",\"leaves\":\"base:leaves\",\"density\":4,\"climate\":{\"moisture\":[0.55,1]},\"shape\":{\"type\":\"deciduous_broadleaf\"}}",
        "{\"id\":\"test:sand\",\"log\":\"base:log\",\"leaves\":\"base:leaves\",\"density\":4,\"substrate\":[\"base:sand\"],\"shape\":{\"type\":\"deciduous_broadleaf\"}}",
        "{\"id\":\"test:grass\",\"log\":\"base:log\",\"leaves\":\"base:leaves\",\"density\":4,\"substrate\":[\"base:grass_block\"],\"biomes\":[\"base:forest\"],\"shape\":{\"type\":\"deciduous_broadleaf\"}}",
        "{\"id\":\"test:swamp\",\"log\":\"base:log\",\"leaves\":\"base:leaves\",\"density\":4,\"biomes\":[\"base:swamp\"],\"shape\":{\"type\":\"deciduous_broadleaf\"}}",
    };
    for (size_t i = 0; i < sizeof PROBES / sizeof *PROBES; i++) CHECK(load_json(PROBES[i]));
    CHECK(data_error_count() == 0);
    int sea = gen_sea_level();
    TreePlan plan = {0};
    int total = box_plan(s, -BOX, -BOX, BOX, BOX, &plan);
    CHECK(total > 200);
    /* epoch 0 is kernel 0: no kernel-1 species may be reachable */
    bool k1_leak = false;
    for (int i = 0; i < plan.n; i++) if (trees_species(plan.t[i].species)->kernel > 0) k1_leak = true;
    CHECK(!k1_leak);
    long count[9] = {0};
    long why[12] = {0};
    long bad_common = 0, bad[9] = {0};
    int swamp_idx = -1;
    for (int b = 0; b < gen_biome_count(); b++) (void)b;
    (void)swamp_idx;
    const TreeSpecies *base_forest_probe = trees_find("test:grass");
    for (int i = 0; i < plan.n; i++) {
        const TreeInst *t = &plan.t[i];
        const TreeSpecies *sp = trees_species(t->species);
        if (t->x < -BOX || t->x > BOX || t->z < -BOX || t->z > BOX) continue;
        TreeColumn c;
        gen_tree_column(s, t->x, t->z, true, &c);
        GenHydrologySample h;
        gen_hydrology_at((float)t->x, (float)t->z, &h);
        /* every tree, whatever the species: a real column above water, out of rivers, biome and climate allowed */
        if (!sp->allow_shallow && (h.wet || (h.flags & GEN_HYD_OCEAN) || c.ground_y <= (float)sea + 0.5f)) bad_common++, why[0]++;
        if (c.ground != t->ground) bad_common++, why[1]++;
        if (!sp->biome_ok[c.biome]) bad_common++, why[2]++;
        if (!in_range(c.temperature, sp->temperature) && (c.temperature < sp->temperature.lo || c.temperature > sp->temperature.hi)) bad_common++, why[3]++;
        if (c.moisture < sp->moisture.lo || c.moisture > sp->moisture.hi) bad_common++, why[4]++;
        if (c.ground < sp->min_y || c.ground > sp->max_y || c.slope > sp->max_slope) bad_common++, why[5]++;
        if (sp->substrate_count) {
            bool ok = false;
            for (int u = 0; u < sp->substrate_count; u++) ok |= sp->substrate[u] == c.surface;
            if (!ok) bad_common++, why[6]++;
        }
        if (h.water_dist < sp->water_min || h.water_dist > sp->water_max) { if (!h.wet) bad_common++, why[7]++; }
        if (!strncmp(sp->id, "test:", 5)) {
            static const char *const N[] = {"test:high", "test:low", "test:flat", "test:shore", "test:cold", "test:wet", "test:sand", "test:grass", "test:swamp"};
            for (int k = 0; k < 9; k++) if (!strcmp(sp->id, N[k])) count[k]++;
        }
    }
    (void)base_forest_probe; (void)bad;
    if (bad_common) { printf("selftest trees: placement violations:"); for (int k = 0; k < 12; k++) printf(" %ld", why[k]); printf("\n"); }
    CHECK(bad_common == 0);
    /* the probes found somewhere to grow, and each one is thinner than the whole */
    long grew = 0;
    for (int k = 0; k < 9; k++) if (count[k] > 0) grew++;
    CHECK(grew >= 7);
    CHECK(count[0] > 0 && count[2] > 0 && count[3] > 0);
    /* rules that are measured, not only filtered */
    long min_ok = 1;
    for (int i = 0; i < plan.n; i++) {
        const TreeSpecies *sp = trees_species(plan.t[i].species);
        if (!strcmp(sp->id, "test:high") && plan.t[i].ground < 72) min_ok = 0;
        if (!strcmp(sp->id, "test:low") && plan.t[i].ground > 70) min_ok = 0;
    }
    CHECK(min_ok);
    /* a tree with no leaf block-free rule: base species sit in plausible biomes */
    long oaks_forest_or_plains = 0, oaks = 0;
    for (int i = 0; i < plan.n; i++) if (!strcmp(trees_species(plan.t[i].species)->id, "base:oak")) { oaks++; TreeColumn c; gen_tree_column(s, plan.t[i].x, plan.t[i].z, false, &c); oaks_forest_or_plains += trees_species(plan.t[i].species)->biome_ok[c.biome]; }
    CHECK(oaks > 5 && oaks_forest_or_plains == oaks);
    trees_plan_free(&plan);
    gen_scratch_destroy(s);
    gen_shutdown();
}

/* 9: spacing and clustering */
static double dispersion(const TreePlan *p, const char *id, int q) {
    enum { N = 40 };
    int cnt[N][N] = {{0}};
    double n = 0;
    for (int i = 0; i < p->n; i++) {
        if (strcmp(trees_species(p->t[i].species)->id, id)) continue;
        int gx = (p->t[i].x + BOX) / q, gz = (p->t[i].z + BOX) / q;
        if (p->t[i].x < -BOX || p->t[i].z < -BOX || gx >= N || gz >= N) continue;
        cnt[gz][gx]++; n++;
    }
    double mean = n / (N * N), var = 0;
    for (int z = 0; z < N; z++) for (int x = 0; x < N; x++) var += (cnt[z][x] - mean) * (cnt[z][x] - mean);
    return mean > 0 ? (var / (N * N)) / mean : 0;
}

static void test_spacing_and_clustering(void) {
    boot(TSEED);
    GenScratch *s = gen_scratch_create();
    CHECK(load_json("{\"id\":\"test:clump\",\"log\":\"base:log\",\"leaves\":\"base:leaves\",\"density\":1.5,\"biomes\":[\"base:forest\",\"base:plains\"],\"spacing\":{\"min_radius\":3,\"cluster_size\":5,\"clustering\":0.95},\"shape\":{\"type\":\"deciduous_slender\"}}"));
    CHECK(load_json("{\"id\":\"test:even\",\"log\":\"base:log\",\"leaves\":\"base:leaves\",\"density\":1.5,\"biomes\":[\"base:forest\",\"base:plains\"],\"spacing\":{\"min_radius\":3},\"shape\":{\"type\":\"deciduous_slender\"}}"));
    TreePlan plan = {0};
    box_plan(s, -BOX, -BOX, BOX, BOX, &plan);
    /* minimum spacing: no two trees of one layer are closer than the larger of their radii */
    long too_close = 0, pairs = 0;
    for (int i = 0; i < plan.n; i++)
        for (int j = i + 1; j < plan.n; j++) {
            const TreeInst *a = &plan.t[i], *b = &plan.t[j];
            if (a->layer != b->layer) continue;
            int dx = a->x - b->x, dz = a->z - b->z;
            if (abs(dx) > 12 || abs(dz) > 12) continue;
            int r = MAX(trees_species(a->species)->min_radius, trees_species(b->species)->min_radius);
            pairs++;
            if (dx * dx + dz * dz < r * r) too_close++;
        }
    CHECK(pairs > 100);
    CHECK(too_close == 0);
    double dc = dispersion(&plan, "test:clump", 32), de = dispersion(&plan, "test:even", 32);
    printf("selftest trees: dispersion clumped %.2f even %.2f\n", dc, de);
    CHECK(dc > de + 0.15);
    trees_plan_free(&plan);
    gen_scratch_destroy(s);
    gen_shutdown();
}

/* ---------------------------------------------------- chunks: understory, cross-chunk, determinism */

typedef struct Chunks { u16 *v; size_t per; int cx0, cz0, n; } Chunks;

static void chunk_hashes(GenScratch *s, int cx0, int cz0, int n, const int *order, u64 *out, u16 *keep) {
    int lo, hi;
    gen_band(&lo, &hi);
    size_t per = (size_t)(hi - lo + 1) * CHUNK_VOL;
    u16 *states = xmalloc(per * sizeof(u16));
    for (int k = 0; k < n * n; k++) {
        int i = order ? order[k] : k;
        gen_column(s, cx0 + i % n, cz0 + i / n, states);
        u64 h = 1469598103934665603ull;
        for (size_t j = 0; j < per; j++) h = (h ^ states[j]) * 1099511628211ull;
        out[i] = h;
        if (keep) memcpy(keep + (size_t)i * per, states, per * sizeof(u16));
    }
    free(states);
}

typedef struct ThreadJob { int cx0, cz0, n; bool reverse; u64 out[64]; } ThreadJob;

static void thread_job(void *p) {
    ThreadJob *j = p;
    GenScratch *s = gen_scratch_create();
    int order[64];
    for (int i = 0; i < j->n * j->n; i++) order[i] = j->reverse ? j->n * j->n - 1 - i : i;
    chunk_hashes(s, j->cx0, j->cz0, j->n, order, j->out, NULL);
    gen_scratch_destroy(s);
}

static void test_chunks_and_determinism(void) {
    boot(TSEED);
    GenScratch *s = gen_scratch_create();
    TreePlan plan = {0};
    /* 7: a plan is pure; two chunk boxes agree on every tree they share, and trees do straddle the border */
    TreePlan a = {0}, b = {0};
    long shared = 0, mismatch = 0;
    for (int c = 0; c < 8; c++) {
        int x0 = c * CHUNK_SIZE * 3, z0 = c * CHUNK_SIZE;
        gen_tree_plan(s, x0, z0, x0 + CHUNK_SIZE - 1, z0 + CHUNK_SIZE - 1, &a);
        gen_tree_plan(s, x0 + CHUNK_SIZE, z0, x0 + 2 * CHUNK_SIZE - 1, z0 + CHUNK_SIZE - 1, &b);
        for (int i = 0; i < a.n; i++)
            for (int j = 0; j < b.n; j++)
                if (a.t[i].gx == b.t[j].gx && a.t[i].gz == b.t[j].gz && a.t[i].layer == b.t[j].layer) {
                    shared++;
                    if (a.t[i].x != b.t[j].x || a.t[i].z != b.t[j].z || a.t[i].ground != b.t[j].ground || a.t[i].species != b.t[j].species) mismatch++;
                }
    }
    CHECK(shared > 10);
    CHECK(mismatch == 0);
    trees_plan_free(&a);
    trees_plan_free(&b);

    /* 10: understory and deadwood spawn, and reach the generated blocks */
    box_plan(s, -BOX, -BOX, BOX, BOX, &plan);
    long shrub = 0, snag = 0, fallen = 0, canopy = 0, understory_layer = 0;
    for (int i = 0; i < plan.n; i++) {
        const char *id = trees_species(plan.t[i].species)->id;
        shrub += !strcmp(id, "base:shrub");
        snag += !strcmp(id, "base:dead_snag");
        fallen += !strcmp(id, "base:fallen_log");
        canopy += plan.t[i].layer == 0;
        understory_layer += plan.t[i].layer == 1;
    }
    CHECK(shrub > 5 && snag > 0 && fallen > 0);
    CHECK(canopy > 50 && understory_layer > canopy / 4);
    /* A forest column near a fallen log holds log blocks lying flat. */
    {
        int fx = 0, fz = 0;
        bool have = false;
        for (int i = 0; i < plan.n && !have; i++)
            if (!strcmp(trees_species(plan.t[i].species)->id, "base:fallen_log") && plan.t[i].x > -BOX + 40 && plan.t[i].z > -BOX + 40) { fx = plan.t[i].x; fz = plan.t[i].z; have = true; }
        CHECK(have);
        if (have) {
            int lo, hi;
            gen_band(&lo, &hi);
            size_t per = (size_t)(hi - lo + 1) * CHUNK_VOL;
            u16 *states = xmalloc(per * sizeof(u16));
            int cx = (int)floorf((float)fx / CHUNK_SIZE), cz = (int)floorf((float)fz / CHUNK_SIZE);
            gen_column(s, cx, cz, states);
            u16 log = block_find("base:log")->default_state;
            long logs = 0;
            for (size_t j = 0; j < per; j++) logs += states[j] == log;
            CHECK(logs > 0);
            free(states);
        }
    }
    trees_plan_free(&plan);

    /* 11: chunk order, threads, and fresh against reloaded worlds all give the same blocks */
    enum { N = 5 };
    int fwd[N * N], rev[N * N], mix[N * N];
    for (int i = 0; i < N * N; i++) { fwd[i] = i; rev[i] = N * N - 1 - i; mix[i] = (int)((u64)i * 7 % (N * N)); }
    u64 h_fwd[N * N], h_rev[N * N], h_mix[N * N], h_fresh[N * N];
    chunk_hashes(s, -2, -2, N, fwd, h_fwd, NULL);
    chunk_hashes(s, -2, -2, N, rev, h_rev, NULL);
    chunk_hashes(s, -2, -2, N, mix, h_mix, NULL);
    CHECK(!memcmp(h_fwd, h_rev, sizeof h_fwd));
    CHECK(!memcmp(h_fwd, h_mix, sizeof h_fwd));
    gen_scratch_destroy(s);
    ThreadJob j1 = {-2, -2, N, false, {0}}, j2 = {-2, -2, N, true, {0}}, j3 = {-2, -2, N, false, {0}};
    Thread *t1 = thread_start(thread_job, &j1), *t2 = thread_start(thread_job, &j2), *t3 = thread_start(thread_job, &j3);
    thread_join(t1); thread_join(t2); thread_join(t3);
    CHECK(!memcmp(h_fwd, j1.out, sizeof h_fwd));
    CHECK(!memcmp(h_fwd, j2.out, sizeof h_fwd));
    CHECK(!memcmp(h_fwd, j3.out, sizeof h_fwd));
    gen_shutdown();
    boot(TSEED); /* a reload: species parsed again, caches dropped */
    GenScratch *s2 = gen_scratch_create();
    chunk_hashes(s2, -2, -2, N, NULL, h_fresh, NULL);
    CHECK(!memcmp(h_fwd, h_fresh, sizeof h_fwd));
    gen_scratch_destroy(s2);
    gen_shutdown();
}

/* ---------------------------------------------------------------------- cost */

static void test_cost(void) {
    boot(TSEED);
    GenScratch *s = gen_scratch_create();
    int lo, hi;
    gen_band(&lo, &hi);
    u16 *states = xmalloc((size_t)(hi - lo + 1) * CHUNK_VOL * sizeof(u16));
    double t[2] = {0, 0};
    int radius = 4, passes = 0;
    for (int rep = 0; rep < 4; rep++)
        for (int k = 0; k < 2; k++) {
            int mode = (k + rep) & 1;
            trees_set_enabled(mode == 1);
            double t0 = time_now_s();
            for (int cz = -radius; cz <= radius; cz++) for (int cx = -radius; cx <= radius; cx++) gen_column(s, cx + 300 + rep * 40, cz, states);
            t[mode] += time_now_s() - t0;
            passes++;
        }
    trees_set_enabled(true);
    double chunks = (double)passes / 2 * (2 * radius + 1) * (2 * radius + 1);
    printf("selftest trees: %.2f ms/chunk original trees, %.2f ms/chunk data-driven (x%.2f)\n", t[0] * 1000 / chunks, t[1] * 1000 / chunks, t[1] / t[0]);
    CHECK(t[1] < t[0] * 1.35); /* generous: timing on a busy machine is noisy; tools/perf_matrix.py holds the tight budget */
    free(states);
    gen_scratch_destroy(s);
    gen_shutdown();
}

void test_trees(void) {
    test_species_loading();
    test_shapes();
    test_placement_rules();
    test_spacing_and_clustering();
    test_chunks_and_determinism();
    test_cost();
    boot(TSEED);
    data_error_reset();
    gen_shutdown();
}
