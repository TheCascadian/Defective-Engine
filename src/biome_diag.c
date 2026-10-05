/* --dump-biomes SEED [N] [STEP] [KERNEL]: biome coverage over an N x N grid of columns STEP blocks apart, so a bad climate
 * box shows up as a biome that never appears or that swallows the map.  Land biomes should each cover about 1% or more
 * and none more than 35%; ocean is exempt from the upper bound. */
#include "biome.h"

static int boot(u64 seed) {
    registry_reset();
    data_error_reset();
    registry_load_blocks();
    registry_load_worldgen_config();
    if (data_error_count()) {
        fprintf(stderr, "data has errors; first: %s\n", data_error_text(0));
        return 1;
    }
    gen_init(seed);
    return 0;
}

int biome_diag_run(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: --dump-biomes SEED [N] [STEP] [KERNEL]\n"); return 2; }
    u64 seed = strtoull(argv[1], NULL, 10);
    int n = argc >= 3 ? atoi(argv[2]) : 200, step = argc >= 4 ? atoi(argv[3]) : 32, kernel = argc >= 5 ? atoi(argv[4]) : 1;
    if (n < 1 || n > 4000 || step < 1) { fprintf(stderr, "N must be 1..4000 and STEP at least 1\n"); return 2; }
    if (boot(seed)) return 1;
    int slots = gen_biome_table_size(kernel);
    long count[BIOME_MAX] = {0};
    double height[BIOME_MAX] = {0};
    long total = 0;
    for (int i = 0; i < n; i++)
        for (int k = 0; k < n; k++) {
            float h;
            int b = gen_sample_biome(kernel, (i - n / 2) * step, (k - n / 2) * step, &h);
            if (b < 0 || b >= BIOME_MAX) continue;
            count[b]++; height[b] += h; total++;
        }
    printf("# seed %llu kernel %d: %d x %d columns, %d blocks apart, %d biomes\n", (unsigned long long)seed, kernel, n, n, step, slots);
    printf("%-26s %8s %7s %8s\n", "biome", "columns", "share", "mean_y");
    int warn = 0;
    for (int b = 0; b < slots; b++) {
        double share = total ? 100.0 * (double)count[b] / (double)total : 0.0;
        bool ocean = !strcmp(gen_biome_id(b), "base:ocean");
        const char *flag = "";
        if (kernel >= 1 && share < 1.0 && !ocean) { flag = "  # under 1%"; warn++; }
        if (kernel >= 1 && share > 35.0 && !ocean) { flag = "  # over 35%"; warn++; }
        printf("%-26s %8ld %6.2f%% %8.1f%s\n", gen_biome_id(b), count[b], share, count[b] ? height[b] / (double)count[b] : 0.0, flag);
    }
    printf("# %d coverage warning(s)\n", warn);
    gen_shutdown();
    return 0;
}
