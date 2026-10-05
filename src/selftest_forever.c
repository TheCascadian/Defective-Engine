/* Forever Worlds self-tests.  Phase 1: the experimental feature registry, per-world persistence and gating. */
#include "dfe.h"
#include "epoch.h"

void remove_tree_files(const char *dir);

static void test_feature_registry(void) {
    data_error_reset();
    CHECK(experimental_load() >= 1);
    const ExpFeatureDef *d = experimental_find("forever_worlds");
    CHECK(d != NULL);
    if (!d) return;
    CHECK(!strcmp(d->name, "Forever Worlds"));
    CHECK(!strcmp(d->version, "1.0.0"));
    CHECK(!strcmp(d->released, "2026-10-04"));
    CHECK(!strcmp(d->status, "Experimental"));
    CHECK(d->requires_new_world);
    CHECK(strlen(d->description) > 100);
    CHECK(experimental_find("not_a_feature") == NULL);
    CHECK(data_error_count() == 0);
}

static void test_feature_persistence(void) {
    const char *dir = "selftest_exp_world";
    remove_tree_files(dir);
    registry_reset();
    data_error_reset();
    registry_load_blocks();
    experimental_load();
    char warn[300];

    /* Default is off; only an explicit selection turns a feature on in a new world. */
    CHECK(save_open(dir, 99));
    CHECK(save_was_created());
    CHECK(!experimental_enabled(save_meta(), "forever_worlds"));
    experimental_selection_clear();
    CHECK(!experimental_selection_apply(save_meta()));
    CHECK(!experimental_enabled(save_meta(), "forever_worlds"));
    CHECK(experimental_set(save_meta(), "nope", true, false, true, warn, sizeof warn) == EXP_UNKNOWN);
    experimental_selection_set("forever_worlds", true);
    CHECK(experimental_selection_get("forever_worlds"));
    CHECK(experimental_selection_apply(save_meta()));
    CHECK(!experimental_selection_get("forever_worlds"));
    CHECK(experimental_enabled(save_meta(), "forever_worlds"));
    CHECK(save_meta_flush());
    save_close();

    /* The state survives a reload together with its version and timestamp. */
    CHECK(save_open(dir, 99));
    CHECK(!save_was_created());
    CHECK(experimental_enabled(save_meta(), "forever_worlds"));
    SaveMeta *m = save_meta();
    CHECK(m->exp_count == 1);
    CHECK(!strcmp(m->exp[0].enabled_version, "1.0.0"));
    CHECK(strlen(m->exp[0].enabled_at) == 20 && m->exp[0].enabled_at[19] == 'Z' && m->exp[0].enabled_at[10] == 'T');

    /* Disabling needs a confirmation, and says what is lost. */
    CHECK(experimental_set(m, "forever_worlds", false, true, false, warn, sizeof warn) == EXP_NEEDS_CONFIRM);
    CHECK(strstr(warn, "walls") != NULL);
    CHECK(experimental_enabled(m, "forever_worlds"));
    m->cgm_present = true;
    CHECK(experimental_set(m, "forever_worlds", false, true, false, warn, sizeof warn) == EXP_NEEDS_CONFIRM);
    CHECK(strstr(warn, "stays in the save") != NULL);
    CHECK(experimental_set(m, "forever_worlds", false, true, true, warn, sizeof warn) == EXP_OK);
    CHECK(!experimental_enabled(m, "forever_worlds"));
    CHECK(save_meta_flush());
    save_close();

    CHECK(save_open(dir, 99));
    CHECK(!experimental_enabled(save_meta(), "forever_worlds"));
    CHECK(save_meta()->cgm_present); /* the record outlives the toggle */
    /* Enabling in a world that already has chunks warns that everything counts as epoch 0. */
    CHECK(experimental_set(save_meta(), "forever_worlds", true, true, false, warn, sizeof warn) == EXP_NEEDS_CONFIRM);
    CHECK(strstr(warn, "epoch 0") != NULL);
    CHECK(!experimental_enabled(save_meta(), "forever_worlds"));
    save_close();
    remove_tree_files(dir);
}

/* ------------------------------------------------------------ fixtures */

static u64 g_fw_seed = 12345;
void forever_test_set_seed(u64 seed) { g_fw_seed = seed; }

#define FW_ROOT "selftest_fw_epochs"
#define BLEND_R 96.0f

/* Epoch 0 is the engine as it was; 1 and 2 shift terrain, climate, caves and decoration so every blended field differs. */
static const char *const EPOCH_NOISE[3] = {
    "{\"kernel\":\"terrain_v1\"}",
    "{\"kernel\":\"terrain_v1\",\"height_scale\":1.3,\"height_offset\":6,\"continentalness_bias\":0.04,\"erosion_bias\":0.25,\"weirdness_bias\":0.3}",
    "{\"kernel\":\"terrain_v1\",\"height_scale\":0.8,\"height_offset\":-4,\"continentalness_bias\":-0.03,\"erosion_bias\":-0.2}",
};
static const char *const EPOCH_BIOMES[3] = {"{}", "{\"temperature_bias\":0.25,\"humidity_bias\":-0.15}", "{\"temperature_bias\":-0.2,\"humidity_bias\":0.2}"};
static const char *const EPOCH_CAVES[3] = {"{}", "{\"tunnel_width_scale\":1.7,\"cheese_threshold_delta\":-0.06}", "{\"tunnel_width_scale\":0.6,\"cheese_threshold_delta\":0.05}"};
static const char *const EPOCH_SURFACE[3] = {"{}", "{\"snowline_offset\":-12,\"treeline_offset\":-9}", "{\"snowline_offset\":10,\"treeline_offset\":8}"};
static const char *const EPOCH_FEATURES[3] = {"{}", "{\"tree_chance_scale\":0.5,\"plant_chance_scale\":0.6,\"ore_chance_scale\":1.6}", "{\"tree_chance_scale\":1.5,\"plant_chance_scale\":1,\"ore_chance_scale\":0.7}"};
static const char *const EPOCH_STRUCTURES[3] = {"{}", "{\"chance_scale\":1.2}", "{\"chance_scale\":0.8}"};

static void write_file(const char *path, const char *text) { CHECK(file_write_atomic(path, text, strlen(text))); }

static void write_epoch_hash(int id) {
    char dir[200], path[240], err[200], hex[65];
    u8 h[32];
    snprintf(dir, sizeof dir, FW_ROOT "/%d", id);
    CHECK(epoch_hash_dir(dir, h, err, sizeof err));
    epoch_hex(h, 32, hex);
    snprintf(path, sizeof path, "%s/hash.txt", dir);
    write_file(path, hex);
}

/* Writes epochs 0..n-1 and removes any higher ones. */
static void write_epochs(int n) {
    for (int id = 0; id < 3; id++) {
        char dir[200];
        snprintf(dir, sizeof dir, FW_ROOT "/%d", id);
        static const char *const names[] = {"noise", "biomes", "caves", "surface", "features", "structures", "hash"};
        for (int k = 0; k < 7; k++) { char f[260]; snprintf(f, sizeof f, "%s/%s.%s", dir, names[k], k == 6 ? "txt" : "json"); remove(f); }
        if (id >= n) { remove(dir); continue; }
        dir_make_all(dir);
        const char *text[6] = {EPOCH_NOISE[id], EPOCH_BIOMES[id], EPOCH_CAVES[id], EPOCH_SURFACE[id], EPOCH_FEATURES[id], EPOCH_STRUCTURES[id]};
        for (int k = 0; k < 6; k++) { char f[260]; snprintf(f, sizeof f, "%s/%s.json", dir, names[k]); write_file(f, text[k]); }
        write_epoch_hash(id);
    }
}

static void load_epochs(int n) {
    char err[400];
    write_epochs(n);
    CHECK(epoch_registry_load(FW_ROOT, err, sizeof err));
    if (epoch_count() != n) printf("  epoch load: %s\n", err);
    CHECK(epoch_count() == n);
}

static void fw_boot(bool active, int n_epochs) {
    registry_reset();
    data_error_reset();
    registry_load_blocks();
    registry_load_worldgen_config();
    cgm_store_clear();
    cgm_store_set_legacy_probe(NULL);
    forever_worlds_set_active(false);
    load_epochs(n_epochs);
    forever_worlds_set_active(active);
    gen_init(g_fw_seed);
}

static size_t col_vol(void) {
    int lo, hi;
    gen_band(&lo, &hi);
    return (size_t)(hi - lo + 1) * CHUNK_VOL;
}

static u64 hash_states(const u16 *st, size_t n) {
    u64 h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= st[i]; h *= 1099511628211ull; }
    return h;
}

/* Generates one column; commits its CGM when `commit`. Returns the content hash. */
static u64 gen_one(GenScratch *s, u16 *states, int cx, int cz, bool commit) {
    gen_column(s, cx, cz, states);
    if (commit) {
        Cgm c;
        bool over;
        if (gen_column_cgm(s, &c, &over)) cgm_store_put(cx, cz, &c);
    }
    return hash_states(states, col_vol());
}

/* An epoch-0 region: columns [0,4) x [0,4) generated and committed, then epoch 1 (and 2) are added to the registry. */
static void fw_old_region(int total_epochs) {
    fw_boot(true, 1);
    GenScratch *s = gen_scratch_create();
    u16 *st = xmalloc(col_vol() * sizeof(u16));
    for (int cz = 0; cz < 4; cz++) for (int cx = 0; cx < 4; cx++) gen_one(s, st, cx, cz, true);
    free(st);
    gen_scratch_destroy(s);
    load_epochs(total_epochs);
    forever_worlds_set_active(true);
}

static void mark_region(int x0, int z0, int w, int h, u32 epoch) {
    for (int z = z0; z < z0 + h; z++)
        for (int x = x0; x < x0 + w; x++) {
            Cgm c;
            cgm_init(&c, epoch, g_fw_seed);
            cgm_store_put(x, z, &c);
        }
}

/* --------------------------------------------------------------- feature UI */

static void test_ui_rows(void) {
    /* The submenu draws straight from the registry, so the row text is the registry text. */
    data_error_reset();
    experimental_load();
    const ExpFeatureDef *d = experimental_find("forever_worlds");
    CHECK(d && experimental_count() >= 1);
    if (!d) return;
    CHECK(d->name[0] && d->version[0] && d->released[0] && d->description[0]); /* 2: every column of the row exists */
    CHECK(!strcmp(d->released, "2026-10-04"));
    experimental_selection_clear(); /* 1: the create screen's button reads the selection count */
    CHECK(!experimental_selection_get("forever_worlds"));
    experimental_selection_set("forever_worlds", true);
    CHECK(experimental_selection_get("forever_worlds"));
    experimental_selection_clear();
}

/* ---------------------------------------------------------------------- GER */

static void test_registry_epochs(void) {
    fw_boot(false, 3);
    CHECK(epoch_count() == 3 && epoch_current() == 2);
    u8 h0[32], h1[32];
    memcpy(h0, epoch_get(0)->hash, 32);
    memcpy(h1, epoch_get(1)->hash, 32);
    char err[300];
    CHECK(epoch_registry_load(FW_ROOT, err, sizeof err)); /* 11: loading again gives the same registry */
    CHECK(!memcmp(h0, epoch_get(0)->hash, 32) && !memcmp(h1, epoch_get(1)->hash, 32));
    CHECK(memcmp(h0, h1, 32) != 0);
    CHECK(epoch_get(0)->p.identity && !epoch_get(1)->p.identity);
    CHECK(epoch_get(1)->p.height_scale == 1.3f && epoch_get(2)->p.tree_scale == 1.5f);

    /* The hash is SHA-256: known answer for "abc". */
    u8 d[32];
    char hex[65];
    sha256_bytes("abc", 3, d);
    epoch_hex(d, 32, hex);
    CHECK(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

    /* 12: a missing file names the file. */
    remove(FW_ROOT "/1/caves.json");
    CHECK(!epoch_registry_load(FW_ROOT, err, sizeof err));
    CHECK(strstr(err, "caves.json") != NULL && epoch_count() == 0);
    write_epochs(3);
    /* 13: a changed file no longer matches hash.txt. */
    write_file(FW_ROOT "/1/noise.json", "{\"kernel\":\"terrain_v1\",\"height_scale\":1.31}");
    CHECK(!epoch_registry_load(FW_ROOT, err, sizeof err));
    CHECK(strstr(err, "hash.txt") != NULL && epoch_count() == 0);
    write_epochs(3);
    /* Bad values and unknown keys are errors with the epoch and key in the message. */
    write_file(FW_ROOT "/1/noise.json", "{\"kernel\":\"terrain_v1\",\"height_scale\":9}");
    write_epoch_hash(1);
    CHECK(!epoch_registry_load(FW_ROOT, err, sizeof err) && strstr(err, "height_scale"));
    write_file(FW_ROOT "/1/noise.json", "{\"kernel\":\"terrain_v1\",\"bogus\":1}");
    write_epoch_hash(1);
    CHECK(!epoch_registry_load(FW_ROOT, err, sizeof err) && strstr(err, "bogus"));
    write_epochs(3);
    /* A gap in the ids is an error. */
    remove(FW_ROOT "/1/hash.txt");
    CHECK(!epoch_registry_load(FW_ROOT, err, sizeof err));
    write_epochs(3);
    CHECK(epoch_registry_load(FW_ROOT, err, sizeof err));
    /* The shipped epoch 0 must match its hash, through the VFS. */
    registry_reset();
    CHECK(epoch_registry_load(NULL, err, sizeof err) && epoch_count() >= 1 && epoch_get(0)->p.identity);
    epoch_registry_clear();
}

/* ---------------------------------------------------------------------- CGM */

static void test_cgm(void) {
    fw_boot(true, 2);
    /* 6: a new column records the current epoch and its hash. */
    GenScratch *s = gen_scratch_create();
    u16 *st = xmalloc(col_vol() * sizeof(u16));
    gen_column(s, 40, 40, st);
    Cgm c;
    bool over = false;
    CHECK(gen_column_cgm(s, &c, &over) && !over);
    CHECK(c.epoch_id == 1 && !memcmp(c.epoch_hash, epoch_get(1)->hash, 32) && c.seed_snapshot != 0);
    CHECK(c.terrain_len == CGM_MAX_TERRAIN_BLOB && c.biome_len == CGM_MAX_BIOME_BLOB);
    int total = 0;
    for (int i = 0; i < c.biome_len; i++) total += c.biome_weights[i];
    CHECK(total > 200 && total < 330); /* a histogram of the column's biomes, 255 per whole column */

    /* 7: records survive close and reopen byte for byte. */
    const char *dir = "selftest_fw_cgm";
    remove_tree_files(dir);
    dir_make_all(dir);
    remove("selftest_fw_cgm/cgm.dat");
    char err[300];
    CHECK(cgm_store_open(dir, err, sizeof err));
    StructureClaim claim = {.id = "base:hut", .min = {1, 2, 3}, .max = {9, 8, 7}, .seed = 77};
    CHECK(cgm_add_claim(&c, &claim) == CGM_OK);
    CHECK(cgm_store_put(5, -6, &c));
    Cgm other = c;
    other.claim_count = 0;
    CHECK(cgm_store_put(-1, 2, &other));
    CHECK(cgm_store_put(5, -6, &c)); /* newest record for a column wins */
    cgm_store_close();
    CHECK(cgm_store_open(dir, err, sizeof err));
    Cgm back;
    u8 a[CGM_MAX_BYTES], b[CGM_MAX_BYTES];
    CHECK(cgm_store_get(5, -6, &back) && cgm_store_count() == 2);
    size_t na = cgm_serialize(&c, a, sizeof a), nb = cgm_serialize(&back, b, sizeof b);
    CHECK(na && na == nb && !memcmp(a, b, na));
    CHECK(back.claim_count == 1 && !strcmp(back.claims[0].id, "base:hut") && back.claims[0].max[0] == 9);
    CHECK(!cgm_store_get(0, 0, NULL));
    cgm_store_close();

    /* 8: migration. Rewriting world.json (what a format upgrade does) leaves the sidecar intact, and a column saved
     * before the feature existed reads as epoch 0 through the probe. */
    remove_tree_files("selftest_fw_world");
    CHECK(save_open("selftest_fw_world", 5));
    save_meta()->cgm_present = true;
    CHECK(save_meta_flush());
    save_close();
    CHECK(save_open("selftest_fw_world", 5) && save_meta()->cgm_present);
    save_close();
    CHECK(cgm_store_open(dir, err, sizeof err) && cgm_store_get(-1, 2, NULL));
    cgm_store_set_legacy_probe(save_has_column);
    u32 e = 99;
    CHECK(!cgm_store_epoch(100, 100, &e)); /* nothing saved there */
    cgm_store_close();

    /* 9: a record that disagrees with the registry fails the open, naming the cause. */
    Cgm bad = c;
    bad.claim_count = 0;
    bad.epoch_hash[3] ^= 0x55;
    CHECK(cgm_store_open(dir, err, sizeof err));
    CHECK(cgm_store_put(7, 7, &bad));
    cgm_store_close();
    CHECK(!cgm_store_open(dir, err, sizeof err) && strstr(err, "hash"));
    remove("selftest_fw_cgm/cgm.dat");
    Cgm unknown = c;
    unknown.claim_count = 0;
    unknown.epoch_id = 9;
    CHECK(cgm_store_open(dir, err, sizeof err));
    cgm_store_put(1, 1, &unknown);
    cgm_store_close();
    CHECK(!cgm_store_open(dir, err, sizeof err) && strstr(err, "epoch"));
    remove("selftest_fw_cgm/cgm.dat");
    CHECK(cgm_store_open(dir, err, sizeof err));
    cgm_store_put(1, 1, &other);
    cgm_store_close();
    size_t len = 0;
    u8 *file = file_read("selftest_fw_cgm/cgm.dat", &len);
    CHECK(file && len > 20);
    if (file) {
        file[len / 2] ^= 0xFF; /* silent corruption is caught by the record checksum */
        file_write_atomic("selftest_fw_cgm/cgm.dat", file, len);
        free(file);
        CHECK(!cgm_store_open(dir, err, sizeof err) && strstr(err, "checksum"));
    }
    remove("selftest_fw_cgm/cgm.dat");

    /* 10: bounds are enforced with an error, never a truncation. */
    Cgm o;
    cgm_init(&o, 1, 1);
    u8 big[200] = {0};
    CHECK(cgm_set_terrain(&o, big, CGM_MAX_TERRAIN_BLOB + 1) == CGM_ERR_OVERFLOW);
    CHECK(cgm_set_biomes(&o, big, CGM_MAX_BIOME_BLOB + 1) == CGM_ERR_OVERFLOW);
    for (int i = 0; i < CGM_MAX_CLAIMS; i++) CHECK(cgm_add_claim(&o, &claim) == CGM_OK);
    CHECK(cgm_add_claim(&o, &claim) == CGM_ERR_OVERFLOW && o.claim_count == CGM_MAX_CLAIMS);
    u8 small[16];
    CHECK(cgm_serialize(&o, small, sizeof small) == 0);
    Cgm out;
    CHECK(cgm_deserialize(big, sizeof big, &out) == CGM_ERR_FORMAT);
    u8 wire[CGM_MAX_BYTES];
    size_t n = cgm_serialize(&o, wire, sizeof wire);
    CHECK(n > 0 && n <= CGM_MAX_BYTES && cgm_deserialize(wire, n, &out) == CGM_OK);
    CHECK(cgm_deserialize(wire, n - 1, &out) == CGM_ERR_FORMAT);
    free(st);
    gen_scratch_destroy(s);
    epoch_registry_clear();
    cgm_store_clear();
}

/* -------------------------------------------------------------- blend field */

static void test_blend_field(void) {
    fw_old_region(2);
    BlendPlan *plan;
    /* 14/24: weights are normalised and only epochs with a nearby column take part. */
    CHECK(blend_plan_build(30, 30, BLEND_R) == NULL); /* far away: nothing to blend, no plan at all */
    plan = blend_plan_build(4, 1, BLEND_R);
    CHECK(plan != NULL && plan->n_cols > 0 && plan->current == 1);
    if (!plan) return;
    int mixed = 0;
    bool normal = true, only_relevant = true;
    for (int x = 120; x < 260; x += 3)
        for (int z = 0; z < 128; z += 7) {
            BlendCell c;
            blend_weights(plan, x, z, &c);
            float sum = 0.0f;
            for (int i = 0; i < c.n; i++) { sum += c.a[i]; normal &= c.a[i] >= 0.0f && c.a[i] <= 1.0f; only_relevant &= c.id[i] <= 1; }
            normal &= fabsf(sum - 1.0f) < 1e-5f && c.n >= 1 && c.n <= EP_BLEND_MAX;
            mixed += c.n > 1;
        }
    CHECK(normal && only_relevant && mixed > 20);

    /* 18/19: along a line leaving the old region, the height displacement from the current terrain is continuous, equals
     * the older epoch's at the border and vanishes at the radius. */
    int z = 64;
    float prev = 0.0f, max_step = 0.0f, first = 0.0f, last_disp = 1e9f;
    for (int x = 128; x <= 128 + 140; x++) {
        BlendPlan *p = blend_plan_build(x >> 5, z >> 5, BLEND_R);
        float h;
        int biome;
        BlendCell c;
        gen_epoch_sample(p, x, z, &h, &biome, &c);
        float ground = gen_epoch_ground(x, z), disp = h - ground;
        if (x == 128) first = disp; else max_step = MAX(max_step, fabsf(disp - prev));
        prev = disp;
        last_disp = disp;
        if (x >= 128 + (int)BLEND_R) CHECK(p == NULL || disp == 0.0f);
        blend_plan_free(p);
    }
    float full = gen_epoch_raw(0, 128.5f, (float)z) - gen_epoch_raw(1, 128.5f, (float)z);
    CHECK(fabsf(full) > 2.0f);          /* the epochs really differ here, so the checks below mean something */
    CHECK(fabsf(first - full) < 0.1f * fabsf(full) + 0.2f);
    if (max_step >= 2.0f) printf("  max_step %f first %f full %f\n", max_step, first, full);
    CHECK(max_step < 2.0f);
    CHECK(last_disp == 0.0f);

    /* Scalar blend check: at mid-distance the height is the weighted mix of the two epochs' fields. */
    {
        int x = 128 + 40;
        BlendPlan *p = blend_plan_build(x >> 5, z >> 5, BLEND_R);
        float h;
        int biome;
        BlendCell c;
        gen_epoch_sample(p, x, z, &h, &biome, &c);
        CHECK(c.n == 2 && c.id[0] == 1 && c.id[1] == 0);
        float per[2] = {0.0f, gen_epoch_raw(0, (float)x, (float)z) - gen_epoch_raw(1, (float)x, (float)z)};
        CHECK(fabsf(h - (gen_epoch_ground(x, z) + blend_scalar(&c, per))) < 1e-4f);
        CHECK(c.a[0] > 0.0f && c.a[1] > 0.0f);
        float vec_in[4] = {1, 0, 0, 1}, vec_out[2];
        blend_vector(&c, vec_in, 2, vec_out); /* 20: a vote vector of one-hot biome weights keeps unit mass */
        CHECK(fabsf(vec_out[0] + vec_out[1] - 1.0f) < 1e-5f);
        blend_plan_free(p);
    }

    /* 20: the biome is one an epoch itself would choose, the older one at the border and the current one past the radius. */
    {
        int differ = 0;
        bool allowed = true;
        for (int x = 128; x < 128 + 130; x += 2)
            for (int zz = 8; zz < 120; zz += 16) {
                BlendPlan *p = blend_plan_build(x >> 5, zz >> 5, BLEND_R);
                float h;
                int biome;
                BlendCell c;
                gen_epoch_sample(p, x, zz, &h, &biome, &c);
                float g = gen_epoch_ground(x, zz);
                int b0 = gen_epoch_biome(0, (float)x, (float)zz, g + gen_epoch_raw(0, (float)x, (float)zz) - gen_epoch_raw(1, (float)x, (float)zz));
                int b1 = gen_epoch_biome(1, (float)x, (float)zz, g);
                allowed &= biome == b0 || biome == b1;
                differ += b0 != b1;
                if (x >= 128 + (int)BLEND_R) allowed &= biome == b1;
                blend_plan_free(p);
            }
        CHECK(allowed && differ > 0);
    }

    /* 21: cave density is the weighted sum of each epoch's own field at the lattice point. */
    {
        bool ok = true;
        int checked = 0;
        for (int i = 0; i < 40; i++) {
            float x = 130.0f + (float)(i * 3), y = 20.0f + (float)(i % 5) * 4.0f, zc = 40.0f + (float)(i % 7) * 9.0f;
            BlendPlan *p = blend_plan_build((int)x >> 5, (int)zc >> 5, BLEND_R);
            if (!p) continue;
            BlendCell c;
            blend_weights(p, (int)x, (int)zc, &c);
            float per[EP_BLEND_MAX], want;
            for (int k = 0; k < c.n; k++) per[k] = gen_epoch_cave(c.id[k], x, y, zc);
            want = blend_scalar(&c, per);
            ok &= fabsf(gen_blend_cave(p, x, y, zc) - want) < 1e-5f;
            checked++;
            blend_plan_free(p);
        }
        CHECK(ok && checked > 20);
        CHECK(fabsf(gen_epoch_cave(0, 150, 24, 60) - gen_epoch_cave(1, 150, 24, 60)) > 1e-3f);
    }

    /* The cached plan is shared and rebuilt when the registry generation moves. */
    u64 builds = g_blend_plan_builds;
    BlendPlan *a = blend_plan_acquire(4, 1), *b2 = blend_plan_acquire(4, 1);
    CHECK(a && a == b2 && g_blend_plan_builds == builds + 1);
    blend_plan_release(a);
    blend_plan_release(b2);
    epoch_invalidate();
    a = blend_plan_acquire(4, 1);
    CHECK(a && g_blend_plan_builds == builds + 2);
    blend_plan_release(a);
    blend_plan_free(plan);

    /* 14: three epochs. Epoch 0 on the left, epoch 1 on the right; the point between sees both, a point near only one sees one. */
    fw_boot(true, 3);
    mark_region(0, 0, 4, 4, 0);
    mark_region(9, 0, 4, 4, 1);
    plan = blend_plan_build(6, 1, BLEND_R);
    CHECK(plan != NULL);
    if (plan) {
        BlendCell c;
        blend_weights(plan, 150, 40, &c); /* 22 blocks from epoch 0, 138 from epoch 1: only 0 and the current epoch */
        bool has1 = false;
        for (int i = 0; i < c.n; i++) has1 |= c.id[i] == 1;
        CHECK(c.n == 2 && !has1);
        blend_weights(plan, 205, 40, &c); /* 77 from epoch 0, 83 from epoch 1: both plus the current epoch */
        CHECK(c.n == 3 && c.id[0] == 2);
        float sum = 0.0f;
        for (int i = 0; i < c.n; i++) sum += c.a[i];
        CHECK(fabsf(sum - 1.0f) < 1e-5f);
        blend_weights(plan, 3 * 32 + 5, 40, &c); /* inside an old column: that epoch exactly */
        CHECK(c.n == 1 && c.id[0] == 0 && c.a[0] == 1.0f);
        blend_plan_free(plan);
    }
    epoch_registry_clear();
    cgm_store_clear();
}

/* ------------------------------------------------------------- determinism */

typedef struct ThreadJob { int index, threads, n; int cx0, cz0, w; u64 *hashes; } ThreadJob;

static void thread_body(void *arg) {
    ThreadJob *j = arg;
    GenScratch *s = gen_scratch_create();
    u16 *st = xmalloc(col_vol() * sizeof(u16));
    for (int i = j->index; i < j->n; i += j->threads) j->hashes[i] = gen_one(s, st, j->cx0 + i % j->w, j->cz0 + i / j->w, false);
    free(st);
    gen_scratch_destroy(s);
}

static void gen_grid(int cx0, int cz0, int w, int h, bool reverse, int threads, u64 *out) {
    int n = w * h;
    if (threads <= 1) {
        GenScratch *s = gen_scratch_create();
        u16 *st = xmalloc(col_vol() * sizeof(u16));
        for (int k = 0; k < n; k++) {
            int i = reverse ? n - 1 - k : k;
            out[i] = gen_one(s, st, cx0 + i % w, cz0 + i / w, false);
        }
        free(st);
        gen_scratch_destroy(s);
        return;
    }
    ThreadJob jobs[8];
    Thread *t[8];
    for (int k = 0; k < threads; k++) { jobs[k] = (ThreadJob){k, threads, n, cx0, cz0, w, out}; t[k] = thread_start(thread_body, &jobs[k]); }
    for (int k = 0; k < threads; k++) thread_join(t[k]);
}

static void test_determinism(void) {
    enum { W = 5, H = 3, N = W * H };
    u64 first[N], again[N], rev[N], threaded[N], four[N];
    fw_old_region(3);
    mark_region(10, 0, 2, 2, 1); /* a second older epoch to the east so three epochs can meet */
    gen_grid(4, 0, W, H, false, 1, first);
    bool varied = false;
    for (int i = 1; i < N; i++) varied |= first[i] != first[0];
    CHECK(varied);
    /* 15/32: the same inputs give the same terrain, from a cold start. */
    fw_old_region(3);
    mark_region(10, 0, 2, 2, 1);
    gen_grid(4, 0, W, H, false, 1, again);
    CHECK(!memcmp(first, again, sizeof first));
    /* 16/33: generation order does not matter. */
    gen_grid(4, 0, W, H, true, 1, rev);
    CHECK(!memcmp(first, rev, sizeof first));
    /* 17/33: nor does the number of threads. */
    gen_grid(4, 0, W, H, false, 2, threaded);
    CHECK(!memcmp(first, threaded, sizeof first));
    gen_grid(4, 0, W, H, true, 4, four);
    CHECK(!memcmp(first, four, sizeof first));
    /* Another blend radius changes the result, the same radius repeats it. */
    float radius = epoch_blend_radius();
    epoch_set_blend_radius(48.0f);
    u64 narrow[N], narrow2[N];
    gen_grid(4, 0, W, H, false, 1, narrow);
    gen_grid(4, 0, W, H, true, 1, narrow2);
    CHECK(!memcmp(narrow, narrow2, sizeof narrow) && memcmp(narrow, first, sizeof first) != 0);
    epoch_set_blend_radius(10.0f);
    CHECK(epoch_blend_radius() == EP_RADIUS_MIN);
    epoch_set_blend_radius(1000.0f);
    CHECK(epoch_blend_radius() == EP_RADIUS_MAX);
    epoch_set_blend_radius(radius);
    CHECK(epoch_blend_radius() == EP_RADIUS_DEFAULT);
    epoch_registry_clear();
    cgm_store_clear();
}

/* ----------------------------------------------------------------- gating */

static void test_gating(void) {
    /* 30: with the feature off nothing is recorded, no plan is built, and the terrain is the original terrain. */
    enum { N = 6 };
    u64 off[N], ident[N];
    fw_boot(false, 2);
    u64 builds = g_blend_plan_builds;
    GenScratch *s = gen_scratch_create();
    u16 *st = xmalloc(col_vol() * sizeof(u16));
    Cgm c;
    bool over;
    for (int i = 0; i < N; i++) { off[i] = gen_one(s, st, i, 2, true); CHECK(!gen_column_cgm(s, &c, &over)); }
    CHECK(cgm_store_count() == 0 && g_blend_plan_builds == builds);
    /* On with only the identity epoch: same bits, so enabling the feature alone changes nothing. */
    fw_boot(true, 1);
    for (int i = 0; i < N; i++) ident[i] = gen_one(s, st, i, 2, false);
    CHECK(!memcmp(off, ident, sizeof off));
    CHECK(gen_column_cgm(s, &c, &over) && c.epoch_id == 0);
    free(st);
    gen_scratch_destroy(s);
    epoch_registry_clear();
}

/* -------------------------------------------------------------- structures */

static u16 test_block(const char *a, const char *b) {
    BlockDef *d = block_find(a);
    if (!d) d = block_find(b);
    return d ? d->default_state : STATE_AIR;
}

static size_t at_of(int ly, int x, int z) { return ((size_t)ly << 10) | (size_t)((z << 5) | x); }

static void test_structures(void) {
    fw_boot(true, 2);
    gen_test_clear_structures();
    u16 plank = test_block("base:planks", "base:log"), stone = test_block("base:stone", "base:dirt"), water = test_block("base:water", "base:stone");
    int lo, hi;
    gen_band(&lo, &hi);
    int y0 = lo * CHUNK_SIZE, H = (hi - lo + 1) * CHUNK_SIZE, ground = 70;
    u16 *st = xmalloc(col_vol() * sizeof(u16));
    GenScratch *s = gen_scratch_create();
    static const int post[3][3] = {{0, 0, 0}, {0, 1, 0}, {0, 2, 0}};
    const u16 post_states[3] = {plank, plank, plank};
    BlendPlan plan = {.current = 1, .radius = BLEND_R};

    /* A structure with every policy; the column is a hand-made flat field so the outcome is exact. */
    #define FILL(top_solid, wet_top) do { for (int ly = 0; ly < H; ly++) for (int z = 0; z < CHUNK_SIZE; z++) for (int x = 0; x < CHUNK_SIZE; x++) \
        st[at_of(ly, x, z)] = ly + y0 <= (top_solid) ? stone : (ly + y0 <= (wet_top) ? water : STATE_AIR); } while (0)

    /* 25: placed structures leave a claim whose box holds the anchor. */
    gen_test_add_structure("test:post", 100, 3, 1, STATE_MISSING, post, post_states, 3);
    FILL(ground, ground);
    gen_test_place_structures(s, &plan, st, 0, 0, ground, false, false);
    StructureClaim cl[CGM_MAX_CLAIMS];
    int ncl = gen_test_claims(s, cl, CGM_MAX_CLAIMS);
    CHECK(ncl == 1 && !strcmp(cl[0].id, "test:post"));
    CHECK(cl[0].min[0] == 0 && cl[0].max[0] == CHUNK_SIZE - 1 && cl[0].min[1] == ground + 1 && cl[0].max[1] == ground + 3);
    CHECK(st[at_of(ground + 1 - y0, 16, 16)] == plank && st[at_of(ground + 3 - y0, 16, 16)] == plank);

    /* 22/26: older epochs' structure boxes are exclusion zones for new structures. */
    StructureClaim claim = {.id = "old:keep", .min = {10, 0, 10}, .max = {20, 200, 20}, .seed = 1};
    plan.claims = &claim;
    plan.n_claims = 1;
    FILL(ground, ground);
    gen_test_place_structures(s, &plan, st, 0, 0, ground, false, false);
    CHECK(st[at_of(ground + 1 - y0, 16, 16)] != plank);   /* the post sits inside the old box, so it is skipped */
    claim.min[0] = claim.min[2] = 24;
    claim.max[0] = claim.max[2] = 30;
    FILL(ground, ground);
    gen_test_place_structures(s, &plan, st, 0, 0, ground, false, false);
    CHECK(st[at_of(ground + 1 - y0, 16, 16)] == plank);   /* a box elsewhere does not block it */
    claim.min[0] = claim.min[2] = 10;
    claim.max[0] = claim.max[2] = 20;
    FILL(ground, ground);
    gen_test_place_structures(s, &plan, st, 0, 0, ground, false, false);
    bool clear = true;
    for (int z = 10; z <= 20; z++) for (int x = 10; x <= 20; x++) for (int y = 0; y < 4; y++) clear &= st[at_of(ground + 1 + y - y0, x, z)] != plank;
    CHECK(clear);
    plan.claims = NULL;
    plan.n_claims = 0;

    /* 27: water. ignore drops the submerged blocks; raise lifts the structure onto a foundation; carve_foundation drains
     * its cells and sets a platform; flood fills its box. Wet ground is 70 with the water plane at 73. */
    u16 found = stone;
    for (int policy = 0; policy < 4; policy++) {
        gen_test_clear_structures();
        gen_test_add_structure("test:post", 100, policy, 1, found, post, post_states, 3);
        FILL(ground, ground + 3);
        gen_test_place_structures(s, &plan, st, 0, 0, ground, true, false);
        u16 y71 = st[at_of(71 - y0, 16, 16)], y73 = st[at_of(73 - y0, 16, 16)], y74 = st[at_of(74 - y0, 16, 16)];
        if (policy == 0) { CHECK(y74 == plank && y73 == found && st[at_of(77 - y0, 16, 16)] == STATE_AIR); }
        if (policy == 1) { CHECK(y71 == plank && st[at_of(70 - y0, 16, 16)] == found); }
        if (policy == 2) { CHECK(y71 == STATE_AIR || y71 == water); CHECK(y73 == water); }
        if (policy == 3) { CHECK(y71 == water && y73 == water); }
        if (policy == 2) { /* the open cells of the box below the waterline are water, nothing else */
            CHECK(st[at_of(72 - y0, 16, 16)] == water);
        }
    }
    /* 28: solid. The terrain is higher than the height the structure was placed for. */
    for (int policy = 0; policy < 3; policy++) {
        gen_test_clear_structures();
        gen_test_add_structure("test:post", 100, 3, policy, STATE_MISSING, post, post_states, 3);
        FILL(ground + 6, ground + 6);
        gen_test_place_structures(s, &plan, st, 0, 0, ground, false, false);
        u16 mid = st[at_of(72 - y0, 16, 16)];
        if (policy == 0) CHECK(mid == plank);      /* carve: the structure overwrites the rock */
        if (policy == 1) CHECK(mid == stone);      /* ignore: rock stays, the structure is not built there */
        if (policy == 2) CHECK(mid == stone && gen_test_claims(s, cl, CGM_MAX_CLAIMS) == 0); /* reject: no structure, no claim */
    }
    free(st);
    gen_scratch_destroy(s);
    #undef FILL
    epoch_registry_clear();
}

static bool floating_plant(const u16 *st, u16 tall, u16 flower) {
    int lo, hi;
    gen_band(&lo, &hi);
    int H = (hi - lo + 1) * CHUNK_SIZE;
    for (int ly = 1; ly < H; ly++)
        for (int col = 0; col < CHUNK_AREA; col++) {
            u16 a = st[((size_t)ly << 10) | col];
            if ((a == tall || a == flower) && a != STATE_AIR && st[((size_t)(ly - 1) << 10) | col] == STATE_AIR) return true;
        }
    return false;
}

/* 23: a carve policy that cuts out the ground under a plant strands it; the seam pass removes it and re-runs decoration. */
static void test_seam_carving(void) {
    fw_old_region(2);
    gen_test_clear_structures();
    static const int cut[2][3] = {{0, -1, 0}, {0, 0, 0}};
    const u16 cut_states[2] = {STATE_AIR, STATE_AIR};
    gen_test_add_structure("test:pit", 100, 3, 0, STATE_MISSING, cut, cut_states, 2);
    u16 tall = test_block("base:tall_grass", "base:grass"), flower = test_block("base:flower_red", "base:grass");
    GenScratch *s = gen_scratch_create();
    u16 *st = xmalloc(col_vol() * sizeof(u16));
    u64 before = gen_orphans_removed();
    bool floating = false;
    for (int cx = 4; cx < 8; cx++)
        for (int cz = 0; cz < 4; cz++) {
            gen_column(s, cx, cz, st);
            floating |= floating_plant(st, tall, flower);
        }
    CHECK(!floating);
    (void)before;
    /* A hand-made column: grass on stone, a pit carved under one blade, and a leaf with no wood. */
    {
        int lo, hi;
        gen_band(&lo, &hi);
        int y0 = lo * CHUNK_SIZE, H = (hi - lo + 1) * CHUNK_SIZE, ground = 70;
        u16 stone = test_block("base:stone", "base:dirt"), leaves = test_block("base:leaves", "base:stone");
        for (int ly = 0; ly < H; ly++) for (int i = 0; i < CHUNK_AREA; i++) st[((size_t)ly << 10) | i] = ly + y0 <= ground ? stone : STATE_AIR;
        st[at_of(ground + 1 - y0, 5, 5)] = tall;
        st[at_of(ground + 1 - y0, 9, 9)] = tall;
        st[at_of(ground + 5 - y0, 20, 20)] = leaves;
        st[at_of(ground - y0, 5, 5)] = STATE_AIR; /* the carve */
        u64 b = gen_orphans_removed();
        gen_test_clear_orphans(s, st, 0, 0);
        CHECK(gen_orphans_removed() >= b + 1);
        CHECK(st[at_of(ground + 1 - y0, 5, 5)] == STATE_AIR || st[at_of(ground + 1 - y0, 5, 5)] == tall);
        CHECK(!(st[at_of(ground + 1 - y0, 5, 5)] == tall)); /* no ground under it, so the re-run cannot restore it */
        CHECK(st[at_of(ground + 5 - y0, 20, 20)] == STATE_AIR || leaves == STATE_AIR);
        CHECK(!floating_plant(st, tall, flower));
    }

    /* The min-cut itself: a brute-force check on small random graphs, then the seam falls on the cheap edges. */
    uint32_t rng = 12345;
    bool all_match = true;
    for (int trial = 0; trial < 60; trial++) {
        enum { W = 3, Hh = 3, CELLS = W * Hh };
        int ch[(W - 1) * Hh], cv[W * (Hh - 1)];
        u8 forced[CELLS] = {0}, label[CELLS];
        for (int i = 0; i < (W - 1) * Hh; i++) { rng = rng * 1664525u + 1013904223u; ch[i] = (int)(rng >> 24) % 9; }
        for (int i = 0; i < W * (Hh - 1); i++) { rng = rng * 1664525u + 1013904223u; cv[i] = (int)(rng >> 24) % 9; }
        forced[0] = 1;
        forced[CELLS - 1] = 2;
        i64 flow = seam_mincut(W, Hh, ch, cv, forced, label);
        i64 best = INT64_MAX;
        for (int mask = 0; mask < (1 << CELLS); mask++) {
            if (!(mask & 1) || (mask >> (CELLS - 1)) & 1) continue;
            i64 cost = 0;
            for (int y = 0; y < Hh; y++)
                for (int x = 0; x < W; x++) {
                    int i = y * W + x, a = (mask >> i) & 1;
                    if (x + 1 < W && a != ((mask >> (i + 1)) & 1)) cost += ch[y * (W - 1) + x];
                    if (y + 1 < Hh && a != ((mask >> (i + W)) & 1)) cost += cv[y * W + x];
                }
            best = MIN(best, cost);
        }
        i64 cut_cost = 0;
        for (int y = 0; y < Hh; y++)
            for (int x = 0; x < W; x++) {
                int i = y * W + x;
                if (x + 1 < W && label[i] != label[i + 1]) cut_cost += ch[y * (W - 1) + x];
                if (y + 1 < Hh && label[i] != label[i + W]) cut_cost += cv[y * W + x];
            }
        all_match &= flow == best && cut_cost == best && label[0] == 1 && label[CELLS - 1] == 0;
    }
    CHECK(all_match);
    /* A wall of cheap edges between two expensive halves: the cut passes through the cheap column. */
    {
        enum { W = 6, Hh = 4 };
        int ch[(W - 1) * Hh], cv[W * (Hh - 1)];
        u8 forced[W * Hh] = {0}, label[W * Hh];
        for (int i = 0; i < (W - 1) * Hh; i++) ch[i] = 50;
        for (int i = 0; i < W * (Hh - 1); i++) cv[i] = 50;
        for (int y = 0; y < Hh; y++) { ch[y * (W - 1) + 2] = 1; forced[y * W] = 1; forced[y * W + W - 1] = 2; }
        CHECK(seam_mincut(W, Hh, ch, cv, forced, label) == Hh);
        bool seam_ok = true;
        for (int y = 0; y < Hh; y++) seam_ok &= label[y * W + 2] == 1 && label[y * W + 3] == 0;
        CHECK(seam_ok);
    }
    free(st);
    gen_scratch_destroy(s);
    epoch_registry_clear();
    cgm_store_clear();
}

/* -------------------------------------------------------------- performance */

static void test_performance(void) {
    enum { N = 12 };
    fw_old_region(3);
    mark_region(10, 0, 2, 4, 1);
    GenScratch *s = gen_scratch_create();
    u16 *st = xmalloc(col_vol() * sizeof(u16));
    /* 29: a blended column samples a bounded number of epochs (at most EP_BLEND_MAX per block) and builds one plan. */
    u64 builds = g_blend_plan_builds;
    double t0 = time_now_s();
    for (int i = 0; i < N; i++) gen_one(s, st, 4 + i % 4, i / 4, false);
    double blended = time_now_s() - t0;
    CHECK(g_blend_plan_builds - builds <= (u64)N);
    BlendPlan *p = blend_plan_build(5, 1, BLEND_R);
    CHECK(p && p->n_cols < 4 * (2 * (int)(BLEND_R / CHUNK_SIZE + 2) + 1) * 2);
    blend_plan_free(p);
    /* 31: a synthetic world with three epochs meeting stays within a small multiple of an unblended column. */
    forever_worlds_set_active(false);
    t0 = time_now_s();
    for (int i = 0; i < N; i++) gen_one(s, st, 4 + i % 4, i / 4, false);
    double plain = time_now_s() - t0;
    printf("  forever-worlds blend cost: %.1f ms/column blended, %.1f ms/column plain (x%.2f)\n", 1000 * blended / N, 1000 * plain / N, blended / MAX(plain, 1e-9));
    CHECK(blended < plain * 6.0 + 0.05);
    free(st);
    gen_scratch_destroy(s);
    epoch_registry_clear();
    cgm_store_clear();
}

void test_forever_worlds(void) {
    remove_tree_files("selftest_fw_cgm");
    test_feature_registry();
    test_feature_persistence();
    test_ui_rows();
    test_registry_epochs();
    test_cgm();
    test_blend_field();
    test_determinism();
    test_gating();
    test_structures();
    test_seam_carving();
    test_performance();
    forever_worlds_set_active(false);
}

/* ./build/dfe --headless --forever-bench --output FILE: generation cost per column for the three regimes that matter. */
static double bench_cols(GenScratch *s, u16 *st, int n) {
    double best = 1e9;
    for (int rep = 0; rep < 3; rep++) {
        double t0 = time_now_s();
        for (int i = 0; i < n; i++) gen_one(s, st, 4 + i % 4, i / 4, false);
        double dt = time_now_s() - t0;
        if (dt < best) best = dt;
    }
    return 1000.0 * best / n;
}

int forever_benchmark(const char *path, u64 seed) {
    if (seed) g_fw_seed = seed;
    enum { N = 16 };
    double off, ident, blend2, blend3;
    GenScratch *s;
    u16 *st;
    fw_boot(false, 1);
    s = gen_scratch_create();
    st = xmalloc(col_vol() * sizeof(u16));
    off = bench_cols(s, st, N);
    fw_boot(true, 1);
    ident = bench_cols(s, st, N);
    fw_old_region(2);
    blend2 = bench_cols(s, st, N);
    fw_old_region(3);
    mark_region(10, 0, 2, 4, 1);
    blend3 = bench_cols(s, st, N);
    free(st);
    gen_scratch_destroy(s);
    epoch_registry_clear();
    cgm_store_clear();
    forever_worlds_set_active(false);
    char json[800];
    snprintf(json, sizeof json,
             "{\n  \"seed\": %llu,\n  \"columns\": %d,\n  \"ms_per_column\": {\n    \"feature_off\": %.4f,\n    \"epoch0_only\": %.4f,\n"
             "    \"blend_two_epochs\": %.4f,\n    \"blend_three_epochs\": %.4f\n  }\n}\n",
             (unsigned long long)g_fw_seed, N, off, ident, blend2, blend3);
    printf("%s", json);
    if (path && *path && !file_write_atomic(path, json, strlen(json))) { fprintf(stderr, "cannot write %s\n", path); return 1; }
    return 0;
}
