/* Command-line diagnostics for the drainage model: transect dumps, river path listings, spawn audits and a
 * load-order comparison. Each command boots just the block registry and world generator, so none needs a window. */
#include "gen_internal.h"

static bool boot_gen(u64 seed) {
    registry_reset();
    data_error_reset();
    registry_load_blocks();
    registry_load_worldgen_config();
    if (data_error_count()) {
        fprintf(stderr, "worldgen data has errors; first: %s\n", data_error_text(0));
        return false;
    }
    gen_init(seed);
    return true;
}

static int dump_hydrology(u64 seed, float x0, float z0, float x1, float z1) {
    if (!boot_gen(seed)) return 1;
    float dx = x1 - x0, dz = z1 - z0, len = sqrtf(dx * dx + dz * dz);
    int n = CLAMP((int)(len / 8.0f) + 1, 2, 4096);
    printf("x\tz\tground\twater\tbed\ttype\tflow\twater_dist\tchannel\tflags\thabitat\n");
    for (int i = 0; i < n; i++) {
        float t = (float)i / (float)(n - 1), x = x0 + dx * t, z = z0 + dz * t;
        GenHydrologySample h;
        gen_hydrology_at(x, z, &h);
        printf("%.1f\t%.1f\t%.2f\t%.2f\t%.2f\t%d\t%.0f\t%.1f\t%.2f\t%d\t%s\n", x, z, h.ground_y, h.wet || (h.flags & GEN_HYD_OCEAN) ? h.water_y : 0.0f,
               h.wet ? h.bed_y : 0.0f, h.type, h.flow, h.water_dist, h.channel, h.flags, gen_habitat_at(&h));
    }
    printf("# regions solved: %d\n", hydro_regions_built());
    gen_shutdown();
    return 0;
}

/* ASCII picture of one square: ~ ocean, = river, # main river, o lake, ; bank, . dry, with ^ where the water is a fall. */
static int dump_map(u64 seed, int x0, int z0, int size, int step) {
    if (!boot_gen(seed)) return 1;
    step = CLAMP(step, 1, 64);
    size = CLAMP(size, 16, 4096);
    for (int z = z0; z < z0 + size; z += step) {
        for (int x = x0; x < x0 + size; x += step) {
            GenHydrologySample h;
            gen_hydrology_at((float)x, (float)z, &h);
            char c = '.';
            if (h.flags & GEN_HYD_OCEAN) c = '~';
            else if (h.wet) c = h.type == 5 ? 'o' : (h.flags & GEN_HYD_FALL ? '^' : (h.type == 3 ? '#' : '='));
            else if (h.water_dist < 3.0f) c = ';';
            putchar(c);
        }
        putchar('\n');
    }
    gen_shutdown();
    return 0;
}

#define RIVER_SPAN 2 /* regions on each side of the origin */

static int dump_rivers(u64 seed) {
    if (!boot_gen(seed)) return 1;
    enum { CELLS = 128 };
    int total_paths = 0;
    printf("river\tregion\tstart_x\tstart_z\tend_x\tend_z\tcells\tstart_width\tend_width\tstart_level\tend_level\toutlet\n");
    for (int rz = -RIVER_SPAN; rz < RIVER_SPAN; rz++)
        for (int rx = -RIVER_SPAN; rx < RIVER_SPAN; rx++) {
            int count = hydro_region_cells(rx, rz, NULL, 0);
            if (!count) continue;
            HydroCellInfo *cells = xmalloc((size_t)count * sizeof *cells);
            hydro_region_cells(rx, rz, cells, count);
            int *grid = xmalloc(CELLS * CELLS * sizeof(int));
            u8 *has_up = xcalloc(CELLS * CELLS, 1);
            for (int i = 0; i < CELLS * CELLS; i++) grid[i] = -1;
            for (int i = 0; i < count; i++) grid[(cells[i].cz - rz * CELLS) * CELLS + (cells[i].cx - rx * CELLS)] = i;
            for (int i = 0; i < count; i++) {
                if (cells[i].flags & 0x80) continue;
                int dx = cells[i].down_cx - rx * CELLS, dz = cells[i].down_cz - rz * CELLS;
                if (dx >= 0 && dz >= 0 && dx < CELLS && dz < CELLS && grid[dz * CELLS + dx] >= 0) has_up[dz * CELLS + dx] = 1;
            }
            for (int i = 0; i < count; i++) {
                int slot = (cells[i].cz - rz * CELLS) * CELLS + (cells[i].cx - rx * CELLS);
                if ((cells[i].flags & 0x80) || has_up[slot]) continue;
                int cur = i, len = 1;
                for (;;) {
                    int dx = cells[cur].down_cx - rx * CELLS, dz = cells[cur].down_cz - rz * CELLS;
                    if (dx < 0 || dz < 0 || dx >= CELLS || dz >= CELLS || grid[dz * CELLS + dx] < 0 || (cells[grid[dz * CELLS + dx]].flags & 0x80)) break;
                    cur = grid[dz * CELLS + dx];
                    len++;
                }
                const HydroCellInfo *a = &cells[i], *b = &cells[cur];
                float ex = (float)(b->down_cx * HY_CELL + 2), ez = (float)(b->down_cz * HY_CELL + 2);
                GenHydrologySample out;
                gen_hydrology_at(ex, ez, &out);
                const char *outlet = out.flags & GEN_HYD_OCEAN ? "ocean" : (out.type == 5 || b->lake_mouth ? "lake" : (out.wet ? "river-in-next-region" : "region-edge"));
                if (len < 4) continue;
                printf("%d\t%d,%d\t%d\t%d\t%d\t%d\t%d\t%.1f\t%.1f\t%.1f\t%.1f\t%s\n", total_paths++, rx, rz, a->cx * HY_CELL, a->cz * HY_CELL, b->cx * HY_CELL, b->cz * HY_CELL,
                       len, a->half_width * 2.0f, b->half_width * 2.0f, a->level, b->level, outlet);
            }
            free(cells); free(grid); free(has_up);
        }
    printf("# %d river paths, %d regions solved\n", total_paths, hydro_regions_built());
    gen_shutdown();
    return 0;
}

static int dump_spawns(u64 seed, int cx_blocks, int cz_blocks, int radius) {
    if (!boot_gen(seed)) return 1;
    radius = CLAMP(radius, 16, 1024);
    BlockDef *log_def = block_find("base:log"), *grass_def = block_find("base:tall_grass"), *water_def = block_find("base:water");
    u16 log_state = log_def ? log_def->default_state : STATE_MISSING, grass_state = grass_def ? grass_def->default_state : STATE_MISSING;
    u16 water_state = water_def ? water_def->default_state : STATE_MISSING;
    int lo, hi;
    gen_band(&lo, &hi);
    size_t n = (size_t)(hi - lo + 1) * CHUNK_VOL;
    u16 *states = xmalloc(n * sizeof(u16));
    GenScratch *s = gen_scratch_create();
    static const char *HAB[] = {"dry", "shore", "wetland", "river", "lake", "ocean"};
    long cols[6] = {0}, trees[6] = {0}, grass[6] = {0}, bad_tree = 0, bad_plant = 0;
    int c0x = (int)floorf((float)(cx_blocks - radius) / CHUNK_SIZE), c1x = (int)floorf((float)(cx_blocks + radius) / CHUNK_SIZE);
    int c0z = (int)floorf((float)(cz_blocks - radius) / CHUNK_SIZE), c1z = (int)floorf((float)(cz_blocks + radius) / CHUNK_SIZE);
    for (int cz = c0z; cz <= c1z; cz++)
        for (int cx = c0x; cx <= c1x; cx++) {
            gen_column(s, cx, cz, states);
            for (int z = 0; z < CHUNK_SIZE; z++)
                for (int x = 0; x < CHUNK_SIZE; x++) {
                    GenHydrologySample h;
                    gen_hydrology_at((float)(cx * CHUNK_SIZE + x), (float)(cz * CHUNK_SIZE + z), &h);
                    const char *name = gen_habitat_at(&h);
                    int k = 0;
                    while (strcmp(HAB[k], name)) k++;
                    cols[k]++;
                    int ground = (int)floorf(h.ground_y), ly = ground + 1 - lo * CHUNK_SIZE;
                    if (ly < 1 || ly >= (hi - lo + 1) * CHUNK_SIZE) continue;
                    u16 above = states[((size_t)ly << 10) | (size_t)((z << 5) | x)];
                    if (above == log_state) { trees[k]++; if (k >= 3) bad_tree++; }
                    if (above == grass_state) { grass[k]++; if (k >= 3) bad_plant++; }
                    (void)water_state;
                }
        }
    printf("habitat\tcolumns\ttrees\ttall_grass\n");
    for (int k = 0; k < 6; k++) printf("%s\t%ld\t%ld\t%ld\n", HAB[k], cols[k], trees[k], grass[k]);
    printf("# trees in water: %ld, plants in water: %ld\n", bad_tree, bad_plant);
    gen_scratch_destroy(s);
    free(states);
    gen_shutdown();
    return bad_tree || bad_plant ? 1 : 0;
}

/* Generates the same area twice from a cold start, visiting chunks in two different orders, and reports every column
 * whose hydrology or voxels differ. Any difference means generation depends on load order. */
static void chunk_order(const char *kind, int span, int *out) {
    int n = span * span;
    for (int i = 0; i < n; i++) out[i] = i;
    if (!strcmp(kind, "reverse")) for (int i = 0; i < n; i++) out[i] = n - 1 - i;
    else if (!strcmp(kind, "shuffle")) {
        u64 r = 0x9e3779b97f4a7c15ull;
        for (int i = n - 1; i > 0; i--) { r = r * 6364136223846793005ull + 1442695040888963407ull; int j = (int)((r >> 33) % (u64)(i + 1)); int t = out[i]; out[i] = out[j]; out[j] = t; }
    } else if (!strcmp(kind, "spiral")) {
        int x = span / 2, z = span / 2, dx = 1, dz = 0, leg = 1, done = 0;
        while (done < n) {
            for (int rep = 0; rep < 2 && done < n; rep++) {
                for (int s = 0; s < leg && done < n; s++) {
                    if (x >= 0 && z >= 0 && x < span && z < span) out[done++] = z * span + x;
                    x += dx; z += dz;
                }
                int t = dx; dx = -dz; dz = t;
            }
            leg++;
        }
    }
}

static int compare_worldgen(u64 seed, const char *a, const char *b) {
    enum { SPAN = 24 };
    static const char *KINDS[] = {"forward", "reverse", "spiral", "shuffle"};
    bool ok_a = false, ok_b = false;
    for (int i = 0; i < 4; i++) { ok_a |= !strcmp(a, KINDS[i]); ok_b |= !strcmp(b, KINDS[i]); }
    if (!ok_a || !ok_b) { fprintf(stderr, "orders are forward, reverse, spiral or shuffle\n"); return 2; }
    int lo, hi;
    size_t n = 0;
    u16 *vox[2] = {NULL, NULL};
    GenHydrologySample *hy[2] = {NULL, NULL};
    int order[SPAN * SPAN];
    for (int pass = 0; pass < 2; pass++) {
        if (!boot_gen(seed)) return 1;
        gen_band(&lo, &hi);
        n = (size_t)(hi - lo + 1) * CHUNK_VOL;
        vox[pass] = xmalloc(n * SPAN * SPAN * sizeof(u16));
        hy[pass] = xmalloc((size_t)SPAN * SPAN * CHUNK_AREA * sizeof(GenHydrologySample));
        chunk_order(pass ? b : a, SPAN, order);
        GenScratch *s = gen_scratch_create();
        for (int k = 0; k < SPAN * SPAN; k++) {
            int cx = order[k] % SPAN - SPAN / 2, cz = order[k] / SPAN - SPAN / 2, slot = order[k];
            gen_column(s, cx, cz, vox[pass] + (size_t)slot * n);
            for (int i = 0; i < CHUNK_AREA; i++)
                gen_hydrology_at((float)(cx * CHUNK_SIZE + (i & 31)), (float)(cz * CHUNK_SIZE + (i >> 5)), &hy[pass][(size_t)slot * CHUNK_AREA + i]);
        }
        gen_scratch_destroy(s);
        gen_shutdown();
    }
    long vdiff = 0, hdiff = 0;
    for (size_t i = 0; i < n * SPAN * SPAN; i++) vdiff += vox[0][i] != vox[1][i];
    for (size_t i = 0; i < (size_t)SPAN * SPAN * CHUNK_AREA; i++) hdiff += memcmp(&hy[0][i], &hy[1][i], sizeof hy[0][i]) != 0;
    printf("seed %llu, %d x %d chunks, order %s vs %s: %ld voxel differences, %ld hydrology column differences\n", (unsigned long long)seed, SPAN, SPAN, a, b, vdiff, hdiff);
    for (int p = 0; p < 2; p++) { free(vox[p]); free(hy[p]); }
    return vdiff || hdiff ? 1 : 0;
}

int hydro_diag_run(int argc, char **argv) {
    const char *cmd = argv[0];
    if (!strcmp(cmd, "--dump-trees") || !strcmp(cmd, "--dump-tree-shape") || !strcmp(cmd, "--tree-bench")) return tree_diag_run(argc, argv);
    if (!strcmp(cmd, "--dump-biomes")) return biome_diag_run(argc, argv);
    if (!strcmp(cmd, "--dump-hydrology") && argc >= 6) return dump_hydrology(strtoull(argv[1], NULL, 10), (float)atof(argv[2]), (float)atof(argv[3]), (float)atof(argv[4]), (float)atof(argv[5]));
    if (!strcmp(cmd, "--dump-hydro-map") && argc >= 6) return dump_map(strtoull(argv[1], NULL, 10), atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), atoi(argv[5]));
    if (!strcmp(cmd, "--dump-rivers") && argc >= 2) return dump_rivers(strtoull(argv[1], NULL, 10));
    if (!strcmp(cmd, "--dump-spawns") && argc >= 5) return dump_spawns(strtoull(argv[1], NULL, 10), atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
    if (!strcmp(cmd, "--compare-worldgen") && argc >= 4) return compare_worldgen(strtoull(argv[1], NULL, 10), argv[2], argv[3]);
    fprintf(stderr, "usage: %s needs more arguments; see --help\n", cmd);
    return 2;
}
