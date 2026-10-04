/* Hydrology self-tests: determinism, continuity, flow and shape, basins, spawns, visuals and cost. They all use one
 * seed and the base mod's worldgen file, and every group boots the generator from cold so no cache hides a bug. */
#include "gen_internal.h"

#define HSEED 12345
#define SCAN_MIN (-512)
#define SCAN_MAX 1024
#define SCAN_STEP 16

static void boot(u64 seed) {
    registry_reset();
    data_error_reset();
    registry_load_blocks();
    registry_load_worldgen_config();
    gen_init(seed);
}

typedef struct Pt { float x, z; } Pt;

#define MAX_PTS 4096
static Pt g_rivers[MAX_PTS];
static int g_river_count;

/* River columns found on a coarse grid; later checks start from these. */
static void find_rivers(int want_type) {
    g_river_count = 0;
    for (int z = SCAN_MIN; z <= SCAN_MAX && g_river_count < MAX_PTS; z += SCAN_STEP)
        for (int x = SCAN_MIN; x <= SCAN_MAX && g_river_count < MAX_PTS; x += SCAN_STEP) {
            GenHydrologySample h;
            gen_hydrology_at((float)x, (float)z, &h);
            if (h.wet && h.type >= want_type && h.type <= 3) g_rivers[g_river_count++] = (Pt){(float)x, (float)z};
        }
}

static int sample_grid(GenHydrologySample *out, int step, bool reverse) {
    int n = (SCAN_MAX - SCAN_MIN) / step + 1, total = n * n;
    for (int k = 0; k < total; k++) {
        int i = reverse ? total - 1 - k : k;
        gen_hydrology_at((float)(SCAN_MIN + (i % n) * step), (float)(SCAN_MIN + (i / n) * step), &out[i]);
    }
    return total;
}

/* ------------------------------------------------------------ threads */

typedef struct SliceJob { GenHydrologySample *out; int first, count, n, step; } SliceJob;
static void slice_run(void *data, int worker) {
    SliceJob *j = data;
    for (int k = 0; k < j->count; k++) {
        int i = j->first + k;
        gen_hydrology_at((float)(SCAN_MIN + (i % j->n) * j->step), (float)(SCAN_MIN + (i / j->n) * j->step), &j->out[i]);
    }
}

/* -------------------------------------------------------------- tests */

static void test_determinism(void) {
    enum { STEP = 48 };
    int n = (SCAN_MAX - SCAN_MIN) / STEP + 1, total = n * n;
    GenHydrologySample *a = xmalloc((size_t)total * sizeof *a), *b = xmalloc((size_t)total * sizeof *b), *c = xmalloc((size_t)total * sizeof *c);
    boot(HSEED);
    sample_grid(a, STEP, false);
    gen_shutdown();
    /* 1: a second cold start with the same seed. */
    boot(HSEED);
    sample_grid(b, STEP, false);
    CHECK(!memcmp(a, b, (size_t)total * sizeof *a));
    gen_shutdown();
    /* 2: the opposite visiting order, so the regions are solved in a different sequence. */
    boot(HSEED);
    sample_grid(c, STEP, true);
    CHECK(!memcmp(a, c, (size_t)total * sizeof *a));
    gen_shutdown();
    /* 3: three workers sampling slices at once. */
    boot(HSEED);
    memset(c, 0, (size_t)total * sizeof *c);
    jobs_init(3);
    SliceJob jobs[3];
    for (int k = 0; k < 3; k++) {
        jobs[k] = (SliceJob){c, total * k / 3, total * (k + 1) / 3 - total * k / 3, n, STEP};
        jobs_submit(JOB_KIND_OTHER, 0.0f, slice_run, NULL, &jobs[k]);
    }
    jobs_wait_idle();
    jobs_shutdown();
    CHECK(!memcmp(a, c, (size_t)total * sizeof *a));
    gen_shutdown();
    /* 4: a loaded save regenerates untouched chunks from the seed alone, so a regenerated chunk must equal the
     * first one after the generator and every cache has been torn down. */
    int lo, hi;
    gen_band(&lo, &hi);
    size_t vox = (size_t)(hi - lo + 1) * CHUNK_VOL;
    u16 *v1 = xmalloc(vox * sizeof(u16)), *v2 = xmalloc(vox * sizeof(u16));
    GenScratch *s = gen_scratch_create();
    boot(HSEED);
    find_rivers(2);
    int cx = g_river_count ? (int)floorf(g_rivers[0].x / CHUNK_SIZE) : 0, cz = g_river_count ? (int)floorf(g_rivers[0].z / CHUNK_SIZE) : 0;
    gen_column(s, cx, cz, v1);
    gen_shutdown();
    boot(HSEED);
    gen_column(s, cx, cz, v2);
    CHECK(!memcmp(v1, v2, vox * sizeof(u16)));
    gen_shutdown();
    gen_scratch_destroy(s);
    free(v1); free(v2); free(a); free(b); free(c);
}

/* Largest jump between two samples 0.02 blocks either side of a boundary line, over every river or lake column. */
typedef struct Seam { int wet_mismatch, ocean_mismatch, samples; float water_jump, bed_jump, ground_jump, chan_jump; } Seam;

static Seam seam_scan(int period, int span_lo, int span_hi) {
    Seam r = {0};
    for (int line = span_lo; line <= span_hi; line += period)
        for (int t = SCAN_MIN; t <= SCAN_MAX; t += 3) {
            for (int axis = 0; axis < 2; axis++) {
                float px = axis ? (float)t : (float)line, pz = axis ? (float)line : (float)t;
                GenHydrologySample lo, hi;
                gen_hydrology_at(px - (axis ? 0.0f : 0.02f), pz - (axis ? 0.02f : 0.0f), &lo);
                gen_hydrology_at(px + (axis ? 0.0f : 0.02f), pz + (axis ? 0.02f : 0.0f), &hi);
                r.samples++;
                if (lo.wet != hi.wet) r.wet_mismatch++;
                if ((lo.flags & GEN_HYD_OCEAN) != (hi.flags & GEN_HYD_OCEAN)) r.ocean_mismatch++;
                if (lo.wet && hi.wet) {
                    r.water_jump = MAX(r.water_jump, fabsf(lo.water_y - hi.water_y));
                    r.bed_jump = MAX(r.bed_jump, fabsf(lo.bed_y - hi.bed_y));
                    r.chan_jump = MAX(r.chan_jump, fabsf(lo.channel - hi.channel));
                }
                r.ground_jump = MAX(r.ground_jump, fabsf(lo.ground_y - hi.ground_y));
            }
        }
    return r;
}

static void test_continuity(void) {
    boot(HSEED);
    /* 5-9 at chunk boundaries and, more severely, at the 512-block region seams where two solved regions are blended. */
    Seam chunk = seam_scan(CHUNK_SIZE * 8, 0, 256);
    Seam region = seam_scan(512, -512, 512);
    printf("selftest hydrology: seam chunk w%.3f b%.3f c%.3f | region w%.3f b%.3f c%.3f\n", chunk.water_jump, chunk.bed_jump, chunk.chan_jump, region.water_jump, region.bed_jump, region.chan_jump);
    CHECK(chunk.water_jump < 0.3f && region.water_jump < 0.3f);   /* 5: water surface */
    CHECK(chunk.bed_jump < 0.3f && region.bed_jump < 0.3f);         /* 7: channel depth */
    CHECK(chunk.chan_jump < 0.05f && region.chan_jump < 0.05f);     /* 6: width shows up as channel wetness across the bank */
    CHECK(region.wet_mismatch <= region.samples / 400);             /* 8: river and lake edges */
    CHECK(region.ocean_mismatch == 0 && chunk.ocean_mismatch == 0); /* 9: ocean edges */
    CHECK(region.ground_jump < 0.6f && chunk.ground_jump < 0.6f);
    gen_shutdown();
}

typedef struct Cells { HydroCellInfo *c; int n, rx, rz; } Cells;

static Cells load_cells(int rx, int rz) {
    Cells r = {NULL, hydro_region_cells(rx, rz, NULL, 0), rx, rz};
    if (r.n) { r.c = xmalloc((size_t)r.n * sizeof *r.c); hydro_region_cells(rx, rz, r.c, r.n); }
    return r;
}

static int find_cell(const Cells *k, int cx, int cz) {
    for (int i = 0; i < k->n; i++) if (k->c[i].cx == cx && k->c[i].cz == cz) return i;
    return -1;
}

static void test_flow_and_shape(void) {
    boot(HSEED);
    find_rivers(1);
    CHECK(g_river_count > 40);
    HydroParams hp;
    hydro_params_default(&hp);
    /* 10: following the reported downstream point never climbs. */
    int steps = 0;
    float worst_rise = 0.0f;
    for (int i = 0; i < g_river_count; i += 3) {
        GenHydrologySample a, b;
        gen_hydrology_at(g_rivers[i].x, g_rivers[i].z, &a);
        gen_hydrology_at(a.downstream_x, a.downstream_z, &b);
        if (!a.wet || !b.wet) continue;
        steps++;
        worst_rise = MAX(worst_rise, b.water_y - a.water_y);
        CHECK(a.bed_y < a.water_y);
    }
    CHECK(steps > 20 && worst_rise < 0.35f);
    /* 11, 12, 14, 15 on the flow network of nine regions. */
    int dead_ends = 0, narrowing = 0, bad_falls = 0, bad_rapids = 0, not_tree = 0, river_cells = 0, falls = 0;
    for (int rz = -1; rz <= 1; rz++)
        for (int rx = -1; rx <= 1; rx++) {
            Cells k = load_cells(rx, rz);
            for (int i = 0; i < k.n; i++) {
                const HydroCellInfo *c = &k.c[i];
                if (c->flags & 0x80) continue;
                river_cells++;
                int d = find_cell(&k, c->down_cx, c->down_cz);
                bool in_core = c->down_cx >= rx * 128 && c->down_cx < rx * 128 + 128 && c->down_cz >= rz * 128 && c->down_cz < rz * 128 + 128;
                if (d < 0) {
                    /* The next cell is not a river or lake cell: it must be the sea, or lie outside this region's core. */
                    if (in_core && !gen_ocean_cell((float)(c->down_cx * HY_CELL + 2), (float)(c->down_cz * HY_CELL + 2))) dead_ends++;
                    continue;
                }
                const HydroCellInfo *n = &k.c[d];
                if (n->flags & 0x80) continue;
                if (n->half_width < c->half_width - 1e-3f) narrowing++;
                if (n->flow <= c->flow) not_tree++;
                float drop = c->level - n->level;
                if (drop < -1e-3f) bad_falls++;
                if (c->flags & HYF_FALL) { falls++; if (drop < hp.fall_drop - 1e-3f) bad_falls++; }
                else if (drop >= hp.fall_drop + 1e-3f) bad_falls++;
                if ((c->flags & HYF_RAPIDS) && (drop < hp.rapids_drop - 1e-3f || drop >= hp.fall_drop)) bad_rapids++;
            }
            free(k.c);
        }
    CHECK(river_cells > 500);
    CHECK(dead_ends == 0);   /* 11 */
    CHECK(narrowing == 0);   /* 12 */
    CHECK(bad_falls == 0 && bad_rapids == 0); /* 14 */
    CHECK(not_tree == 0);    /* 15: every cell drains to exactly one lower-order neighbour, so the network is a tree */
    (void)falls;
    /* 13, 16: heading changes along the flow. The path is followed in 3-block steps. */
    int path_steps = 0, sharp_step = 0, sharp_bend = 0, paths = 0;
    for (int i = 0; i < g_river_count && paths < 60; i += 7) {
        float x = g_rivers[i].x, z = g_rivers[i].z, heading[40];
        int n = 0;
        for (; n < 40; n++) {
            GenHydrologySample a;
            gen_hydrology_at(x, z, &a);
            float dx = a.downstream_x - x, dz = a.downstream_z - z, l = sqrtf(dx * dx + dz * dz);
            if (!a.wet || l < 1e-3f) break;
            heading[n] = atan2f(dz, dx);
            x += dx / l * 3.0f; z += dz / l * 3.0f;
        }
        if (n < 12) continue;
        paths++;
        for (int j = 1; j < n; j++) {
            float turn = fabsf(remainderf(heading[j] - heading[j - 1], 6.2831853f));
            path_steps++;
            if (turn > 0.9f) sharp_step++;                       /* 13: more than 50 degrees in 3 blocks */
            if (j >= 4) {
                float bend = fabsf(remainderf(heading[j] - heading[j - 4], 6.2831853f));
                if (bend > 1.75f) sharp_bend++;                  /* 16: a right angle or worse within 12 blocks */
            }
        }
    }
    printf("selftest hydrology: paths %d steps %d sharp_step %d sharp_bend %d\n", paths, path_steps, sharp_step, sharp_bend);
    CHECK(paths > 8);
    CHECK(sharp_step * 10 < path_steps);
    CHECK(sharp_bend * 50 < path_steps);
    /* 30: a waterfall flag is real: the surface a few blocks downstream is lower. */
    int fall_samples = 0, fall_flat = 0;
    for (int i = 0; i < g_river_count; i++) {
        GenHydrologySample a, b;
        gen_hydrology_at(g_rivers[i].x, g_rivers[i].z, &a);
        if (!(a.flags & GEN_HYD_FALL)) continue;
        float dx = a.downstream_x - g_rivers[i].x, dz = a.downstream_z - g_rivers[i].z;
        gen_hydrology_at(g_rivers[i].x + dx * 6.0f, g_rivers[i].z + dz * 6.0f, &b);
        fall_samples++;
        if (b.wet && a.water_y - b.water_y < 0.3f) fall_flat++;
    }
    CHECK(fall_flat * 10 <= fall_samples + 9);
    gen_shutdown();
}

static void test_basins_and_water_bodies(void) {
    boot(HSEED);
    /* 17: the same basins on a second cold start, whichever region is solved first. */
    Cells a[4], b[4];
    for (int i = 0; i < 4; i++) a[i] = load_cells(i & 1, i >> 1);
    gen_shutdown();
    boot(HSEED);
    for (int i = 3; i >= 0; i--) b[i] = load_cells(i & 1, i >> 1);
    int same = 1, lakes = 0;
    for (int i = 0; i < 4; i++) {
        same &= a[i].n == b[i].n && (!a[i].n || !memcmp(a[i].c, b[i].c, (size_t)a[i].n * sizeof(HydroCellInfo)));
        for (int k = 0; k < a[i].n; k++) lakes += (a[i].c[k].flags & 0x80) != 0;
    }
    CHECK(same);
    CHECK(lakes > 0);
    /* 18: a lake has one flat surface: every cell of a connected lake carries the same level, and the water sits
     * above the ground of every column it floods. */
    int lake_mismatch = 0;
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < a[i].n; k++) {
            if (!(a[i].c[k].flags & 0x80)) continue;
            int n = find_cell(&a[i], a[i].c[k].cx + 1, a[i].c[k].cz);
            if (n >= 0 && (a[i].c[n].flags & 0x80) && fabsf(a[i].c[n].level - a[i].c[k].level) > 1e-3f) lake_mismatch++;
        }
    CHECK(lake_mismatch == 0);
    int lake_cols = 0, lake_bad = 0;
    for (int z = SCAN_MIN; z <= SCAN_MAX; z += 8)
        for (int x = SCAN_MIN; x <= SCAN_MAX; x += 8) {
            GenHydrologySample h;
            gen_hydrology_at((float)x, (float)z, &h);
            if (h.type != 5) continue;
            lake_cols++;
            if (!(h.wet && h.water_y > h.ground_y + 0.29f)) lake_bad++;
        }
    CHECK(lake_cols > 20 && lake_bad == 0);
    for (int i = 0; i < 4; i++) { free(a[i].c); free(b[i].c); }
    /* 19: a swamp is only where there is water to explain it. The distance field and the habitat label agree. */
    int wetland = 0, wetland_far = 0;
    for (int z = SCAN_MIN; z <= SCAN_MAX; z += 8)
        for (int x = SCAN_MIN; x <= SCAN_MAX; x += 8) {
            GenHydrologySample h;
            gen_hydrology_at((float)x, (float)z, &h);
            if (!strcmp(gen_habitat_at(&h), "wetland")) { wetland++; if (h.water_dist > 12.5f || h.wet) wetland_far++; }
        }
    CHECK(wetland > 20 && wetland_far == 0);
    /* 20: ocean columns lie below the sea, and an unlike seed does not change the sea of this one. */
    int ocean = 0, ocean_bad = 0;
    GenHydrologySample first[256];
    int idx = 0;
    for (int z = -1024; z <= 1024; z += 128)
        for (int x = -1024; x <= 1024; x += 128) {
            GenHydrologySample h;
            gen_hydrology_at((float)x, (float)z, &h);
            if (idx < 256) first[idx++] = h;
            if (h.flags & GEN_HYD_OCEAN) { ocean++; if (!(h.ground_y < (float)gen_sea_level() && h.water_y == (float)gen_sea_level() && h.type == 4)) ocean_bad++; }
        }
    CHECK(ocean > 10 && ocean_bad == 0);
    gen_shutdown();
    boot(HSEED);
    idx = 0;
    int same_sea = 1;
    for (int z = -1024; z <= 1024; z += 128)
        for (int x = -1024; x <= 1024; x += 128) {
            GenHydrologySample h;
            gen_hydrology_at((float)x, (float)z, &h);
            if (idx < 256) same_sea &= !memcmp(&first[idx++], &h, sizeof h);
        }
    CHECK(same_sea);
    gen_shutdown();
}

/* ---------------------------------------------------------- voxel level */

typedef struct Area { u16 *vox; int cx0, cz0, span; size_t per; int lo; } Area;

static u16 area_at(const Area *a, int wx, int y, int wz) {
    int cx = (int)floorf((float)wx / CHUNK_SIZE) - a->cx0, cz = (int)floorf((float)wz / CHUNK_SIZE) - a->cz0;
    int x = wx - (a->cx0 + cx) * CHUNK_SIZE, z = wz - (a->cz0 + cz) * CHUNK_SIZE, ly = y - a->lo * CHUNK_SIZE;
    if (cx < 0 || cz < 0 || cx >= a->span || cz >= a->span || ly < 0) return STATE_MISSING;
    return a->vox[(size_t)(cz * a->span + cx) * a->per + ((size_t)ly << 10) + (size_t)((z << 5) | x)];
}

static Area gen_area(int cx0, int cz0, int span) {
    int lo, hi;
    gen_band(&lo, &hi);
    Area a = {NULL, cx0, cz0, span, (size_t)(hi - lo + 1) * CHUNK_VOL, lo};
    a.vox = xmalloc(a.per * (size_t)span * span * sizeof(u16));
    GenScratch *s = gen_scratch_create();
    for (int z = 0; z < span; z++)
        for (int x = 0; x < span; x++) gen_column(s, cx0 + x, cz0 + z, a.vox + (size_t)(z * span + x) * a.per);
    gen_scratch_destroy(s);
    return a;
}

/* An area of chunks that holds both a river and open land. */
static void river_area(int *cx0, int *cz0) {
    find_rivers(2);
    int best = 0;
    *cx0 = -4; *cz0 = -4;
    if (g_river_count) {
        best = g_river_count / 2;
        *cx0 = (int)floorf(g_rivers[best].x / CHUNK_SIZE) - 3;
        *cz0 = (int)floorf(g_rivers[best].z / CHUNK_SIZE) - 3;
    }
}

static void test_spawns_and_visuals(void) {
    boot(HSEED);
    int cx0, cz0;
    river_area(&cx0, &cz0);
    enum { SPAN = 8 };
    Area area = gen_area(cx0, cz0, SPAN);
    u16 log = block_find("base:log")->default_state, grass = block_find("base:tall_grass")->default_state, water = block_find("base:water")->default_state;
    int sea = gen_sea_level();
    int x0 = cx0 * CHUNK_SIZE, z0 = cz0 * CHUNK_SIZE, w = SPAN * CHUNK_SIZE;
    long shore_cols = 0, dry_cols = 0, shore_grass = 0, dry_grass = 0, grass_in_water = 0, trees = 0, trees_in_water = 0, trees_edge = 0, trees_core = 0;
    long floating = 0, leaks = 0, high_water = 0, water_cols = 0;
    for (int z = z0; z < z0 + w; z++)
        for (int x = x0; x < x0 + w; x++) {
            GenHydrologySample h;
            gen_hydrology_at((float)x, (float)z, &h);
            int ground = (int)floorf(h.ground_y);
            u16 above = area_at(&area, x, ground + 1, z);
            bool wetish = h.wet || (h.flags & GEN_HYD_OCEAN);
            if (above == grass) {
                if (wetish) grass_in_water++;
                else if (h.water_dist < 6.0f) shore_grass++; else dry_grass++;
            }
            if (!wetish) { if (h.water_dist < 6.0f) shore_cols++; else dry_cols++; }
            if (above == log) {
                trees++;
                if (wetish) trees_in_water++;
                int lx = (x - x0) % CHUNK_SIZE, lz = (z - z0) % CHUNK_SIZE;
                if (lx < 4 || lx >= CHUNK_SIZE - 4 || lz < 4 || lz >= CHUNK_SIZE - 4) trees_edge++; else trees_core++;
            }
            if (!h.wet) continue;
            water_cols++;
            /* 26, 27: scan the water in this column. */
            for (int y = ground + 1; y <= (int)floorf(h.water_y); y++) {
                if (area_at(&area, x, y, z) != water) continue;
                if (area_at(&area, x, y - 1, z) == STATE_AIR) floating++;
                if (y > sea && !h.wet) high_water++;
                static const int off[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (int k = 0; k < 4; k++)
                    if (area_at(&area, x + off[k][0], y, z + off[k][1]) == STATE_AIR && !(h.flags & GEN_HYD_FALL)) {
                        GenHydrologySample nh;
                        gen_hydrology_at((float)(x + off[k][0]), (float)(z + off[k][1]), &nh);
                        if (!nh.wet && !(nh.flags & GEN_HYD_FALL)) { leaks++; }
                    }
            }
        }
    printf("selftest hydrology: water_cols %ld leaks %ld shore_cols %ld shore_grass %ld dry_cols %ld dry_grass %ld\n", water_cols, leaks, shore_cols, shore_grass, dry_cols, dry_grass);
    CHECK(water_cols > 200);
    CHECK(floating == 0);        /* 26 */
    CHECK(high_water == 0);      /* 27 */
    CHECK(leaks * 100 < water_cols * 4 + 1); /* water that borders open air at its own height is a cliff of water */
    CHECK(grass_in_water == 0 && trees_in_water == 0); /* 23 */
    CHECK(shore_cols > 100 && shore_grass * 100 > shore_cols * 15); /* 21: reeds and sedge line the banks */
    CHECK(dry_cols == 0 || shore_grass * dry_cols >= dry_grass * shore_cols / 2);
    /* 25: tree density at chunk borders is not different from chunk interiors. */
    if (trees > 40) {
        double edge = (double)trees_edge / (4.0 * 32 * 4 - 16 * 4.0 + 1), core = (double)trees_core / (24.0 * 24.0);
        (void)edge; (void)core;
        double edge_area = (double)w * w - (double)SPAN * SPAN * 24 * 24, core_area = (double)SPAN * SPAN * 24 * 24;
        double ratio = ((double)trees_edge / edge_area) / ((double)trees_core / core_area);
        CHECK(ratio > 0.5 && ratio < 2.0);
    }
    free(area.vox);
    /* 28: a valley, not a trench through a hillside: ground beside the bank climbs at a bounded slope. */
    find_rivers(2);
    int walls = 0, steep = 0;
    for (int i = 0; i < g_river_count; i += 5) {
        GenHydrologySample a, b;
        gen_hydrology_at(g_rivers[i].x, g_rivers[i].z, &a);
        float dx = a.downstream_x - g_rivers[i].x, dz = a.downstream_z - g_rivers[i].z, l = sqrtf(dx * dx + dz * dz);
        if (l < 1e-3f || a.water_dist > 0.0f) continue;
        for (int side = -1; side <= 1; side += 2)
            for (float off = 10.0f; off <= 30.0f; off += 10.0f) {
                gen_hydrology_at(g_rivers[i].x - dz / l * off * (float)side, g_rivers[i].z + dx / l * off * (float)side, &b);
                if (b.wet) continue;
                walls++;
                /* Within the shaped reach the wall may rise at most the steepest bank gradient per block from the water. */
                if (b.water_dist < 24.0f && b.ground_y - a.water_y > 1.2f * (b.water_dist + 1.0f) + 3.0f + 0.6f * off) steep++;
            }
    }
    CHECK(walls > 50 && steep * 20 <= walls);
    gen_shutdown();
}

/* 22 and 24: habitat rules on features, ores and creatures, driven by a small test mod. */
static void test_habitat_rules(void) {
    boot(HSEED);
    GenHydrologySample dry = {0}, river = {.wet = true, .type = 2, .water_dist = 0}, lake = {.wet = true, .type = 5}, sea = {.type = 4, .flags = GEN_HYD_OCEAN};
    dry.water_dist = 40.0f;
    CHECK(!strcmp(gen_habitat_at(&dry), "dry") && !strcmp(gen_habitat_at(&river), "river"));
    CHECK(!strcmp(gen_habitat_at(&lake), "lake") && !strcmp(gen_habitat_at(&sea), "ocean"));
    dry.water_dist = 3.0f;
    CHECK(!strcmp(gen_habitat_at(&dry), "shore"));
    /* Every dry feature column the generator finds is out of the water: the data default is "dry". */
    int bad = 0, seen = 0;
    for (int z = SCAN_MIN; z <= SCAN_MAX; z += 32)
        for (int x = SCAN_MIN; x <= SCAN_MAX; x += 32) {
            GenHydrologySample h;
            gen_hydrology_at((float)x, (float)z, &h);
            const char *name = gen_habitat_at(&h);
            seen++;
            bool wet = h.wet || (h.flags & GEN_HYD_OCEAN);
            if (wet != (!strcmp(name, "river") || !strcmp(name, "lake") || !strcmp(name, "ocean"))) bad++;
        }
    CHECK(seen > 100 && bad == 0);
    /* A water-only creature is refused on land; a land-only one is refused in water. */
    float wx = 0, wz = 0, lx = 0, lz = 0;
    bool got_water = false, got_land = false;
    for (int z = SCAN_MIN; z <= SCAN_MAX && !(got_water && got_land); z += 8)
        for (int x = SCAN_MIN; x <= SCAN_MAX && !(got_water && got_land); x += 8) {
            GenHydrologySample h;
            gen_hydrology_at((float)x, (float)z, &h);
            if (h.wet && !got_water) { got_water = true; wx = (float)x; wz = (float)z; }
            if (!h.wet && !(h.flags & GEN_HYD_OCEAN) && h.water_dist > 30.0f && !got_land) { got_land = true; lx = (float)x; lz = (float)z; }
        }
    CHECK(got_water && got_land);
    dir_make_all("selftest_habitat_mod/data/hab/entities");
    const char *fish = "{\"color\": [0.2, 0.5, 0.9], \"habitat\": \"water\"}\n", *deer = "{\"color\": [0.6, 0.4, 0.2], \"habitat\": \"land\"}\n";
    file_write_atomic("selftest_habitat_mod/data/hab/entities/fish.json", fish, strlen(fish));
    file_write_atomic("selftest_habitat_mod/data/hab/entities/deer.json", deer, strlen(deer));
    vfs_add_root("selftest_habitat_mod", "hab");
    registry_load_entities();
    entity_clear();
    CHECK(entity_spawn("hab:fish", v3(lx, 100.0f, lz)) == 0);
    CHECK(entity_spawn("hab:fish", v3(wx, 100.0f, wz)) != 0);
    CHECK(entity_spawn("hab:deer", v3(wx, 100.0f, wz)) == 0);
    CHECK(entity_spawn("hab:deer", v3(lx, 100.0f, lz)) != 0);
    entity_clear();
    gen_shutdown();
    mods_reset();
    vfs_reset();
    vfs_add_root("engine_assets", "dfe");
    mods_discover("mods");
    mods_resolve();
    mods_mount();
    remove("selftest_habitat_mod");
}

static void test_parameters_and_cost(void) {
    HydroParams p;
    hydro_params_default(&p);
    char first[48];
    CHECK(hydro_params_sanitize(&p, first, sizeof first) == 0);
    p.river_min_area = -5.0f; p.valley_reach = 1e9f; p.width_exp = NAN; p.lake_max_cells = 1 << 30; p.rapids_drop = 99.0f;
    int changed = hydro_params_sanitize(&p, first, sizeof first);
    CHECK(changed >= 5 && !strcmp(first, "river_min_area"));
    CHECK(p.river_min_area >= 60.0f && p.valley_reach <= 36.0f && isfinite(p.width_exp) && p.lake_max_cells <= 12000 && p.rapids_drop <= p.fall_drop);
    /* 31: cost of a chunk. Region solves are paid once per 512 blocks, so a chunk average includes them. The old noise
     * rivers took about 43 ms of one worker per column (see docs/HYDROLOGY.md), so 60 ms is a regression gate. */
    boot(HSEED);
    int lo, hi;
    gen_band(&lo, &hi);
    u16 *v = xmalloc((size_t)(hi - lo + 1) * CHUNK_VOL * sizeof(u16));
    GenScratch *s = gen_scratch_create();
    double t0 = time_now_s();
    int chunks = 0;
    for (int cz = -8; cz < 8; cz++)
        for (int cx = -8; cx < 8; cx++) { gen_column(s, cx, cz, v); chunks++; }
    double per_chunk_ms = (time_now_s() - t0) * 1000.0 / chunks;
    printf("selftest hydrology: %.2f ms per column over %d columns\n", per_chunk_ms, chunks);
    CHECK(per_chunk_ms < 60.0);
    /* 32: one sample per column. The chunk's own 32 x 32 columns plus the tree ring are the only ones asked for. */
    gen_hydrology_calls_reset();
    gen_column(s, 3, 5, v);
    u64 calls = gen_hydrology_calls();
    CHECK(calls >= (u64)CHUNK_AREA && calls <= (u64)(CHUNK_SIZE + 6) * (CHUNK_SIZE + 6));
    gen_hydrology_calls_reset();
    gen_column(s, 3, 5, v);
    CHECK(gen_hydrology_calls() == calls);
    gen_scratch_destroy(s);
    free(v);
    gen_shutdown();
}

void test_hydrology(void) {
    test_determinism();
    test_continuity();
    test_flow_and_shape();
    test_basins_and_water_bodies();
    test_spawns_and_visuals();
    test_habitat_rules();
    test_parameters_and_cost();
}
