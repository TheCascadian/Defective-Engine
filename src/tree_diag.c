/* Command-line tree diagnostics: a placement dump over an area, an ASCII picture of one tree, and a timing
 * comparison of the data-driven placement against the original single-shape trees. */
#include "gen_internal.h"
#include "epoch.h"

static bool boot_gen(u64 seed) {
    registry_reset();
    data_error_reset();
    registry_load_blocks();
    registry_load_worldgen_config();
    if (data_error_count()) {
        fprintf(stderr, "data has errors; first: %s\n", data_error_text(0));
        return false;
    }
    /* DFE_DIAG_FW=1 runs the diagnostic under Forever Worlds, so the newest epoch (and its biome kernel) applies. */
    const char *fw = getenv("DFE_DIAG_FW");
    if (fw && *fw == '1') {
        char err[256];
        if (!epoch_registry_load(NULL, err, sizeof err)) { fprintf(stderr, "epochs: %s\n", err); return false; }
        forever_worlds_set_active(true);
    }
    gen_init(seed);
    return true;
}

static int dump_trees(u64 seed, int x0, int z0, int x1, int z1) {
    if (!boot_gen(seed)) return 1;
    if (!trees_enabled()) { fprintf(stderr, "no tree species are loaded\n"); gen_shutdown(); return 1; }
    GenScratch *s = gen_scratch_create();
    TreePlan plan = {0};
    gen_tree_plan(s, MIN(x0, x1), MIN(z0, z1), MAX(x0, x1), MAX(z0, z1), &plan);
    long per[TREE_MAX_SPECIES] = {0};
    printf("x\tz\tground\tspecies\tlayer\theight\tlogs\tleaves\tradius\n");
    for (int i = 0; i < plan.n; i++) {
        const TreeInst *t = &plan.t[i];
        if (t->x < MIN(x0, x1) || t->x > MAX(x0, x1) || t->z < MIN(z0, z1) || t->z > MAX(z0, z1)) continue;
        const TreeSpecies *sp = trees_species(t->species);
        const TreeShape *sh = tree_generate(sp, tree_seed(seed, t->gx, t->gz, t->layer));
        per[t->species]++;
        printf("%d\t%d\t%d\t%s\t%d\t%d\t%d\t%d\t%.1f\n", t->x, t->z, t->ground, sp->id, t->layer, sh->measured_height, sh->logs, sh->leaves, sh->canopy_radius);
    }
    printf("# trees by species:");
    for (int i = 0; i < trees_species_count(); i++) if (per[i]) printf(" %s=%ld", trees_species(i)->id, per[i]);
    printf("\n# sites evaluated: %llu\n", (unsigned long long)trees_sites_evaluated());
    trees_plan_free(&plan);
    gen_scratch_destroy(s);
    gen_shutdown();
    return 0;
}

static int dump_shape(const char *id, u64 seed, int x, int z) {
    if (!boot_gen(seed)) return 1;
    const TreeSpecies *sp = trees_find(id);
    if (!sp) {
        fprintf(stderr, "no tree species \"%s\"; loaded:", id);
        for (int i = 0; i < trees_species_count(); i++) fprintf(stderr, " %s", trees_species(i)->id);
        fprintf(stderr, "\n");
        gen_shutdown();
        return 1;
    }
    const TreeShape *sh = tree_generate(sp, tree_seed(seed, x, z, sp->layer));
    printf("# %s seed %llu at (%d,%d): trunk height %d (measured %d), %d logs, %d leaves, branches %d, trunk radius %.2f, lean %.2f, canopy radius %.1f, density %.2f, clipped %d\n",
           sp->id, (unsigned long long)seed, x, z, sh->height, sh->measured_height, sh->logs, sh->leaves, sh->branch_count, sh->trunk_radius, sh->lean, sh->canopy_radius, sh->canopy_density, sh->clipped);
    int w = sh->max_x - sh->min_x + 1, d = sh->max_z - sh->min_z + 1;
    char *grid = xmalloc((size_t)w * (size_t)d);
    for (int y = sh->max_y; y >= sh->min_y; y--) {
        memset(grid, '.', (size_t)w * (size_t)d);
        bool any = false;
        for (int i = 0; i < sh->n; i++) {
            const TreeVoxel *v = &sh->v[i];
            if (v->y != y) continue;
            grid[(v->z - sh->min_z) * w + (v->x - sh->min_x)] = v->kind == TV_LEAF ? 'L' : (v->kind == TV_LOG ? '#' : (v->kind == TV_ROOT ? 'R' : 'F'));
            any = true;
        }
        if (!any) continue;
        printf("y=%d\n", y);
        for (int zz = 0; zz < d; zz++) printf("%.*s\n", w, grid + (size_t)zz * (size_t)w);
    }
    free(grid);
    gen_shutdown();
    return 0;
}

/* Generates the same chunks with the original trees and with the data-driven ones, alternating, and prints both times. */
static int tree_bench(u64 seed, int radius) {
    if (!boot_gen(seed)) return 1;
    radius = CLAMP(radius, 1, 16);
    int lo, hi;
    gen_band(&lo, &hi);
    u16 *states = xmalloc((size_t)(hi - lo + 1) * CHUNK_VOL * sizeof(u16));
    GenScratch *s = gen_scratch_create();
    double t[2] = {0, 0};
    long chunks = 0;
    /* Each pass takes a fresh stretch of world, so neither mode reuses the other's tree cache; the order alternates so
     * the one that runs second gets no systematic head start from shared hydrology regions. */
    for (int rep = 0; rep < 4; rep++)
        for (int k = 0; k < 2; k++) {
            int mode = (k + rep) & 1;
            trees_set_enabled(mode == 1);
            double t0 = time_now_s();
            for (int cz = -radius; cz <= radius; cz++)
                for (int cx = -radius; cx <= radius; cx++) gen_column(s, cx + rep * 64, cz, states);
            t[mode] += time_now_s() - t0;
        }
    chunks = (long)(2 * radius + 1) * (2 * radius + 1) * 8;
    trees_set_enabled(true);
    printf("{\"seed\": %llu, \"chunks_per_pass\": %ld, \"legacy_ms_per_chunk\": %.3f, \"trees_ms_per_chunk\": %.3f, \"ratio\": %.3f}\n",
           (unsigned long long)seed, chunks / 8, t[0] * 1000.0 / (double)(chunks / 2), t[1] * 1000.0 / (double)(chunks / 2), t[0] > 0 ? t[1] / t[0] : 0.0);
    free(states);
    gen_scratch_destroy(s);
    gen_shutdown();
    return 0;
}

int tree_diag_run(int argc, char **argv) {
    const char *cmd = argv[0];
    if (!strcmp(cmd, "--dump-trees") && argc >= 6) return dump_trees(strtoull(argv[1], NULL, 10), atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), atoi(argv[5]));
    if (!strcmp(cmd, "--dump-tree-shape") && argc >= 5) return dump_shape(argv[1], strtoull(argv[2], NULL, 10), atoi(argv[3]), atoi(argv[4]));
    if (!strcmp(cmd, "--tree-bench") && argc >= 2) return tree_bench(strtoull(argv[1], NULL, 10), argc >= 3 ? atoi(argv[2]) : 6);
    fprintf(stderr, "usage: %s needs more arguments; see --help\n", cmd);
    return 2;
}
