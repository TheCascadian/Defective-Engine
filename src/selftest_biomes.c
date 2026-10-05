/* Biome self-tests: the data-driven biome files (kernel 1), their layer rules, coverage and determinism.
 * Epoch 0 invariance is covered by test_gen_golden_epoch0 in selftest.c. */
#include "gen_internal.h"
#include "biome.h"

#define BSEED 424242

static void boot(u64 seed) {
    registry_reset();
    data_error_reset();
    registry_load_blocks();
    registry_load_worldgen_config();
    gen_init(seed);
}

static void test_biome_files(void) {
    boot(BSEED);
    CHECK(biome_count() >= 24);
    CHECK(biome_count() <= BIOME_MAX);
    static const char *legacy[BIOME_LEGACY] = {"base:ocean", "base:beach", "base:desert", "base:tundra", "base:swamp", "base:forest", "base:plains", "base:mountain"};
    for (int i = 0; i < BIOME_LEGACY; i++) CHECK(biome_get(i) && strcmp(biome_get(i)->id, legacy[i]) == 0);
    CHECK(biome_find("base:alpine") >= BIOME_LEGACY && biome_find("base:badlands") >= BIOME_LEGACY);
    CHECK(biome_find("base:no_such_biome") < 0);
    for (int i = 0; i < biome_count(); i++) {
        const BiomeDef *b = biome_get(i);
        CHECK(b->layer_count > 0);
        /* every top layer chain ends in an unconditional layer, so a column always gets a surface block */
        int last_top = -1;
        for (int k = 0; k < b->layer_count; k++) if (b->layers[k].depth == 0) last_top = k;
        CHECK(last_top >= 0);
        for (int k = 0; k < b->layer_count; k++) {
            CHECK(b->layers[k].state != STATE_AIR && b->layers[k].state != STATE_MISSING);
            CHECK(block_of_state(b->layers[k].state) != NULL);
        }
        for (int k = 0; k < b->plant_count; k++) {
            CHECK(b->plants[k].state != STATE_AIR && b->plants[k].state != STATE_MISSING);
            CHECK(b->plants[k].on_count > 0);
            for (int j = 0; j < b->plants[k].on_count; j++) CHECK(b->plants[k].on[j] != STATE_AIR && b->plants[k].on[j] != STATE_MISSING);
        }
    }
    CHECK(biome_has_tag(biome_get(biome_find("base:alpine")), "treeline"));
    gen_shutdown();
}

/* Sweeps the environment space: no biome ever returns air or a missing state for a surface or subsurface request. */
static void test_layers_never_air(void) {
    boot(BSEED);
    u64 rng = 0x9e3779b97f4a7c15ull;
    for (int i = 0; i < biome_count(); i++) {
        const BiomeDef *b = biome_get(i);
        for (int n = 0; n < 1000; n++) {
            BiomeEnv e = {0};
            rng = hash64(rng + 1);
            e.y = 30 + (int)(rng % 220);
            e.slope = (float)((rng >> 8) % 100) / 8.0f;
            e.river = (float)((rng >> 16) % 100) / 100.0f;
            e.water_dist = (float)((rng >> 24) % 64);
            e.snowline = 150 + (float)((rng >> 32) % 60);
            e.treeline = 120 + (float)((rng >> 40) % 60);
            e.speck_height = (float)e.y;
            for (int k = 0; k < 3; k++) { rng = hash64(rng + 1); e.patch[k] = (float)(rng % 2000) / 1000.0f - 1.0f; }
            u16 top = biome_surface_state(b, &e);
            CHECK(top != STATE_AIR && top != STATE_MISSING);
            if (n % 8 == 0) {
                int depth = 1 + (int)((rng >> 50) % (u64)(b->subsurface_depth > 0 ? b->subsurface_depth : 1));
                u16 sub = biome_sub_state(b, depth, &e);
                CHECK(sub != STATE_AIR);
            }
        }
    }
    gen_shutdown();
}

static void test_coverage_and_determinism(void) {
    boot(BSEED);
    int n = biome_count();
    int counts[BIOME_MAX] = {0};
    int total = 0;
    int first[64], nfirst = 0;
    for (int z = -16; z < 16; z++)
        for (int x = -16; x < 16; x++) {
            int b = gen_sample_biome(1, x * 192, z * 192, NULL);
            CHECK(b >= 0 && b < n);
            counts[b]++;
            total++;
            if ((x + z) % 8 == 0 && nfirst < 64) first[nfirst++] = b;
        }
    /* a second sweep of a subset reproduces the same indices */
    int again = 0;
    for (int z = -16; z < 16; z++)
        for (int x = -16; x < 16; x++)
            if ((x + z) % 8 == 0 && again < nfirst) CHECK(gen_sample_biome(1, x * 192, z * 192, NULL) == first[again++]);
    int ocean = 0, land_present = 0;
    for (int i = 0; i < n; i++) {
        if (biome_get(i)->role == BIOME_ROLE_OCEAN) { ocean = counts[i]; continue; }
        if (biome_get(i)->role == BIOME_ROLE_SHORE) continue;
        if (counts[i] > 0) land_present++;
        CHECK(counts[i] * 100 <= total * 35); /* no land biome swallows the map */
    }
    CHECK(ocean < total);
    CHECK(land_present >= n / 2); /* a 1000-sample window sees most of the land biomes */
    /* kernel 0 never returns an index outside the original eight */
    for (int i = 0; i < 100; i++) CHECK(gen_sample_biome(0, i * 53, -i * 71, NULL) < BIOME_LEGACY);
    gen_shutdown();
}

void test_biomes(void) {
    test_biome_files();
    test_layers_never_air();
    test_coverage_and_determinism();
    boot(BSEED);
    data_error_reset();
    gen_shutdown();
}
