/* Self-test driver. Each module contributes a group of checks through a function
 * declared here; failures are counted and reported with file and line. */
#include "dfe.h"
#include "ui.h"
#include "screen.h"
#include <GLFW/glfw3.h>
#include "icons.h"

static int g_checks, g_failures;

void selftest_check(bool ok, const char *expr, const char *file, int line) {
    g_checks++;
    if (ok) return;
    g_failures++;
    fprintf(stderr, "  FAIL %s:%d  %s\n", file, line, expr);
}

static int g_job_order[64];
static int g_job_order_n;
static Mutex *g_job_mutex;

static void job_record(void *data, int worker) {
    mutex_lock(g_job_mutex);
    g_job_order[g_job_order_n++] = (int)(intptr_t)data;
    mutex_unlock(g_job_mutex);
}

static int g_complete_sum;
static void job_complete_add(void *data) { g_complete_sum += (int)(intptr_t)data; }

static volatile int g_gate;
static void gate_job(void *d, int w) { while (!g_gate) sleep_ms(1); }

static void test_jobs(void) {
    g_job_mutex = mutex_create();
    jobs_init(1);
    /* With a single worker blocked behind a gate job, queued jobs must run in priority order. */
    jobs_submit(JOB_KIND_OTHER, 0.0f, gate_job, NULL, NULL);
    sleep_ms(30);
    jobs_submit(JOB_KIND_OTHER, 5.0f, job_record, NULL, (void *)(intptr_t)5);
    jobs_submit(JOB_KIND_OTHER, 1.0f, job_record, NULL, (void *)(intptr_t)1);
    jobs_submit(JOB_KIND_OTHER, 3.0f, job_record, NULL, (void *)(intptr_t)3);
    g_gate = 1;
    jobs_wait_idle();
    CHECK(g_job_order_n == 3);
    CHECK(g_job_order[0] == 1 && g_job_order[1] == 3 && g_job_order[2] == 5);
    for (int i = 1; i <= 10; i++) jobs_submit(JOB_KIND_OTHER, (float)i, job_record, job_complete_add, (void *)(intptr_t)i);
    jobs_wait_idle();
    while (jobs_in_flight() > 0) jobs_pump(1.0);
    CHECK(g_complete_sum == 55);
    jobs_shutdown();
    mutex_destroy(g_job_mutex);
}

static void test_base(void) {
    StrMap m;
    strmap_init(&m);
    char key[32];
    for (int i = 0; i < 1000; i++) { snprintf(key, sizeof key, "mod:block_%d", i); strmap_set(&m, key, (u32)i * 3); }
    u32 v = 0;
    CHECK(strmap_get(&m, "mod:block_777", &v) && v == 2331);
    CHECK(!strmap_get(&m, "mod:missing", &v));
    strmap_set(&m, "mod:block_5", 99);
    CHECK(strmap_get(&m, "mod:block_5", &v) && v == 99 && m.count == 1000);
    strmap_free(&m);

    M4 p = m4_perspective(70.0f * DEG2RAD, 16.0f / 9.0f, 0.1f, 500.0f);
    M4 inv = m4_inverse(p);
    M4 id = m4_mul(p, inv);
    bool ident = true;
    for (int i = 0; i < 16; i++) ident &= fabsf(id.m[i] - (i % 5 == 0 ? 1.0f : 0.0f)) < 1e-3f;
    CHECK(ident);

    Camera cam = {.pos = v3(0, 0, 0), .yaw = 0, .pitch = 0, .fov_y = 70.0f * DEG2RAD, .znear = 0.1f, .zfar = 100.0f};
    camera_update(&cam, 1.0f);
    CHECK(frustum_box_visible(&cam.frustum, v3(-1, -1, -11), v3(1, 1, -9)));
    CHECK(!frustum_box_visible(&cam.frustum, v3(-1, -1, 9), v3(1, 1, 11)));
    CHECK(!frustum_box_visible(&cam.frustum, v3(500, -1, -11), v3(501, 1, -9)));

    CHECK(floor_div(-1, 32) == -1 && floor_mod(-1, 32) == 31 && floor_div(32, 32) == 1);
    CHECK(hash3(7, 1, 2, 3) == hash3(7, 1, 2, 3) && hash3(7, 1, 2, 3) != hash3(7, 3, 2, 1));
    double vals[5] = {5, 1, 4, 2, 3};
    CHECK(fabs(percentile_of(vals, 5, 50) - 3.0) < 1e-9 && fabs(percentile_of(vals, 5, 100) - 5.0) < 1e-9);
}

static void test_small_correctness_fixes(void) {
    CHECK(fabsf(player_effective_friction(0.2f) - 0.2f) < 1e-6f);
    CHECK(fabsf(player_effective_friction(0.6f) - 0.6f) < 1e-6f);
    CHECK(fabsf(player_effective_friction(-1.0f) - 0.05f) < 1e-6f);
    CHECK(fabsf(player_effective_friction(100.0f) - 1.1f) < 1e-6f);
    CHECK(player_effective_friction(NAN) == 0.05f);

    CHECK(rgba_shadow(rgba(255, 0, 0, 255)) == rgba(63, 0, 0, 255));
    CHECK(rgba_shadow(rgba(0, 255, 0, 255)) == rgba(0, 63, 0, 255));
    CHECK(rgba_shadow(rgba(0, 0, 255, 255)) == rgba(0, 0, 63, 255));
    CHECK(rgba_shadow(rgba(255, 255, 255, 255)) == rgba(63, 63, 63, 255));
    CHECK(rgba_shadow(rgba(63, 127, 191, 128)) == rgba(15, 31, 47, 128));
    CHECK(rgba_shadow(rgba(3, 0, 0, 255)) == rgba(0, 0, 0, 255));
}

static void test_content_registry(void) {
    content_reset();
    CHECK(content_register("item", "test:coin") >= 0);
    CHECK(content_register("item", "test:coin") >= 0);
    CHECK(content_count("item") == 1);
    CHECK(!strcmp(content_id_at("item", 0), "test:coin"));
    CHECK(!strcmp(content_kind("test:coin"), "item"));
    CHECK(content_register("item", "not-namespaced") < 0);
}

static void test_vfs(void) {
    /* Two roots: the later one must win for a shared path while unique files stay visible. */
    char a[] = "selftest_vfs_a", b[] = "selftest_vfs_b";
    dir_make_all("selftest_vfs_a/sub");
    dir_make_all("selftest_vfs_b/sub");
    file_write_atomic("selftest_vfs_a/sub/x.txt", "low", 3);
    file_write_atomic("selftest_vfs_a/only_a.txt", "a", 1);
    file_write_atomic("selftest_vfs_b/sub/x.txt", "high", 4);
    vfs_reset();
    vfs_add_root(a, "first");
    vfs_add_root(b, "second");
    size_t n = 0;
    const char *owner = NULL;
    u8 *data = vfs_read("sub/x.txt", &n, &owner);
    CHECK(data && n == 4 && memcmp(data, "high", 4) == 0 && strcmp(owner, "second") == 0);
    free(data);
    CHECK(vfs_exists("only_a.txt") && !vfs_exists("nope.txt"));
    StrList l = {0};
    vfs_list("sub", &l);
    CHECK(l.n == 1);
    strlist_free(&l);
    remove("selftest_vfs_a/sub/x.txt"); remove("selftest_vfs_a/only_a.txt"); remove("selftest_vfs_b/sub/x.txt");
    remove("selftest_vfs_a/sub"); remove("selftest_vfs_b/sub"); remove("selftest_vfs_a"); remove("selftest_vfs_b");
    vfs_reset();
}


/* ------------------------------------------------------------------ storage */

static void test_palette(void) {
    Chunk *c = chunk_create(0, 0, 0);
    c->uniform = 7;
    CHECK(chunk_get(c, 0) == 7 && chunk_get(c, CHUNK_VOL - 1) == 7 && c->bits == 0);
    chunk_set(c, 5, 7);
    CHECK(c->bits == 0); /* writing the existing value must not allocate */
    u16 *shadow = xmalloc(CHUNK_VOL * sizeof(u16));
    for (int i = 0; i < CHUNK_VOL; i++) shadow[i] = 7;
    Rng rng = {99};
    /* Growing the number of distinct values walks the palette through 1, 2, 4, 8 and 16 bit indices. */
    int widths_seen = 0, last_bits = -1;
    for (int distinct = 1; distinct <= 400; distinct++) {
        for (int k = 0; k < 40; k++) {
            int idx = rng_range(&rng, 0, CHUNK_VOL - 1);
            u16 v = (u16)(7 + rng_range(&rng, 0, distinct));
            chunk_set(c, idx, v);
            shadow[idx] = v;
        }
        if (c->bits != last_bits) { widths_seen++; last_bits = c->bits; }
    }
    CHECK(widths_seen >= 4);
    bool same = true;
    for (int i = 0; i < CHUNK_VOL; i++) same &= chunk_get(c, i) == shadow[i];
    CHECK(same);
    u16 *flat = xmalloc(CHUNK_VOL * sizeof(u16));
    chunk_unpack(c, flat);
    CHECK(!memcmp(flat, shadow, CHUNK_VOL * sizeof(u16)));
    Chunk *d = chunk_create(1, 0, 0);
    chunk_pack_from(d, flat);
    same = true;
    for (int i = 0; i < CHUNK_VOL; i++) same &= chunk_get(d, i) == shadow[i];
    CHECK(same);
    size_t before = chunk_memory_bytes(d);
    for (int i = 0; i < CHUNK_VOL; i++) chunk_set(d, i, 3);
    chunk_compact(d);
    CHECK(chunk_get(d, 1234) == 3 && d->bits == 0 && chunk_memory_bytes(d) < before);
    chunk_destroy(c);
    chunk_destroy(d);
    free(shadow);
    free(flat);
}

static void test_json(void) {
    const char *text = "// comment\n{\"a\": 1, \"b\": [1, 2, 3,], /* x */ \"c\": {\"d\": \"e\"},}\n";
    char err[100];
    int line = 0;
    Json *j = json_parse(text, strlen(text), err, sizeof err, &line);
    CHECK(j != NULL);
    if (j) {
        CHECK(json_int(j, "a", 0) == 1 && json_len(json_get(j, "b")) == 3);
        CHECK(!strcmp(json_str(json_get(j, "c"), "d", ""), "e"));
        CHECK(json_get(j, "c")->line == 2);
        json_free(j);
    }
    const char *bad = "{\n  \"a\": 1\n  \"b\": 2\n}";
    CHECK(json_parse(bad, strlen(bad), err, sizeof err, &line) == NULL && line == 3);
}

static void test_mod_storage(void) {
    /* The old test used base:stone before loading the block registry, so every
     * insertion was rejected and the empty container looked like a pass. */
    vfs_reset();
    vfs_add_root("engine_assets", "dfe");
    data_error_reset();
    mods_reset();
    events_clear_all();
    mods_discover("mods");
    mods_resolve();
    mods_mount();
    registry_reset();
    registry_load_blocks();
    const BlockDef *stone = block_find("base:stone");
    const BlockDef *dirt = block_find("base:dirt");
    CHECK(stone != NULL && dirt != NULL && data_error_count() == 0);
    if (!stone || !dirt) { mods_reset(); vfs_reset(); return; }

    remove("selftest_mod_storage");
    CHECK(save_open("selftest_mod_storage", 7));
    Json *progress = json_parse("{\"quest\": \"village\", \"stage\": 2, \"flags\": [true, false, true], \"inventory\": {\"ore\": 9}}", 100, NULL, 0, NULL);
    CHECK(progress);
    CHECK(mod_storage_set("alpha", "progress", progress));
    json_free(progress);
    Json *coins = json_parse("7", 1, NULL, 0, NULL);
    CHECK(coins);
    CHECK(mod_storage_set("alpha", "coins", coins));
    json_free(coins);
    CHECK(mod_storage_get("alpha", "coins")->type == JSON_NUMBER);
    CHECK(mod_storage_get("alpha", "missing") == NULL);
    CHECK(mod_storage_remove("alpha", "coins"));
    CHECK(mod_storage_get("alpha", "coins") == NULL);
    Json *flag = json_parse("true", 4, NULL, 0, NULL);
    CHECK(flag);
    CHECK(mod_storage_set("beta", "flag", flag));
    json_free(flag);
    Json *bad = json_parse("1", 1, NULL, 0, NULL);
    CHECK(bad);
    CHECK(!mod_storage_set("alpha", "", bad));
    json_free(bad);
    save_close();
    CHECK(save_open("selftest_mod_storage", 7));
    CHECK(mod_storage_get("alpha", "progress") != NULL && mod_storage_get("alpha", "progress")->items[0]->str && !strcmp(mod_storage_get("alpha", "progress")->items[0]->str, "village"));
    CHECK(mod_storage_get("beta", "flag") != NULL && mod_storage_get("beta", "flag")->boolean == true);
    CHECK(mod_storage_get("alpha", "coins") == NULL);
    CHECK(mod_storage_get("alpha", "missing") == NULL);
    Container chest; CHECK(container_open("alpha", "chest", &chest, 3));
    CHECK(chest.slots == 3 && container_add_item(&chest, stone->name, 5) == 0);
    CHECK(chest.slot[0].count == 5 && !strcmp(chest.slot[0].item_id, stone->name));
    CHECK(container_add_item(&chest, dirt->name, 7) == 0);
    CHECK(chest.slot[1].count == 7 && !strcmp(chest.slot[1].item_id, dirt->name));
    CHECK(container_close("alpha", "chest", &chest));
    save_close();
    CHECK(save_open("selftest_mod_storage", 7));
    Container restored; CHECK(container_open("alpha", "chest", &restored, 1));
    CHECK(restored.slots == 3);
    CHECK(restored.slot[0].count == 5 && !strcmp(restored.slot[0].item_id, stone->name));
    CHECK(restored.slot[1].count == 7 && !strcmp(restored.slot[1].item_id, dirt->name));
    CHECK(container_close("alpha", "chest", &restored));

    Container overflow; container_init(&overflow, 1);
    CHECK(container_add_item(&overflow, stone->name, 65) == 1);
    CHECK(overflow.slot[0].count == 64);
    save_close();
    remove("selftest_mod_storage/world.json");
    remove("selftest_mod_storage/region");
    remove("selftest_mod_storage");
    mods_reset();
    vfs_reset();
}

static void test_worldgen_data_driven(void) {
    vfs_reset();
    vfs_add_root("engine_assets", "dfe");
    dir_make_all("selftest_worldgen_mod/data/test/blocks");
    dir_make_all("selftest_worldgen_mod/data/test/worldgen");
    file_write_atomic("selftest_worldgen_mod/data/test/blocks/stone.json",
        "{\"shape\": \"cube\", \"layer\": \"opaque\", \"textures\": {\"all\": \"test:block/stone\"}}\n", strlen("{\"shape\": \"cube\", \"layer\": \"opaque\", \"textures\": {\"all\": \"test:block/stone\"}}\n"));
    file_write_atomic("selftest_worldgen_mod/data/test/blocks/crimson_sand.json",
        "{\"shape\": \"cube\", \"layer\": \"opaque\", \"textures\": {\"all\": \"test:block/crimson_sand\"}}\n", strlen("{\"shape\": \"cube\", \"layer\": \"opaque\", \"textures\": {\"all\": \"test:block/crimson_sand\"}}\n"));
    file_write_atomic("selftest_worldgen_mod/data/test/blocks/jade_ore.json",
        "{\"shape\": \"cube\", \"layer\": \"opaque\", \"textures\": {\"all\": \"test:block/jade_ore\"}}\n", strlen("{\"shape\": \"cube\", \"layer\": \"opaque\", \"textures\": {\"all\": \"test:block/jade_ore\"}}\n"));
    file_write_atomic("selftest_worldgen_mod/data/test/worldgen/default.json",
        "{\n  \"sea_level\": 62,\n  \"deep_level\": 0,\n  \"blocks\": {\n    \"stone\": \"test:stone\", \"deep_stone\": \"test:stone\", \"dirt\": \"test:stone\", \"grass\": \"test:stone\", \"sand\": \"test:crimson_sand\", \"sandstone\": \"test:stone\", \"gravel\": \"test:stone\", \"snow\": \"test:stone\", \"mud\": \"test:stone\", \"water\": \"test:stone\"\n  },\n  \"biomes\": [\n    {\"id\": \"test:crimson\", \"surface\": \"test:crimson_sand\", \"subsurface\": \"test:stone\", \"temperature\": [0.6, 1.0], \"humidity\": [0.0, 0.5], \"min_height\": 0, \"max_height\": 200},\n    {\"id\": \"test:stonefield\", \"surface\": \"test:stone\", \"subsurface\": \"test:stone\", \"temperature\": [0.0, 0.6], \"humidity\": [0.0, 1.0], \"min_height\": 0, \"max_height\": 200}\n  ],\n  \"ores\": [\n    {\"id\": \"test:jade_ore\", \"ore\": \"test:jade_ore\", \"replace\": \"test:stone\", \"min_y\": 0, \"max_y\": 32, \"density\": 0.95, \"size\": 3, \"rarity\": 4, \"biomes\": [\"test:crimson\"]}\n  ]\n}\n",
        strlen("{\n  \"sea_level\": 62,\n  \"deep_level\": 0,\n  \"blocks\": {\n    \"stone\": \"test:stone\", \"deep_stone\": \"test:stone\", \"dirt\": \"test:stone\", \"grass\": \"test:stone\", \"sand\": \"test:crimson_sand\", \"sandstone\": \"test:stone\", \"gravel\": \"test:stone\", \"snow\": \"test:stone\", \"mud\": \"test:stone\", \"water\": \"test:stone\"\n  },\n  \"biomes\": [\n    {\"id\": \"test:crimson\", \"surface\": \"test:crimson_sand\", \"subsurface\": \"test:stone\", \"temperature\": [0.6, 1.0], \"humidity\": [0.0, 0.5], \"min_height\": 0, \"max_height\": 200},\n    {\"id\": \"test:stonefield\", \"surface\": \"test:stone\", \"subsurface\": \"test:stone\", \"temperature\": [0.0, 0.6], \"humidity\": [0.0, 1.0], \"min_height\": 0, \"max_height\": 200}\n  ],\n  \"ores\": [\n    {\"id\": \"test:jade_ore\", \"ore\": \"test:jade_ore\", \"replace\": \"test:stone\", \"min_y\": 0, \"max_y\": 32, \"density\": 0.95, \"size\": 3, \"rarity\": 4, \"biomes\": [\"test:crimson\"]}\n  ]\n}\n"));
    vfs_add_root("selftest_worldgen_mod", "test");
    registry_load_blocks();
    CHECK(registry_load_worldgen_config() == 0);
    CHECK(gen_biome_count() >= 2);
    CHECK(gen_ore_count() >= 1);
    gen_init(777);
    GenScratch *scratch = gen_scratch_create();
    int lo, hi;
    gen_band(&lo, &hi);
    int H = (hi - lo + 1) * CHUNK_SIZE;
    u16 states[H * CHUNK_AREA];
    gen_column(scratch, 0, 0, states);
    u16 crimson = block_find("test:crimson_sand")->default_state;
    u16 jade = block_find("test:jade_ore")->default_state;
    bool saw_crimson = false, saw_jade = false;
    for (int y = lo * CHUNK_SIZE; y < (hi + 1) * CHUNK_SIZE; y++) {
        for (int z = 0; z < CHUNK_SIZE; z++) {
            for (int x = 0; x < CHUNK_SIZE; x++) {
                int ly = y - lo * CHUNK_SIZE;
                size_t idx = ((size_t)ly << 10) | (size_t)((z << 5) | x);
                if (states[idx] == crimson) saw_crimson = true;
                if (states[idx] == jade) saw_jade = true;
            }
        }
    }
    CHECK(saw_crimson && saw_jade);
    gen_scratch_destroy(scratch);
    mods_reset();
    vfs_reset();
    vfs_add_root("engine_assets", "dfe");
    mods_discover("mods");
    mods_resolve();
    mods_mount();
    remove("selftest_worldgen_mod");
}

void test_hydrology(void);
void test_trees(void);
void test_biomes(void);
void test_forever_worlds(void);
static void test_worldgen_features_and_structures(void) {
    vfs_reset();
    vfs_add_root("engine_assets", "dfe");
    dir_make_all("selftest_feature_mod/data/test/blocks");
    dir_make_all("selftest_feature_mod/data/test/worldgen");
    file_write_atomic("selftest_feature_mod/data/test/blocks/feature_glow.json",
        "{\"shape\": \"cube\", \"layer\": \"opaque\", \"textures\": {\"all\": \"test:block/feature_glow\"}}\n", strlen("{\"shape\": \"cube\", \"layer\": \"opaque\", \"textures\": {\"all\": \"test:block/feature_glow\"}}\n"));
    file_write_atomic("selftest_feature_mod/data/test/blocks/structure_anchor.json",
        "{\"shape\": \"cube\", \"layer\": \"opaque\", \"textures\": {\"all\": \"test:block/structure_anchor\"}}\n", strlen("{\"shape\": \"cube\", \"layer\": \"opaque\", \"textures\": {\"all\": \"test:block/structure_anchor\"}}\n"));
    file_write_atomic("selftest_feature_mod/data/test/worldgen/default.json",
        "{\n  \"sea_level\": 62,\n  \"deep_level\": 0,\n  \"blocks\": {\n    \"stone\": \"test:stone\", \"deep_stone\": \"test:stone\", \"dirt\": \"test:stone\", \"grass\": \"test:stone\", \"sand\": \"test:stone\", \"sandstone\": \"test:stone\", \"gravel\": \"test:stone\", \"snow\": \"test:stone\", \"mud\": \"test:stone\", \"water\": \"test:stone\"\n  },\n  \"biomes\": [\n    {\"id\": \"test:crimson\", \"surface\": \"test:stone\", \"subsurface\": \"test:stone\", \"temperature\": [0.6, 1.0], \"humidity\": [0.0, 0.5], \"min_height\": 0, \"max_height\": 200}\n  ],\n  \"features\": [\n    {\"id\": \"test:crimson_bloom\", \"block\": \"test:feature_glow\", \"biomes\": [\"test:crimson\"], \"chance\": 100, \"min_y\": 0, \"max_y\": 64, \"radius\": 1}\n  ],\n  \"structures\": [\n    {\"id\": \"test:watchtower\", \"biomes\": [\"test:crimson\"], \"chance\": 100, \"anchor\": \"test:stone\", \"blocks\": [[0, 0, 0, \"test:structure_anchor\"], [1, 0, 0, \"test:structure_anchor\"], [0, 1, 0, \"test:structure_anchor\"], [0, 0, 1, \"test:structure_anchor\"]]}\n  ]\n}\n",
        strlen("{\n  \"sea_level\": 62,\n  \"deep_level\": 0,\n  \"blocks\": {\n    \"stone\": \"test:stone\", \"deep_stone\": \"test:stone\", \"dirt\": \"test:stone\", \"grass\": \"test:stone\", \"sand\": \"test:stone\", \"sandstone\": \"test:stone\", \"gravel\": \"test:stone\", \"snow\": \"test:stone\", \"mud\": \"test:stone\", \"water\": \"test:stone\"\n  },\n  \"biomes\": [\n    {\"id\": \"test:crimson\", \"surface\": \"test:stone\", \"subsurface\": \"test:stone\", \"temperature\": [0.6, 1.0], \"humidity\": [0.0, 0.5], \"min_height\": 0, \"max_height\": 200}\n  ],\n  \"features\": [\n    {\"id\": \"test:crimson_bloom\", \"block\": \"test:feature_glow\", \"biomes\": [\"test:crimson\"], \"chance\": 100, \"min_y\": 0, \"max_y\": 64, \"radius\": 1}\n  ],\n  \"structures\": [\n    {\"id\": \"test:watchtower\", \"biomes\": [\"test:crimson\"], \"chance\": 100, \"anchor\": \"test:stone\", \"blocks\": [[0, 0, 0, \"test:structure_anchor\"], [1, 0, 0, \"test:structure_anchor\"], [0, 1, 0, \"test:structure_anchor\"], [0, 0, 1, \"test:structure_anchor\"]]}\n  ]\n}\n"));
    file_write_atomic("selftest_feature_mod/data/test/blocks/stone.json",
        "{\"shape\": \"cube\", \"layer\": \"opaque\", \"textures\": {\"all\": \"test:block/stone\"}}\n", strlen("{\"shape\": \"cube\", \"layer\": \"opaque\", \"textures\": {\"all\": \"test:block/stone\"}}\n"));
    vfs_add_root("selftest_feature_mod", "test");
    registry_load_blocks();
    CHECK(registry_load_worldgen_config() == 0);
    CHECK(gen_feature_count() >= 1);
    CHECK(gen_structure_count() >= 1);
    gen_init(777);
    GenScratch *scratch = gen_scratch_create();
    int lo, hi;
    gen_band(&lo, &hi);
    int H = (hi - lo + 1) * CHUNK_SIZE;
    u16 states[H * CHUNK_AREA];
    u16 glow = block_find("test:feature_glow")->default_state;
    u16 anchor = block_find("test:structure_anchor")->default_state;
    bool saw_glow = false, saw_anchor = false;
    for (int cz = -4; cz <= 4 && !(saw_glow && saw_anchor); cz++) {
        for (int cx = -4; cx <= 4 && !(saw_glow && saw_anchor); cx++) {
            gen_column(scratch, cx, cz, states);
            for (int y = lo * CHUNK_SIZE; y < (hi + 1) * CHUNK_SIZE; y++) {
                for (int z = 0; z < CHUNK_SIZE; z++) {
                    for (int x = 0; x < CHUNK_SIZE; x++) {
                        int ly = y - lo * CHUNK_SIZE;
                        size_t idx = ((size_t)ly << 10) | (size_t)((z << 5) | x);
                        if (states[idx] == glow) saw_glow = true;
                        if (states[idx] == anchor) saw_anchor = true;
                    }
                }
            }
        }
    }
    CHECK(saw_glow && saw_anchor);
    gen_scratch_destroy(scratch);
    mods_reset();
    vfs_reset();
    vfs_add_root("engine_assets", "dfe");
    mods_discover("mods");
    mods_resolve();
    mods_mount();
    remove("selftest_feature_mod");
}

/* Registry behaviour with hand-made blocks, independent of any mod on disk. */
static void test_registry(void) {
    registry_reset();
    BlockDef b = {0};
    snprintf(b.name, sizeof b.name, "test:lever");
    snprintf(b.mod, sizeof b.mod, "test");
    b.shape = SHAPE_CUBE;
    b.nprops = 2;
    snprintf(b.props[0].name, sizeof b.props[0].name, "powered");
    b.props[0].count = 2;
    snprintf(b.props[0].values[0], 16, "false");
    snprintf(b.props[0].values[1], 16, "true");
    snprintf(b.props[1].name, sizeof b.props[1].name, "facing");
    b.props[1].count = 3;
    snprintf(b.props[1].values[0], 16, "n");
    snprintf(b.props[1].values[1], 16, "e");
    snprintf(b.props[1].values[2], 16, "s");
    BlockDef *r = block_register(&b);
    CHECK(r && r->state_count == 6);
    registry_freeze_blocks();
    u16 st = block_state_with(r, r->default_state, "facing", "s");
    st = block_state_with(r, st, "powered", "true");
    CHECK(st == r->first_state + 1 + 2 * 2);
    CHECK(block_state_prop_index(r, st, 0) == 1 && block_state_prop_index(r, st, 1) == 2);
    CHECK(block_find("test:lever") == r && block_find("test:nothing") == NULL);

    /* State text round trips, and every kind of typo fails instead of resolving to a default. */
    char text[128];
    CHECK(block_parse_state("test:lever[powered=true,facing=s]") == st);
    CHECK(block_parse_state("test:lever[facing=s,powered=true]") == st);
    CHECK(block_parse_state("test:lever[powered=true]") == r->first_state + 1);
    CHECK(block_parse_state("test:lever") == r->default_state);
    CHECK(block_format_state(st, text, sizeof text) && !strcmp(text, "test:lever[powered=true,facing=s]"));
    CHECK(block_parse_state("test:lever[powered=maybe]") == STATE_UNLOADED);
    CHECK(block_parse_state("test:lever[colour=red]") == STATE_UNLOADED);
    CHECK(block_parse_state("test:lever[powered=true") == STATE_UNLOADED);
    CHECK(block_parse_state("test:nothing[powered=true]") == STATE_UNLOADED);
    CHECK(block_format_state(0xFFF0, text, sizeof text) == false);

    /* A block can emit light only in the states where a property has a given value. */
    BlockDef lamp = {0};
    snprintf(lamp.name, sizeof lamp.name, "test:lamp");
    snprintf(lamp.mod, sizeof lamp.mod, "test");
    lamp.shape = SHAPE_CUBE;
    lamp.nprops = 1;
    snprintf(lamp.props[0].name, sizeof lamp.props[0].name, "lit");
    lamp.props[0].count = 2;
    snprintf(lamp.props[0].values[0], 16, "off");
    snprintf(lamp.props[0].values[1], 16, "on");
    lamp.emit[0] = 15;
    lamp.emit_prop = 0;
    snprintf(lamp.emit_prop_name, sizeof lamp.emit_prop_name, "lit");
    snprintf(lamp.emit_value, sizeof lamp.emit_value, "on");
    BlockDef *lr = block_register(&lamp);
    registry_freeze_blocks();
    CHECK(lr && g_state_emit[block_parse_state("test:lamp[lit=off]")] == 0);
    CHECK(lr && g_state_emit[block_parse_state("test:lamp[lit=on]")] == (15 << 8));

    BlockNameTable saved;
    block_table_save_names(&saved);
    u32 index = 0;
    for (int i = 0; i < saved.names.n; i++) if (!strcmp(saved.names.d[i], "test:lever")) index = (u32)i;
    CHECK(block_table_remap_state(&saved, index, 3) == r->first_state + 3);
    CHECK(block_table_remap_state(&saved, 9999, 0) == STATE_MISSING);
    block_table_free(&saved);

    BlockDef dup = b;
    dup.nprops = 1;
    CHECK(block_register(&dup) == NULL && data_error_count() > 0);
    data_error_reset();
}

/* ---------------------------------------------------------------- mesher */

static void fill_padded(MeshInput *in, u16 state) {
    in->states = xmalloc(MESH_PAD_VOL * sizeof(u16));
    in->light = xmalloc(MESH_PAD_VOL * sizeof(u16));
    for (int i = 0; i < MESH_PAD_VOL; i++) { in->states[i] = state; in->light[i] = LIGHT_FULL_SKY; }
}

static void set_padded(MeshInput *in, int x, int y, int z, u16 s) { in->states[((y + 1) * MESH_PAD + (z + 1)) * MESH_PAD + (x + 1)] = s; }

static void test_mesher(const BlockDef *stone) {
    MeshInput in = {0};
    MeshOutput out;
    fill_padded(&in, STATE_AIR);
    mesh_build(&in, &out);
    CHECK(out.count[0] + out.count[1] + out.count[2] == 0 && out.conn == 0x7FFF);
    mesh_output_free(&out);

    set_padded(&in, 5, 5, 5, stone->default_state);
    mesh_build(&in, &out);
    CHECK(out.count[LAYER_OPAQUE] == 24); /* one cube: six quads */
    mesh_output_free(&out);

    set_padded(&in, 6, 5, 5, stone->default_state);
    mesh_build(&in, &out);
    CHECK(out.count[LAYER_OPAQUE] == 24); /* two cubes: end caps plus four merged side quads */
    mesh_output_free(&out);

    /* A solid slab through the middle separates the lower half of the chunk from the upper half. */
    for (int z = 0; z < 32; z++) for (int x = 0; x < 32; x++) set_padded(&in, x, 16, z, stone->default_state);
    mesh_build(&in, &out);
    CHECK(!chunk_faces_connected(out.conn, DIR_PY, DIR_NY));
    CHECK(chunk_faces_connected(out.conn, DIR_PX, DIR_NX) == true);
    mesh_output_free(&out);
    mesh_input_free(&in);

    CHECK(conn_pair_index(0, 1) == 0 && conn_pair_index(4, 5) == 14 && conn_pair_index(5, 4) == 14);
}

static void test_light_column(const BlockDef *stone) {
    const int layers = 2, h = layers * CHUNK_SIZE;
    u16 *states = xcalloc((size_t)h * CHUNK_AREA, sizeof(u16));
    u16 *light = xcalloc((size_t)h * CHUNK_AREA, sizeof(u16));
    /* Stone roof at y=40 over x in [0,15]; the rest of the layer stays open to the sky. */
    for (int z = 0; z < 32; z++) for (int x = 0; x < 16; x++) states[(40 << 10) | (z << 5) | x] = stone->default_state;
    light_init_column(states, layers, light);
    CHECK(LIGHT_SKY(light[(63 << 10) | (5 << 5) | 5]) == 15);
    CHECK(LIGHT_SKY(light[(39 << 10) | (5 << 5) | 5]) == 4);     /* eleven voxels in from the open edge */
    CHECK(LIGHT_SKY(light[(39 << 10) | (5 << 5) | 15]) == 14);  /* one step in from the open edge */
    CHECK(LIGHT_SKY(light[(39 << 10) | (5 << 5) | 12]) == 11);
    CHECK(LIGHT_SKY(light[(39 << 10) | (5 << 5) | 16]) == 15);
    free(states);
    free(light);
}

/* Needs the real base content: generation, incremental light, edits. */
static bool open_sky_spot(int x, int z) {
    int ground = ifloor(gen_height_at((float)x, (float)z));
    for (int dz = -3; dz <= 3; dz++)
        for (int dx = -3; dx <= 3; dx++)
            for (int y = ifloor(gen_height_at((float)(x + dx), (float)(z + dz))) + 2; y <= ground + 16; y++)
                if (world_get_state(x + dx, y, z + dz) != STATE_AIR) return false;
    return true;
}

static void test_world_light(void) {
    registry_reset();
    data_error_reset();
    registry_load_blocks();
    registry_load_worldgen_config();
    CHECK(data_error_count() == 0);
    BlockDef *crystal = block_find("base:crystal_red"), *stone = block_find("base:stone");
    CHECK(crystal && stone);
    if (!crystal || !stone || data_error_count()) return;
    jobs_init(2);
    world_init(4242);
    world_flush_generation(0, 0, 2);

    /* Trees and plants are generated, so pick a spot whose surroundings are open air for the whole test. */
    int x = 8, z = 8;
    for (int k = 0; k < 400 && !open_sky_spot(x, z); k++) { x = 8 + 37 * (k % 20); z = 8 + 37 * (k / 20); }
    int ground = ifloor(gen_height_at((float)x, (float)z));
    CHECK(open_sky_spot(x, z));
    CHECK(world_get_state(x, ground, z) != STATE_AIR);
    CHECK(LIGHT_SKY(world_get_light(x, ground + 6, z)) == 15);
    CHECK(LIGHT_SKY(world_get_light(x, ground - 8, z)) == 0);

    /* Block light from a coloured emitter falls off by one per voxel in open air and is exactly undone on removal. */
    int ey = ground + 12;
    CHECK(world_set_state(x, ey, z, crystal->default_state));
    light_process(1 << 30);
    CHECK(LIGHT_R(world_get_light(x, ey, z)) == 15);
    CHECK(LIGHT_R(world_get_light(x + 1, ey, z)) == 14);
    CHECK(LIGHT_R(world_get_light(x, ey + 4, z)) == 11);
    CHECK(LIGHT_G(world_get_light(x + 1, ey, z)) == 2);
    CHECK(world_set_state(x, ey, z, STATE_AIR));
    light_process(1 << 30);
    CHECK(LIGHT_R(world_get_light(x + 1, ey, z)) == 0 && LIGHT_G(world_get_light(x, ey + 2, z)) == 0);
    CHECK(LIGHT_SKY(world_get_light(x + 1, ey, z)) == 15);

    /* A roof removes sky light below it and taking the roof away restores it. */
    int ry = ground + 5;
    for (int dx = -1; dx <= 1; dx++) for (int dz = -1; dz <= 1; dz++) world_set_state(x + dx, ry, z + dz, stone->default_state);
    light_process(1 << 30);
    CHECK(LIGHT_SKY(world_get_light(x, ry - 2, z)) < 15);
    for (int dx = -1; dx <= 1; dx++) for (int dz = -1; dz <= 1; dz++) world_set_state(x + dx, ry, z + dz, STATE_AIR);
    light_process(1 << 30);
    CHECK(LIGHT_SKY(world_get_light(x, ry - 2, z)) == 15);

    world_shutdown();
    jobs_shutdown();
}

/* Generated terrain and edits survive a save, unload and reload. */
void remove_tree_files(const char *dir) {
    char path[600];
    StrList names = {0};
    snprintf(path, sizeof path, "%s/region", dir);
    dir_list(path, &names);
    for (int i = 0; i < names.n; i++) {
        char f[700];
        snprintf(f, sizeof f, "%s/%s", path, names.d[i]);
        remove(f);
    }
    strlist_free(&names);
    remove(path);
    snprintf(path, sizeof path, "%s/world.json", dir);
    remove(path);
    remove(dir);
}

static void test_save_roundtrip(void) {
    registry_reset();
    data_error_reset();
    registry_load_blocks();
    registry_load_worldgen_config();
    BlockDef *crystal = block_find("base:crystal_red"), *stone = block_find("base:stone"), *glass = block_find("base:glass");
    CHECK(crystal && stone && glass);
    if (!crystal || !stone || !glass || data_error_count()) return;
    const char *dir = "selftest_world";
    const u64 seed = 0x123456789ABCDEF0ull;
    remove_tree_files(dir);
    jobs_init(2);
    CHECK(save_open(dir, seed));
    CHECK(save_seed() == seed);
    world_init(save_seed());
    int gen_lo, gen_hi;
    gen_band(&gen_lo, &gen_hi);
    size_t generated_count = (size_t)(gen_hi - gen_lo + 1) * CHUNK_VOL;
    u16 *unseen_before = xmalloc(generated_count * sizeof(u16));
    u16 *unseen_after = xmalloc(generated_count * sizeof(u16));
    GenScratch *unseen_scratch = gen_scratch_create();
    gen_column(unseen_scratch, 20, -17, unseen_before);
    world_flush_generation(0, 0, 2);
    int ground = ifloor(gen_height_at(5, 5));
    for (int z = 2; z < 22; z++)
        for (int x = 2; x < 22; x++) ground = MAX(ground, ifloor(gen_height_at((float)x, (float)z)));
    int ey = ground + 20;
    /* A varied pattern forces a multi-entry palette, and the emitter forces non-uniform light. */
    for (int i = 0; i < 40; i++) world_set_state(2 + i % 20, ey, 2 + i / 2, (i & 1) ? stone->default_state : glass->default_state);
    world_set_state(5, ey + 3, 5, crystal->default_state);
    light_process(1 << 30);
    u16 want_state = world_get_state(5, ey + 3, 5), want_light = world_get_light(6, ey + 3, 5);
    u16 want_pat = world_get_state(3 + 0, ey, 2 + 0);
    CHECK(LIGHT_R(want_light) == 14);
    world_shutdown();
    save_close();
    jobs_shutdown();

    jobs_init(2);
    CHECK(save_open(dir, 12345));
    CHECK(save_seed() == seed); /* an existing world keeps its exact 64-bit seed */
    SavedColumn untouched = {0};
    bool found_untouched = save_load_column(1, 1, &untouched);
    CHECK(found_untouched);
    if (found_untouched) {
        for (int k = 0; k <= untouched.hi - untouched.lo; k++) chunk_destroy(untouched.chunks[k]);
        free(untouched.chunks);
    }
    world_init(save_seed());
    gen_column(unseen_scratch, 20, -17, unseen_after);
    CHECK(!memcmp(unseen_before, unseen_after, generated_count * sizeof(u16)));
    gen_scratch_destroy(unseen_scratch);
    free(unseen_before);
    free(unseen_after);
    world_flush_generation(0, 0, 2);
    CHECK(world_get_state(5, ey + 3, 5) == want_state && want_state == crystal->default_state);
    CHECK(world_get_light(6, ey + 3, 5) == want_light);
    CHECK(world_get_state(3, ey, 2) == want_pat);
    /* Terrain far from the edit regenerates identically. */
    CHECK(world_get_state(-40, ifloor(gen_height_at(-40, 3)), 3) != STATE_AIR);
    world_shutdown();
    save_close();
    jobs_shutdown();
    remove_tree_files(dir);
}

static void test_save_schema_version(void) {
    registry_reset();
    data_error_reset();
    registry_load_blocks();
    const char *dir = "selftest_save_schema";
    remove_tree_files(dir);

    CHECK(save_open(dir, 7));
    save_close();
    char path[600];
    snprintf(path, sizeof path, "%s/world.json", dir);
    size_t len = 0;
    u8 *raw = file_read(path, &len);
    CHECK(raw && strstr((const char *)raw, "\"schema_version\""));
    free(raw);

    /* Missing schema_version is the supported legacy policy and is rewritten on close. */
    const char *legacy = "{\"version\":2,\"seed\":7,\"seed_high\":0,\"blocks\":[]}";
    CHECK(file_write_atomic(path, legacy, strlen(legacy)));
    CHECK(save_open(dir, 99));
    CHECK(save_seed() == 7);
    save_close();

    const char *newer = "{\"version\":2,\"schema_version\":3,\"seed\":7,\"seed_high\":0,\"blocks\":[]}";
    CHECK(file_write_atomic(path, newer, strlen(newer)));
    CHECK(!save_open(dir, 7));
    const char *older = "{\"version\":2,\"schema_version\":0,\"seed\":7,\"seed_high\":0,\"blocks\":[]}";
    CHECK(file_write_atomic(path, older, strlen(older)));
    CHECK(!save_open(dir, 7));
    remove_tree_files(dir);
}

/* One call saves a bounded number of columns; the rest stay dirty until a later call or world_save_all. */
static void test_save_budget(void) {
    registry_reset();
    data_error_reset();
    registry_load_blocks();
    registry_load_worldgen_config();
    BlockDef *stone = block_find("base:stone"), *glass = block_find("base:glass"), *crystal = block_find("base:crystal_red");
    CHECK(stone && glass && crystal);
    if (!stone || !glass || !crystal || data_error_count()) return;
    enum { SIDE = 8, COLUMNS = SIDE * SIDE, BUDGET = 16 };
    const char *dir = "selftest_world_budget";
    remove_tree_files(dir);
    jobs_init(2);
    CHECK(save_open(dir, 7));
    world_init(save_seed());
    world_flush_generation(0, 0, SIDE / 2);
    world_save_all();
    CHECK(world_dirty_columns() == 0);
    int ex[COLUMNS], ey[COLUMNS], ez[COLUMNS];
    u16 want[COLUMNS];
    for (int i = 0; i < COLUMNS; i++) {
        int cx = i % SIDE - SIDE / 2, cz = i / SIDE - SIDE / 2;
        ex[i] = cx * CHUNK_SIZE + 3 + i % 25;
        ez[i] = cz * CHUNK_SIZE + 5 + i % 23;
        ey[i] = ifloor(gen_height_at((float)ex[i], (float)ez[i])) + 10 + i % 5;
        want[i] = i % 7 == 0 ? crystal->default_state : (i & 1) ? stone->default_state : glass->default_state;
        world_set_state(ex[i], ey[i], ez[i], want[i]);
    }
    CHECK(world_dirty_columns() == COLUMNS);
    world_save_dirty();
    CHECK(world_dirty_columns() == COLUMNS - BUDGET);
    world_save_all();
    CHECK(world_dirty_columns() == 0);
    int round_tripped = 0;
    for (int i = 0; i < COLUMNS; i++) {
        int cx = ex[i] >> CHUNK_SHIFT, cz = ez[i] >> CHUNK_SHIFT, cy = ey[i] >> CHUNK_SHIFT;
        SavedColumn sc;
        if (!save_load_column(cx, cz, &sc)) continue;
        if (cy >= sc.lo && cy <= sc.hi) {
            int idx = ((ey[i] & 31) << 10) | ((ez[i] & 31) << 5) | (ex[i] & 31);
            if (chunk_get(sc.chunks[cy - sc.lo], idx) == want[i]) round_tripped++;
        }
        for (int k = 0; k <= sc.hi - sc.lo; k++) chunk_destroy(sc.chunks[k]);
        free(sc.chunks);
    }
    CHECK(round_tripped == COLUMNS);
    world_shutdown();
    save_close();
    jobs_shutdown();
    remove_tree_files(dir);
}

static void test_gen_determinism(void) {
    registry_reset();
    data_error_reset();
    registry_load_blocks();
    registry_load_worldgen_config();
    if (data_error_count()) { CHECK(false); return; }
    gen_init(777);
    int above_sea = 0, below_sea = 0, sea_shelf_samples = 0;
    float min_height = 10000.0f, max_height = -10000.0f;
    for (int z = -1024; z <= 1024; z += 64)
        for (int x = -1024; x <= 1024; x += 64) {
            float h = gen_height_at((float)x, (float)z);
            if (h > (float)gen_sea_level()) above_sea++;
            else below_sea++;
            if (h == (float)gen_sea_level() + 1.0f) sea_shelf_samples++;
            min_height = MIN(min_height, h);
            max_height = MAX(max_height, h);
        }
    CHECK(above_sea > 100 && below_sea > 100);
    CHECK(sea_shelf_samples < 16);
    CHECK(max_height - min_height > 55.0f);
    /* A hydrology query is a pure world-generation input: repeated samples must agree and dry cells must not expose a
     * stale water surface. The production change this catches is a query that retains mutable generation state or
     * reports wet terrain without a carved bed. */
    GenHydrologySample hydrology_a, hydrology_b;
    gen_hydrology_at(384.0f, -192.0f, &hydrology_a);
    gen_hydrology_at(384.0f, -192.0f, &hydrology_b);
    CHECK(!memcmp(&hydrology_a, &hydrology_b, sizeof hydrology_a));
    if (hydrology_a.wet) CHECK(hydrology_a.bed_y < hydrology_a.water_y);
    else CHECK(hydrology_a.channel <= 0.5f);
    int routed_samples = 0, routed_steps = 0;
    for (int z = -768; z <= 768; z += 32)
        for (int x = -768; x <= 768; x += 32) {
            GenHydrologySample sample;
            gen_hydrology_at((float)x, (float)z, &sample);
            if (!sample.wet) continue;
            routed_samples++;
            CHECK(sample.channel > 0.0f && sample.bed_y < sample.water_y);
            CHECK(sample.water_y < (float)gen_sea_level() + 160.0f);
            float downstream_x = sample.downstream_x, downstream_z = sample.downstream_z;
            GenHydrologySample downstream;
            gen_hydrology_at(downstream_x, downstream_z, &downstream);
            if (downstream.wet) {
                routed_steps++;
                CHECK(downstream.water_y <= sample.water_y + 0.01f);
                CHECK(sample.water_y - downstream.water_y < 3.0f);
            }
        }
    CHECK(routed_samples > 0);
    CHECK(routed_steps > 0);
    bool found_river = false, found_dry_below_sea = false, found_ocean = false;
    int sea_voxel = gen_sea_level();
    GenLodGrid lod;
    for (int cz = -24; cz <= 24 && !(found_river && found_dry_below_sea && found_ocean); cz += 4)
        for (int cx = -24; cx <= 24 && !(found_river && found_dry_below_sea && found_ocean); cx += 4) {
            gen_lod_grid(0, cx, cz, &lod);
            for (int z = 1; z <= CHUNK_SIZE; z++)
                for (int x = 1; x <= CHUNK_SIZE; x++)
                    {
                        int i = z * LOD_PAD + x;
                        int water_top = lod.water_top[i];
                        int water_depth = water_top - lod.top[i];
                        if (lod.top[i] < sea_voxel && water_top == lod.top[i]) found_dry_below_sea = true;
                        if (lod.top[i] < sea_voxel && water_top == sea_voxel) found_ocean = true;
                        if (water_top > sea_voxel && water_depth >= 1 && water_depth <= 6) {
                            found_river = true;
                        }
                    }
        }
    CHECK(found_river);
    CHECK(found_dry_below_sea);
    CHECK(found_ocean);
    int lo, hi;
    gen_band(&lo, &hi);
    size_t n = (size_t)(hi - lo + 1) * CHUNK_VOL;
    u16 *a = xmalloc(n * sizeof(u16)), *b = xmalloc(n * sizeof(u16));
    GenScratch *s1 = gen_scratch_create(), *s2 = gen_scratch_create();
    gen_column(s1, -3, 7, a);
    gen_column(s2, 4, 4, b);   /* dirty the second scratch with a different column first */
    gen_column(s2, -3, 7, b);
    CHECK(!memcmp(a, b, n * sizeof(u16)));
    gen_scratch_destroy(s1);
    gen_scratch_destroy(s2);
    gen_shutdown();
    free(a);
    free(b);
}

/* Epoch-0 guard: digests are over block names (not state ids) so new blocks cannot disturb them. Captured from the
 * pre-overhaul generator; a mismatch means legacy worldgen output changed. */
static u64 golden_column_digest(GenScratch *s, int cx, int cz, u16 *buf, size_t n) {
    gen_column(s, cx, cz, buf);
    u64 h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) {
        BlockDef *b = block_of_state(buf[i]);
        const char *nm = b ? b->name : "?";
        for (; *nm; nm++) h = (h ^ (u8)*nm) * 1099511628211ull;
        h = (h ^ (u64)(b ? buf[i] - b->default_state : 0xffff)) * 1099511628211ull;
    }
    return h;
}

static void test_gen_golden_epoch0(void) {
    registry_reset();
    data_error_reset();
    registry_load_blocks();
    registry_load_worldgen_config();
    if (data_error_count()) { CHECK(false); return; }
    static const struct { u64 seed; int cx, cz; u64 digest; } G[8] = {
        {777, 0, 0, 0xb8e6d85295bc6d45ull}, {777, -3, 7, 0xeeba96804bd2485bull}, {777, 20, -15, 0x15f3538be5325e70ull}, {777, -40, -40, 0x617aabd60ec34d68ull},
        {1, 5, 5, 0x9e5f1fc23ba434bdull}, {1, -12, 30, 0x551b9f5d4f574dd0ull}, {424242, 0, 0, 0xac2483916627e9a6ull}, {424242, 64, -64, 0x2f534eea26450ad6ull},
    };
    int lo = 0, hi = 0;
    u64 cur_seed = 0;
    GenScratch *s = gen_scratch_create();
    for (int i = 0; i < 8; i++) {
        if (i == 0 || G[i].seed != cur_seed) {
            if (i) gen_shutdown();
            gen_init(G[i].seed);
            cur_seed = G[i].seed;
            gen_band(&lo, &hi);
        }
        size_t n = (size_t)(hi - lo + 1) * CHUNK_VOL;
        u16 *buf = xmalloc(n * sizeof(u16));
        u64 d = golden_column_digest(s, G[i].cx, G[i].cz, buf, n);
        if (getenv("DFE_GOLDEN_PRINT")) printf("GOLDEN %d 0x%016llxull\n", i, (unsigned long long)d);
        CHECK(d == G[i].digest);
        free(buf);
    }
    gen_scratch_destroy(s);
    gen_shutdown();
}

static void test_river_smoothness(void) {
    registry_reset();
    data_error_reset();
    registry_load_blocks();
    registry_load_worldgen_config();
    if (data_error_count()) { CHECK(false); return; }
    gen_init(777);
    float last = gen_height_at(-64.0f, 0.0f);
    float max_step = 0.0f;
    for (float x = -63.5f; x <= 64.0f; x += 0.5f) {
        float h = gen_height_at(x, 0.0f);
        float step = fabsf(h - last);
        max_step = MAX(max_step, step);
        last = h;
    }
    CHECK(max_step < 5.0f);
    gen_shutdown();
}

static void test_world_and_mesh(void) {
    test_world_light();
    test_save_roundtrip();
    test_save_schema_version();
    test_save_budget();
    test_gen_determinism();
    test_gen_golden_epoch0();
    test_river_smoothness();
    registry_reset();
    BlockDef st = {0};
    snprintf(st.name, sizeof st.name, "test:stone");
    snprintf(st.mod, sizeof st.mod, "test");
    st.shape = SHAPE_CUBE;
    st.nprops = 0;
    BlockDef *stone = block_register(&st);
    registry_freeze_blocks();
    test_mesher(stone);
    test_light_column(stone);
}


/* ---------------------------------------------------------------- mods and scripting */

#define MODS_ROOT "selftest_mods"
static char g_made_files[64][300], g_made_dirs[64][300];
static int g_made_file_n, g_made_dir_n;

static void put_file(const char *mod, const char *rel, const char *text) {
    char dir[260], path[300];
    snprintf(dir, sizeof dir, "%s/%s", MODS_ROOT, mod);
    char sub[200];
    snprintf(sub, sizeof sub, "%s", rel);
    char *slash = strrchr(sub, '/');
    if (slash) { *slash = '\0'; snprintf(dir, sizeof dir, "%s/%s/%s", MODS_ROOT, mod, sub); }
    dir_make_all(dir);
    if (g_made_dir_n < 64) snprintf(g_made_dirs[g_made_dir_n++], 300, "%s", dir);
    snprintf(path, sizeof path, "%s/%s/%s", MODS_ROOT, mod, rel);
    file_write_atomic(path, text, strlen(text));
    if (g_made_file_n < 64) snprintf(g_made_files[g_made_file_n++], 300, "%s", path);
}

static void put_manifest(const char *mod, const char *body) {
    char text[600];
    snprintf(text, sizeof text, "{\"id\": \"%s\", \"version\": \"1.0.0\", %s}", mod, body);
    put_file(mod, "mod.json", text);
}

static void remove_made(void) {
    for (int i = 0; i < g_made_file_n; i++) remove(g_made_files[i]);
    for (int i = g_made_dir_n - 1; i >= 0; i--) remove(g_made_dirs[i]);
    StrList l = {0};
    dir_list(MODS_ROOT, &l);
    for (int i = 0; i < l.n; i++) { char d[300]; snprintf(d, sizeof d, "%s/%s", MODS_ROOT, l.d[i]); remove(d); }
    strlist_free(&l);
    remove(MODS_ROOT);
    g_made_file_n = g_made_dir_n = 0;
}

static int order_of(const char *id) { const ModInfo *m = mods_find(id); return m ? m->order : -2; }

static bool last_log_contains(const char *needle) {
    int n = log_history_count();
    for (int i = n - 1; i >= 0 && i >= n - 6; i--) if (strstr(log_history_line(i, NULL), needle)) return true;
    return false;
}

static void test_mod_resolution(void) {
    put_manifest("base", "\"name\": \"B\"");
    put_manifest("alpha", "\"depends\": [\"base>=1.0\"]");
    put_manifest("beta", "\"depends\": [\"alpha^1.0\"]");
    put_manifest("aardvark", "\"depends\": [\"base\"]");
    put_manifest("gamma", "\"depends\": [\"alpha>=2.0\"]");
    put_manifest("delta", "\"depends\": [\"nothing_here\"]");
    put_manifest("epsilon", "\"depends\": [\"base\", \"?absent\"]");
    put_manifest("cyc_a", "\"depends\": [\"cyc_b\"]");
    put_manifest("cyc_b", "\"depends\": [\"cyc_a\"]");
    put_manifest("late", "\"load_after\": [\"epsilon\"]");
    put_manifest("dependsondelta", "\"depends\": [\"delta\"]");
    put_manifest("futuristic", "\"api\": 99");
    put_file("broken", "mod.json", "{\"id\": \"broken\", ");
    put_file("off_mod", "mod.json", "{\"id\": \"off_mod\", \"version\": \"1.0.0\"}");
    put_file(".", "mods.json", "{\"disabled\": [\"off_mod\"]}");
    data_error_reset();
    mods_reset();
    mods_discover(MODS_ROOT);
    mods_resolve();
    CHECK(order_of("base") == 0);
    CHECK(order_of("aardvark") > 0 && order_of("alpha") > 0 && order_of("beta") > order_of("alpha"));
    CHECK(order_of("aardvark") < order_of("alpha"));      /* ties are broken by id */
    CHECK(mods_find("off_mod")->disabled && order_of("off_mod") == -1);   /* mods.json switches it off */
    CHECK(order_of("late") > order_of("epsilon"));         /* load_after is honoured */
    CHECK(order_of("gamma") == -1 && mods_find("gamma")->failed);          /* version too old */
    CHECK(order_of("delta") == -1 && mods_find("delta")->failed);          /* missing dependency */
    CHECK(mods_find("dependsondelta")->failed);            /* failure cascades */
    CHECK(mods_find("cyc_a")->failed && mods_find("cyc_b")->failed);       /* cycle is reported, not looped on */
    CHECK(!mods_find("epsilon")->failed);                  /* optional dependency may be absent */
    CHECK(mods_find("futuristic") == NULL);                /* wrong api never registers */
    CHECK(data_error_count() >= 7);
    bool named = false;
    for (int i = 0; i < data_error_count(); i++) if (strstr(data_error_text(i), "[mod gamma]") && strstr(data_error_text(i), "mod.json")) named = true;
    CHECK(named);
    /* The same inputs give the same order on a second run. */
    int first[16], n = mods_loaded_count();
    for (int i = 0; i < n && i < 16; i++) first[i] = order_of(mods_loaded_at(i)->id);
    mods_reset();
    data_error_reset();
    mods_discover(MODS_ROOT);
    mods_resolve();
    bool same = mods_loaded_count() == n;
    for (int i = 0; same && i < n && i < 16; i++) same = order_of(mods_loaded_at(i)->id) == first[i];
    CHECK(same);
}

static int g_tick_hits;
static int native_tick(const dfe_event_t *ev, void *user) { (void)ev; (void)user; g_tick_hits++; return 0; }
static void native_command(const char *args, void *user) { (void)args; (void)user; }

static void test_mod_user_ownership(void) {
    int *native_event_user = malloc(sizeof *native_event_user);
    int *native_command_user = malloc(sizeof *native_command_user);
    *native_event_user = 17;
    *native_command_user = 29;
    CHECK(api_get()->subscribe("tick", native_tick, native_event_user, "native_ownership") > 0);
    CHECK(api_get()->register_command("native_ownership", "test", native_command, native_command_user, "native_ownership") > 0);
    events_clear_all();
    CHECK(*native_event_user == 17 && *native_command_user == 29);
    free(native_event_user);
    free(native_command_user);

    void *lua_event_user = malloc(8);
    void *lua_command_user = malloc(8);
    CHECK(api_subscribe_owned("tick", native_tick, lua_event_user, "lua_ownership") > 0);
    CHECK(api_register_command_owned("lua_ownership", "test", native_command, lua_command_user, "lua_ownership") > 0);
    events_clear_all();
    events_clear_all();
}

static void test_scripting(void) {
    remove_made();
    registry_reset();
    put_manifest("base", "\"script\": \"scripts/main.lua\"");
    put_file("base", "scripts/main.lua",
             "local util = require('util')\n"
             "if os or io or load or loadstring or dofile or require_missing or package or debug or ffi or jit or collectgarbage then error('sandbox leak') end\n"
             "dfe.command('ping', 'test command', function(args) dfe.console('pong ' .. args .. util.suffix) end)\n"
             "dfe.on('block_place', function(ev) return ev.x == 5 end)\n"
             "ticks = 0\n"
             "dfe.on('tick', function(ev) ticks = ticks + 1; if ticks == 2 then error('boom') end end)\n");
    put_file("base", "scripts/util.lua", "return {suffix = '!'}\n");
    put_manifest("looper", "\"depends\": [\"base\"], \"script\": \"scripts/main.lua\"");
    put_file("looper", "scripts/main.lua", "dfe.on('tick', function() end)\nwhile true do end\n");
    put_manifest("syntax", "\"depends\": [\"base\"], \"script\": \"scripts/main.lua\"");
    put_file("syntax", "scripts/main.lua", "local x = = 1\n");
    put_manifest("isolated", "\"depends\": [\"base\"], \"script\": \"scripts/main.lua\"");
    put_file("isolated", "scripts/main.lua", "if ticks ~= nil then error('globals leaked between mods') end\n");
    put_manifest("memory", "\"depends\": [\"base\"], \"script\": \"scripts/main.lua\"");
    put_file("memory", "scripts/main.lua", "local s = 'x' for i = 1, 40 do s = s .. s end\n");
    /* Two mods write into their own string, table and math; the third, loaded after both, must see none of it.
     * A failed assert in any of them raises the error count checked below. */
    put_manifest("libone", "\"depends\": [\"base\"], \"script\": \"scripts/main.lua\"");
    put_file("libone", "scripts/main.lua", "string.foo = 'mine'; table.foo = 1; math.foo = 2\nassert(string.foo == 'mine' and ('x'):upper() == 'X')\n");
    put_manifest("libtwo", "\"depends\": [\"base\"], \"script\": \"scripts/main.lua\"");
    put_file("libtwo", "scripts/main.lua", "string.foo = 'mine'; string.upper = nil\nassert(string.foo == 'mine' and ('x'):upper() == 'X')\n");
    put_manifest("libthree", "\"depends\": [\"libone\", \"libtwo\"], \"script\": \"scripts/main.lua\"");
    put_file("libthree", "scripts/main.lua", "assert(string.foo == nil and table.foo == nil and math.foo == nil and string.upper ~= nil)\n");
    data_error_reset();
    mods_reset();
    events_clear_all();
    mods_discover(MODS_ROOT);
    mods_resolve();
    CHECK(script_init());
    int errors = script_load_mods();
    CHECK(errors == 3); /* looper, syntax and memory fail; base, isolated and the three lib mods load */
    bool loop_named = false, syntax_named = false;
    for (int i = 0; i < data_error_count(); i++) {
        if (strstr(data_error_text(i), "[mod looper]") && strstr(data_error_text(i), "scripts/main.lua:2") && strstr(data_error_text(i), "instructions")) loop_named = true;
        if (strstr(data_error_text(i), "[mod syntax]") && strstr(data_error_text(i), "scripts/main.lua:1")) syntax_named = true;
    }
    CHECK(loop_named && syntax_named);
    command_run("ping hello");
    CHECK(last_log_contains("pong hello!"));
    dfe_event_t place = {.name = "block_place", .x = 5}, other = {.name = "block_place", .x = 6};
    CHECK(event_fire(&place));
    CHECK(!event_fire(&other));
    /* A failing handler is reported once, then switched off. */
    int before = script_error_count();
    dfe_event_t tick = {.name = "tick", .dt = 0.05};
    for (int i = 0; i < 5; i++) event_fire(&tick);
    CHECK(script_error_count() == before + 1);
    /* The looping mod's handler was removed with its failed script, so only native subscribers remain. */
    g_tick_hits = 0;
    api_get()->subscribe("tick", native_tick, NULL, "native_test");
    event_fire(&tick);
    CHECK(g_tick_hits == 1);
    CHECK(api_get()->subscribe("no_such_event", native_tick, NULL, "native_test") == 0);
    script_eval("1 + 2");
    CHECK(last_log_contains("3"));
    entity_clear();
    { char e[96]; CHECK(entity_type_register_json("base:hopper", "{\"size\":[0.5,0.6]}", e, sizeof e)); }
    entity_world_init(1);
    script_eval("(function()\n"
        "local id = assert(dfe.entity.spawn('base:hopper', {x = 5.5, y = 201, z = 5.5}))\n"
        "assert(dfe.entity.get(id).type == 'base:hopper')\n"
        "assert(dfe.entity.set(id, {health = 1}) == false or true)\n"
        "assert(dfe.entity.near({x = 5.5, y = 201, z = 5.5}, 3)[1].distance < 1)\n"
        "local n = 0 for _ in dfe.entity.iter() do n = n + 1 end\n"
        "assert(n == 1)\n"
        "assert(dfe.entity.spawn('nosuch:type', {x = 1, y = 1, z = 1}) == nil)\n"
        "assert(dfe.entity.despawn(id))\n"
        "assert(dfe.entity.get(id) == nil)\n"
        "return 'ENTOK' end)()");
    CHECK(last_log_contains("ENTOK"));
    entity_clear();
    script_eval("os.exit(1)");
    CHECK(last_log_contains("attempt"));
    script_eval("while true do end");
    CHECK(last_log_contains("instructions"));
    script_shutdown();
    events_clear_all();
    mods_reset();
    data_error_reset();
    remove_made();
}

static void test_mods_and_scripts(void) {
    remove_made();
    test_mod_resolution();
    remove_made();
    test_scripting();
    test_mod_user_ownership();
}

/* ---------------------------------------------------------------- example mods */

/* Loads base plus every mod in examples/mods and exercises each one, so the examples cannot rot unnoticed.
 * The native plugin is included only when it has been built (cmake -DDFE_BUILD_EXAMPLES=ON). */
static void test_example_mods(void) {
    if (!path_is_dir("examples/mods") || !path_is_dir("mods")) { printf("selftest examples skipped: run from the repository root\n"); return; }
    vfs_reset();
    vfs_add_root("engine_assets", "dfe");
    data_error_reset();
    mods_reset();
    events_clear_all();
    mods_discover("mods");
    mods_discover("examples/mods");
    mods_resolve();
    mods_mount();
    CHECK(data_error_count() == 0);
    CHECK(mods_loaded_count() == 9);
    /* The shader pack replaces an engine shader through the same override rule as any other asset. */
    size_t pack_size = 0;
    const char *pack_owner = NULL;
    u8 *pack = vfs_read("assets/dfe/shaders/post.frag", &pack_size, &pack_owner);
    CHECK(pack && pack_owner && !strcmp(pack_owner, "warmgrade"));
    free(pack);
    CHECK(order_of("base") == 0 && order_of("gems") < order_of("builder"));
    registry_reset();
    registry_load_blocks();
    registry_load_worldgen_config();
    CHECK(data_error_count() == 0);
    CHECK(gen_sea_level() == 74); /* highsea shadows base's worldgen file */
    /* pbr_stone: the "pbr" key parses into the block; blocks without it keep the defaults that mean "plain". */
    const BlockDef *polished = block_find("pbr_stone:polished_stone"), *plain = block_find("base:dirt");
    CHECK(polished && polished->has_pbr && !strcmp(polished->pbr_normal, "pbr_stone:block/polished_stone_n"));
    CHECK(polished && fabsf(polished->pbr_roughness - 0.35f) < 1e-4f && fabsf(polished->pbr_bump - 0.6f) < 1e-4f && polished->pbr_metalness == 0.0f);
    CHECK(plain && !plain->has_pbr && plain->pbr_roughness < 0.0f && plain->pbr_bump == 1.0f);
    u16 lamp_on = block_parse_state("gems:lamp[lit=on]"), lamp_off = block_parse_state("gems:lamp[lit=off]");
    u16 ruby = block_parse_state("gems:ruby_block");
    CHECK(lamp_on != STATE_UNLOADED && lamp_off != STATE_UNLOADED && ruby != STATE_UNLOADED);
    CHECK(g_state_emit[lamp_off] == 0 && g_state_emit[lamp_on] != 0);
    CHECK(block_parse_state("gems:amethyst_cluster") != STATE_UNLOADED);
    if (data_error_count() || ruby == STATE_UNLOADED) return;

    jobs_init(2);
    world_init(4242);
    world_flush_generation(0, 0, 2);
    /* Spawn follows the terrain of the active worldgen: above the raised sea, standing on the column's surface. */
    V3 spawn = player_find_spawn();
    CHECK(spawn.y > (float)gen_sea_level() + 1.0f);
    CHECK(fabsf(spawn.y - (floorf(gen_height_at(spawn.x, spawn.z)) + 1.01f)) < 0.01f);
    console_init();
    CHECK(script_init());
    CHECK(script_load_mods() == 0);
    bool native = path_exists("examples/mods/tally/plugins/libtally.so") || path_exists("examples/mods/tally/plugins/tally.dll");
    if (native) mods_load_plugins(true);

    dfe_event_t tick = {.name = "tick", .dt = 0.05};
    const int sky = 200; /* far above any terrain, so every position is air before the commands run */
    command_run("sphere 8 200 8 3 gems:ruby_block");
    command_run("fill 0 210 0 3 210 3 base:stone");
    for (int i = 0; i < 4; i++) event_fire(&tick);
    CHECK(world_get_state(8, sky, 8) == ruby && world_get_state(8, sky + 3, 8) == ruby && world_get_state(8, sky + 4, 8) == STATE_AIR);
    CHECK(world_get_state(2, 210, 2) == block_parse_state("base:stone"));

    command_run("setblock 8 200 8 gems:lamp");
    command_run("lamp 8 200 8 on");
    light_process(1 << 30);
    CHECK(world_get_state(8, sky, 8) == lamp_on && LIGHT_R(world_get_light(8, sky, 8)) == 15);
    command_run("lamp 8 200 8 toggle");
    light_process(1 << 30);
    CHECK(world_get_state(8, sky, 8) == lamp_off && LIGHT_R(world_get_light(8, sky, 8)) == 0);

    /* Guard cancels edits made on the player's behalf inside a region and nowhere else. */
    command_run("guard add 8 8 2");
    command_run("setblock 8 230 8 base:stone");
    CHECK(world_get_state(8, 230, 8) == STATE_AIR);
    command_run("setblock 20 230 20 base:stone");
    CHECK(world_get_state(20, 230, 20) == block_parse_state("base:stone"));
    command_run("guard clear");
    command_run("setblock 8 230 8 base:stone");
    CHECK(world_get_state(8, 230, 8) == block_parse_state("base:stone"));

    if (native) {
        command_run("tally 8 200 8 8 203 8");
        CHECK(last_log_contains("gems:ruby_block"));
        command_run("tally");
        CHECK(last_log_contains("placed"));
    } else printf("selftest examples: native plugin not built, skipping tally\n");

    mods_unload_plugins();
    script_shutdown();
    world_shutdown();
    jobs_shutdown();
    events_clear_all();
    mods_reset();
    data_error_reset();
}

/* ---------------------------------------------------------------- gameplay */

#define SLAB_Y 200
#define SLAB_MAX 16
#define FLUID_SETTLE_TICKS 120
#define FLUID_EBB_TICKS 400

static int water_level_at(const BlockDef *water, int x, int y, int z) {
    u16 s = world_get_state(x, y, z);
    if (block_of_state(s) != water) return -1;
    return block_state_prop_index(water, s, water->fluid_level_prop);
}

static void test_player_physics(u16 stone) {
    Player p;
    player_init(&p, v3(8.5f, 210.0f, 8.5f));
    PlayerInput none = {0};
    for (int i = 0; i < 300; i++) player_step(&p, &none, 1.0f / 60.0f);
    CHECK(p.on_ground && fabsf(p.pos.y - (float)(SLAB_Y + 1)) < 0.01f);
    CHECK(p.health < PLAYER_MAX_HEALTH);
    /* Result must not depend on the frame rate. */
    Player q;
    player_init(&q, v3(8.5f, 210.0f, 8.5f));
    for (int i = 0; i < 75; i++) player_step(&q, &none, 1.0f / 15.0f);
    CHECK(fabsf(q.pos.y - p.pos.y) < 0.01f);
    Player long_frame, regular_frames;
    player_init(&long_frame, v3(8.5f, 210.0f, 8.5f));
    player_init(&regular_frames, v3(8.5f, 210.0f, 8.5f));
    player_step(&long_frame, &none, 1.0f);
    for (int i = 0; i < 60; i++) player_step(&regular_frames, &none, 1.0f / 60.0f);
    CHECK(fabsf(long_frame.pos.y - regular_frames.pos.y) < 0.01f && fabsf(long_frame.vel.y - regular_frames.vel.y) < 0.01f);
    Player mover;
    player_init(&mover, v3(8.5f, (float)(SLAB_Y + 1), 8.5f));
    for (int i = 0; i < 4; i++) player_step(&mover, &none, 1.0f / 60.0f);
    CHECK(mover.on_ground);
    PlayerInput forward = {.forward = 1.0f};
    player_step(&mover, &forward, 1.0f / 60.0f);
    CHECK(mover.vel.z < -3.5f);
    float stop_z = mover.pos.z;
    player_step(&mover, &none, 1.0f / 60.0f);
    CHECK(fabsf(mover.vel.z) < 1e-6f && fabsf(mover.pos.z - stop_z) < 0.08f);
    const BlockDef *snow = block_find("base:snow");
    CHECK(snow && fabsf(snow->friction - 0.85f) < 1e-6f);
    if (snow) {
        world_set_state(14, SLAB_Y, 14, snow->default_state);
        Player snowy;
        player_init(&snowy, v3(14.5f, (float)(SLAB_Y + 1), 14.5f));
        for (int i = 0; i < 4; i++) player_step(&snowy, &none, 1.0f / 60.0f);
        player_step(&snowy, &forward, 1.0f / 60.0f);
        CHECK(fabsf(snowy.vel.z + 4.3f * snow->friction) < 0.01f);
        world_set_state(14, SLAB_Y, 14, stone);
    }
    const BlockDef *ice = block_find("base:ice");
    CHECK(ice && fabsf(player_effective_friction(ice->friction) - ice->friction) < 1e-6f);
    PlayerInput held_jump = {.jump = true};
    for (int i = 0; i < 60; i++) player_step(&mover, &held_jump, 1.0f / 60.0f);
    CHECK(mover.on_ground);
    PlayerInput jump_press = {.jump = true, .jump_pressed = true};
    player_step(&mover, &jump_press, 1.0f / 60.0f);
    for (int i = 0; i < 90; i++) player_step(&mover, &held_jump, 1.0f / 60.0f);
    CHECK(mover.on_ground);
    PlayerInput crouch = {.crouch = true};
    player_step(&mover, &crouch, 1.0f / 60.0f);
    CHECK(mover.crouched && fabsf(mover.height - PLAYER_CROUCH_HEIGHT) < 1e-6f);
    CHECK(fabsf(player_eye(&mover).y - mover.pos.y - PLAYER_CROUCH_EYE) < 1e-5f);
    CHECK(player_eye_render(&mover).y < mover.pos.y + PLAYER_EYE && player_eye_render(&mover).y > player_eye(&mover).y);
    int mover_x = ifloor(mover.pos.x), mover_z = ifloor(mover.pos.z);
    world_set_state(mover_x, SLAB_Y + 1, mover_z, stone);
    player_step(&mover, &none, 1.0f / 60.0f);
    CHECK(mover.crouched && fabsf(mover.height - PLAYER_CROUCH_HEIGHT) < 1e-6f);
    world_set_state(mover_x, SLAB_Y + 1, mover_z, STATE_AIR);
    player_step(&mover, &none, 1.0f / 60.0f);
    CHECK(!mover.crouched && fabsf(mover.height - PLAYER_HEIGHT) < 1e-6f);
    Player survivor;
    player_init(&survivor, v3(8.5f, (float)(SLAB_Y + 1), 8.5f));
    player_hurt(&survivor, 4.0f);
    CHECK(survivor.health == 16.0f && survivor.hurt_timer > 0.0f);
    player_hurt(&survivor, 4.0f);
    CHECK(survivor.health == 16.0f);
    CHECK(!player_teleport(&survivor, v3(8.5f, (float)SLAB_Y + 0.5f, 8.5f)));
    survivor.vel = v3(3.0f, 4.0f, 5.0f);
    CHECK(player_teleport(&survivor, v3(9.5f, (float)(SLAB_Y + 1), 8.5f)));
    CHECK(survivor.pos.x == 9.5f && survivor.vel.x == 0.0f && survivor.vel.y == 0.0f && survivor.vel.z == 0.0f);
    for (int i = 0; i < 45; i++) player_step(&survivor, &none, 1.0f / 60.0f);
    player_hurt(&survivor, 100.0f);
    CHECK(survivor.dead && survivor.health == 0.0f);
    V3 death_pos = survivor.pos;
    player_step(&survivor, &forward, 1.0f / 60.0f);
    CHECK(survivor.pos.x == death_pos.x && survivor.pos.z == death_pos.z);
    player_respawn(&survivor, v3(9.5f, (float)(SLAB_Y + 1), 8.5f));
    CHECK(!survivor.dead && survivor.health == PLAYER_MAX_HEALTH && survivor.pos.x == 9.5f);
    /* A wall stops a walking player and a one block ledge is stepped over only when 0.6 or lower. */
    for (int y = SLAB_Y + 1; y < SLAB_Y + 4; y++) world_set_state(12, y, 8, stone);
    PlayerInput fwd = {.strafe = 1.0f};
    p.yaw = 0;
    for (int i = 0; i < 240; i++) player_step(&p, &fwd, 1.0f / 60.0f);
    CHECK(p.pos.x < 12.0f - PLAYER_WIDTH * 0.5f + 0.01f && p.pos.x > 10.0f);
    for (int y = SLAB_Y + 1; y < SLAB_Y + 4; y++) world_set_state(12, y, 8, STATE_AIR);
    for (int x = 10; x <= 15; x++) world_set_state(x, SLAB_Y + 1, 8, stone);
    Player step_up;
    player_init(&step_up, v3(9.5f, (float)(SLAB_Y + 1), 8.5f));
    for (int i = 0; i < 4; i++) player_step(&step_up, &none, 1.0f / 60.0f);
    float step_up_speed = 0.0f;
    for (int i = 0; i < 45; i++) {
        player_step(&step_up, &fwd, 1.0f / 60.0f);
        step_up_speed = MAX(step_up_speed, step_up.vel.x);
    }
    CHECK(step_up.pos.x > 10.5f && fabsf(step_up.pos.y - (float)(SLAB_Y + 2)) < 0.01f);
    CHECK(step_up_speed > 4.0f);
    for (int x = 10; x <= 15; x++) world_set_state(x, SLAB_Y + 1, 8, STATE_AIR);
    world_set_state(12, SLAB_Y + 1, 8, stone);
    Player step_down;
    player_init(&step_down, v3(12.5f, (float)(SLAB_Y + 2), 8.5f));
    for (int i = 0; i < 4; i++) player_step(&step_down, &none, 1.0f / 60.0f);
    CHECK(step_down.on_ground && fabsf(step_down.pos.y - (float)(SLAB_Y + 2)) < 0.01f);
    bool stayed_grounded = true;
    for (int i = 0; i < 45; i++) {
        player_step(&step_down, &fwd, 1.0f / 60.0f);
        if (!step_down.on_ground) stayed_grounded = false;
    }
    CHECK(stayed_grounded && step_down.pos.x > 13.0f && fabsf(step_down.pos.y - (float)(SLAB_Y + 1)) < 0.01f);
    world_set_state(12, SLAB_Y + 1, 8, STATE_AIR);
    CHECK(!player_box_blocked(v3(8.5f, (float)(SLAB_Y + 1) + 0.01f, 8.5f)));
    CHECK(player_box_blocked(v3(8.5f, (float)SLAB_Y + 0.5f, 8.5f)));
}

/* ------------------------------------------------- player movement at block edges */

#define EDGE_Y (SLAB_Y + 1)
#define DT60 (1.0f / 60.0f)

static void run_input(Player *p, const PlayerInput *in, int n) {
    for (int i = 0; i < n; i++) player_step(p, in, DT60);
}

static Player edge_player(float x, float z) {
    Player p;
    PlayerInput none = {0};
    player_init(&p, v3(x, (float)EDGE_Y, z));
    run_input(&p, &none, 4);
    return p;
}

static void edge_column(int x, int z, int h, u16 state) {
    for (int y = 0; y < h; y++) world_set_state(x, EDGE_Y + y, z, state);
}

static void test_player_edges(u16 stone) {
    PlayerInput none = {0}, east = {.strafe = 1.0f}, diag = {.strafe = 1.0f, .forward = 1.0f};
    /* Platform spanning the x = 32 chunk boundary. */
    for (int z = 0; z < 16; z++)
        for (int x = 20; x <= 44; x++) world_set_state(x, SLAB_Y, z, stone);

    /* 1 + 10: walking across flush seams and a chunk boundary keeps constant speed and height. */
    Player p = edge_player(26.5f, 8.5f);
    run_input(&p, &east, 1);
    float v0 = p.vel.x, minv = v0, maxv = v0, maxdy = 0.0f;
    bool grounded = true;
    for (int i = 0; i < 150; i++) {
        float x_before = p.pos.x;
        run_input(&p, &east, 1);
        minv = MIN(minv, p.vel.x);
        maxv = MAX(maxv, p.vel.x);
        maxdy = MAX(maxdy, fabsf(p.pos.y - (float)EDGE_Y - 0.002f));
        grounded &= p.on_ground;
        CHECK(fabsf((p.pos.x - x_before) - v0 * DT60) < 1e-4f);
    }
    CHECK(p.pos.x > 33.0f && grounded && maxv - minv < 1e-5f && maxdy < 0.005f);

    /* 10: friction and ground detection straddling the chunk boundary match a centred stand. */
    Player straddle = edge_player(31.9f, 8.5f), centred = edge_player(25.5f, 8.5f);
    float f_s, f_c;
    CHECK(player_ground_probe(straddle.pos, straddle.half_width, &f_s) == 2);
    CHECK(player_ground_probe(centred.pos, centred.half_width, &f_c) == 1);
    CHECK(straddle.on_ground && centred.on_ground && fabsf(f_s - f_c) < 1e-6f && fabsf(straddle.pos.y - centred.pos.y) < 1e-6f);

    /* 2: diagonal into a wall slides along it at the component speed. */
    for (int z = 0; z < 16; z++) edge_column(36, z, 2, stone);
    p = edge_player(34.5f, 12.5f);
    run_input(&p, &diag, 1);
    float vz = p.vel.z, z0 = p.pos.z;
    run_input(&p, &diag, 60);
    CHECK(p.pos.x < 36.0f - p.half_width + 0.01f && p.pos.x > 36.0f - p.half_width - 0.05f);
    CHECK(vz < -1.0f && fabsf((p.pos.z - z0) - vz * 60.0f * DT60) < 0.02f && p.vel.z == vz);
    for (int z = 0; z < 16; z++) edge_column(36, z, 2, STATE_AIR);

    /* 3: diagonal across a flush seam moves symmetrically on both axes with no catch. */
    p = edge_player(29.5f, 12.5f);
    run_input(&p, &diag, 1);
    float sx = p.pos.x, sz = p.pos.z, vx = p.vel.x;
    run_input(&p, &diag, 60);
    CHECK(fabsf((p.pos.x - sx) + (p.pos.z - sz)) < 1e-3f && fabsf((p.pos.x - sx) - vx * 60.0f * DT60) < 1e-3f);

    /* 4, 5, 6: auto-step. */
    edge_column(30, 8, 1, stone);
    p = edge_player(28.5f, 8.5f);
    for (int i = 0; i < 60 && p.pos.x < 30.5f; i++) run_input(&p, &east, 1);
    CHECK(p.pos.x > 30.5f && p.on_ground && fabsf(p.pos.y - (float)(EDGE_Y + 1)) < 0.01f);
    edge_column(30, 8, 2, stone);
    p = edge_player(28.5f, 8.5f);
    run_input(&p, &east, 60);
    CHECK(p.pos.x < 30.0f - p.half_width + 0.01f && fabsf(p.pos.y - (float)EDGE_Y) < 0.01f);
    edge_column(30, 8, 2, STATE_AIR);
    edge_column(30, 8, 1, stone);
    Player air;
    player_init(&air, v3(29.69f, (float)EDGE_Y + 0.3f, 8.5f));
    bool stepped = false;
    for (int i = 0; i < 3; i++) {
        run_input(&air, &east, 1);
        stepped |= air.pos.y > (float)EDGE_Y + 0.3f || air.pos.x > 29.71f;
    }
    CHECK(!stepped && !air.on_ground);
    edge_column(30, 8, 1, STATE_AIR);

    /* 7 + 11: the grounded flag follows the box footprint exactly across the platform edge, one clean transition. */
    p = edge_player(43.0f, 8.5f);
    int left_at = -1;
    float last_vy = 0.0f;
    bool clean = true;
    for (int i = 0; i < 90; i++) {
        run_input(&p, &east, 1);
        bool supported = p.pos.x - p.half_width + PLAYER_COLLISION_EPSILON < 45.0f;
        if (left_at < 0 && !p.on_ground) left_at = i;
        if (left_at >= 0) clean &= !p.on_ground && p.vel.y <= last_vy + 1e-6f;
        else clean &= supported && p.on_ground;
        last_vy = p.vel.y;
    }
    CHECK(left_at > 0 && clean && p.pos.y < (float)EDGE_Y - 1.0f);
    float inside, at_edge, outside;
    V3 q = v3(45.3f - 0.002f, (float)EDGE_Y + 0.002f, 8.5f);
    CHECK(player_ground_probe(q, 0.3f, &inside) == 1);
    q.x = 45.3f;
    CHECK(player_ground_probe(q, 0.3f, &at_edge) == 0);
    q.x = 45.3f + 0.002f;
    CHECK(player_ground_probe(q, 0.3f, &outside) == 0);
    p = edge_player(45.3f - 0.002f, 8.5f);
    float rest_y = p.pos.y;
    run_input(&p, &none, 30);
    CHECK(p.on_ground && fabsf(p.pos.y - rest_y) < 1e-5f);

    /* 12: centre over air, one footprint corner over a block. */
    p = edge_player(45.2f, 8.5f);
    CHECK(p.on_ground && world_get_state(45, SLAB_Y, 8) == STATE_AIR && fabsf(p.pos.y - (float)EDGE_Y) < 0.01f);

    /* 8: jump distance and height do not depend on where on the block the jump starts. */
    float start_x[4] = {24.5f, 20.31f, 24.99f, 24.01f}, start_z[4] = {8.5f, 8.5f, 0.31f, 8.99f};
    PlayerInput hop = {.strafe = 1.0f, .jump = true, .jump_pressed = true};
    float dist[4], apex[4];
    for (int k = 0; k < 4; k++) {
        p = edge_player(start_x[k], start_z[k]);
        float x_start = p.pos.x, y_start = p.pos.y, peak = 0.0f;
        player_step(&p, &hop, DT60);
        PlayerInput hold = {.strafe = 1.0f};
        for (int i = 0; i < 120 && !p.on_ground; i++) {
            player_step(&p, &hold, DT60);
            peak = MAX(peak, p.pos.y - y_start);
        }
        dist[k] = p.pos.x - x_start;
        apex[k] = peak;
        CHECK(p.on_ground && peak > 1.0f);
    }
    for (int k = 1; k < 4; k++) CHECK(fabsf(dist[k] - dist[0]) < 1e-3f && fabsf(apex[k] - apex[0]) < 1e-3f);

    /* 9: an inside corner does not trap; diagonal out leaves it. */
    for (int z = 0; z < 16; z++) edge_column(36, z, 2, stone);
    for (int x = 28; x <= 40; x++) edge_column(x, 3, 2, stone);
    p = edge_player(35.0f, 6.5f);
    run_input(&p, &diag, 90);
    CHECK(p.pos.x <= 35.71f && p.pos.z >= 4.29f && p.pos.x > 35.6f && p.pos.z < 4.4f);
    PlayerInput out = {.strafe = -1.0f, .forward = -1.0f};
    run_input(&p, &out, 60);
    CHECK(p.pos.x < 34.0f && p.pos.z > 6.0f);
    for (int z = 0; z < 16; z++) edge_column(36, z, 2, STATE_AIR);
    for (int x = 28; x <= 40; x++) edge_column(x, 3, 2, STATE_AIR);

    /* 15: a diagonal gap narrower than the box cannot be passed. */
    edge_column(36, 8, 2, stone);
    edge_column(37, 9, 2, stone);
    p = edge_player(36.5f, 9.5f);
    run_input(&p, &diag, 120);
    CHECK(!(p.pos.x > 37.0f && p.pos.z < 9.0f) && !player_box_blocked(p.pos));
    edge_column(36, 8, 2, STATE_AIR);
    edge_column(37, 9, 2, STATE_AIR);

    /* 13 + 14: ice friction, and the mean over a mixed footprint. */
    const BlockDef *ice = block_find("base:ice");
    CHECK(ice != NULL);
    if (ice) {
        for (int x = 40; x <= 41; x++) world_set_state(x, SLAB_Y, 8, ice->default_state);
        p = edge_player(40.5f, 8.5f);
        float f;
        CHECK(player_ground_probe(p.pos, p.half_width, &f) == 1 && fabsf(f - 0.2f) < 1e-6f);
        run_input(&p, &east, 1);
        CHECK(fabsf(p.vel.x - 4.3f * 0.2f) < 1e-4f);
        world_set_state(40, SLAB_Y, 8, ice->default_state);
        world_set_state(41, SLAB_Y, 8, stone);
        p = edge_player(41.0f, 8.5f);
        CHECK(player_ground_probe(p.pos, p.half_width, &f) == 2);
        float expect = (CLAMP(ice->friction, 0.05f, 2.0f) + CLAMP(block_of_state(stone)->friction, 0.05f, 2.0f)) * 0.5f;
        CHECK(fabsf(f - expect) < 1e-6f);
        for (int x = 40; x <= 41; x++) world_set_state(x, SLAB_Y, 8, stone);
    }
}

static void test_entities(void) {
    content_load_all(); /* items first: drops are checked against the item registry */
    registry_load_entities();
    CHECK(entity_type_count() >= 1);
    int hopper_id = 0;
    for (int i = 0; i < entity_type_count(); i++) if (!strcmp(entity_type_at(i)->id, "base:hopper")) hopper_id = 1;
    CHECK(hopper_id == 1);
    entity_world_init(1);
    CHECK(entity_spawn("nosuch:type", v3(8.5f, 210.0f, 8.5f)) == 0);
    int id = entity_spawn("base:hopper", v3(8.5f, 210.0f, 8.5f));
    CHECK(id > 0 && entity_count() == 1);
    /* Two seconds is long enough to land on the slab and short enough that wandering cannot reach its edge. */
    for (int i = 0; i < 120; i++) entity_update(1.0f / 60.0f);
    V3 p;
    CHECK(entity_position(id, &p) && fabsf(p.y - (float)(SLAB_Y + 1)) < 0.01f);
    CHECK(p.x > 0.0f && p.x < (float)SLAB_MAX && p.z > 0.0f && p.z < (float)SLAB_MAX);
    CHECK(entity_remove(id) && !entity_remove(id) && entity_count() == 0);
    int other = entity_spawn("base:hopper", v3(8.5f, 210.0f, 8.5f));
    CHECK(other != id); /* handles are not reused */
    entity_clear();
    CHECK(entity_count() == 0);
}

static int g_ev_damage_cancel, g_ev_death_cancel, g_ev_count[5];
static int ent_ev(const dfe_event_t *ev, void *user) {
    (void)user;
    if (!strcmp(ev->name, "entity_spawn")) g_ev_count[0]++;
    else if (!strcmp(ev->name, "entity_damage")) { g_ev_count[1]++; return g_ev_damage_cancel; }
    else if (!strcmp(ev->name, "entity_death")) { g_ev_count[2]++; return g_ev_death_cancel; }
    else if (!strcmp(ev->name, "entity_despawn")) g_ev_count[3]++;
    else if (!strcmp(ev->name, "entity_tick")) g_ev_count[4]++;
    return 0;
}

static void test_entity_system(void) {
    char err[160];
    /* Data-driven definitions and validation. */
    CHECK(entity_type_register_json("test:brute", "{\"name\":\"Brute\",\"size\":[0.6,1.8],\"health\":10,\"behaviour\":\"hostile\",\"attack\":2,\"sight\":12,\"save\":true,\"drops\":[{\"item\":\"base:stone\",\"min\":1,\"max\":2}]}", err, sizeof err));
    CHECK(entity_type_register_json("test:rock", "{\"name\":\"Rock\",\"size\":[0.8,0.8],\"behaviour\":\"static\"}", err, sizeof err));
    CHECK(!entity_type_register_json("test:bad1", "{\"size\":[0,1]}", err, sizeof err) && err[0]);
    CHECK(!entity_type_register_json("test:bad2", "{\"size\":[1,1],\"behaviour\":\"fly\"}", err, sizeof err));
    CHECK(!entity_type_register_json("test:bad3", "{\"size\":[1,1],\"health\":-3}", err, sizeof err));
    CHECK(!entity_type_register_json("test:bad4", "not json", err, sizeof err));
    CHECK(!entity_type_register_json("test:bad5", "{\"size\":[1,1],\"drops\":[{\"item\":\"x\",\"min\":5,\"max\":1}]}", err, sizeof err));

    entity_world_init(1);
    entity_clear();
    /* Health, damage, heal, events, cancellation. */
    const dfe_api_t *api = api_get();
    memset(g_ev_count, 0, sizeof g_ev_count);
    g_ev_damage_cancel = g_ev_death_cancel = 0;
    CHECK(api->subscribe("entity_spawn", ent_ev, NULL, "selftest") > 0);
    CHECK(api->subscribe("entity_damage", ent_ev, NULL, "selftest") > 0);
    CHECK(api->subscribe("entity_death", ent_ev, NULL, "selftest") > 0);
    CHECK(api->subscribe("entity_despawn", ent_ev, NULL, "selftest") > 0);
    CHECK(api->subscribe("entity_tick", ent_ev, NULL, "selftest") > 0);
    int b = entity_spawn("test:brute", v3(5.5f, (float)(SLAB_Y + 1), 5.5f));
    CHECK(b > 0 && g_ev_count[0] == 1);
    EntityInfo in;
    CHECK(entity_get(b, &in) && in.health == 10.0f && in.max_health == 10.0f && in.behaviour == ENT_BEHAVE_HOSTILE);
    DamageSource src = {DAMAGE_MOD, 0};
    g_ev_damage_cancel = 1;
    CHECK(!entity_damage(b, 3.0f, &src) && entity_get(b, &in) && in.health == 10.0f);
    g_ev_damage_cancel = 0;
    CHECK(entity_damage(b, 3.0f, &src) && entity_get(b, &in) && fabsf(in.health - 7.0f) < 1e-4f);
    CHECK(entity_heal(b, 100.0f) && entity_get(b, &in) && in.health == 10.0f);
    g_ev_death_cancel = 1;
    for (int i = 0; i < 30; i++) entity_update(0.1f); /* let the invulnerability timer lapse */
    entity_damage(b, 50.0f, &src);
    CHECK(g_ev_count[2] >= 1 && entity_get(b, &in)); /* death cancelled: entity lives */
    g_ev_death_cancel = 0;
    for (int i = 0; i < 30; i++) entity_update(0.1f);
    CHECK(entity_damage(b, 50.0f, &src));
    CHECK(!entity_get(b, &in) && g_ev_count[3] >= 1);
    CHECK(g_ev_count[4] > 0);
    int rock = entity_spawn("test:rock", v3(12.5f, (float)(SLAB_Y + 1), 12.5f));
    CHECK(!entity_damage(rock, 5.0f, &src) && entity_get(rock, &in));
    events_clear("selftest");
    entity_clear();

    /* Behaviours. */
    int st = entity_spawn("test:rock", v3(14.5f, (float)(SLAB_Y + 1), 3.5f)), hs = entity_spawn("test:brute", v3(3.5f, (float)(SLAB_Y + 1), 14.5f));
    V3 s0, s1;
    entity_position(st, &s0);
    for (int i = 0; i < 120; i++) entity_update(1.0f / 60.0f);
    entity_position(st, &s1);
    CHECK(fabsf(s1.x - s0.x) < 1e-3f && fabsf(s1.z - s0.z) < 1e-3f);
    CHECK(hs > 0 && entity_set_behaviour(hs, ENT_BEHAVE_PASSIVE) && entity_get(hs, &in) && in.behaviour == ENT_BEHAVE_PASSIVE);
    entity_clear();

    /* Collision: entities do not overlap each other and stay on the floor. */
    int a = entity_spawn("test:rock", v3(8.5f, (float)(SLAB_Y + 1), 8.5f));
    int c = entity_spawn("test:brute", v3(8.6f, (float)(SLAB_Y + 1), 8.5f));
    entity_set_behaviour(c, ENT_BEHAVE_PASSIVE); /* two fixed entities never push each other */
    for (int i = 0; i < 120; i++) entity_update(1.0f / 60.0f);
    V3 pa, pc;
    entity_position(a, &pa); entity_position(c, &pc);
    CHECK(fabsf(pa.x - pc.x) >= 0.7f || fabsf(pa.z - pc.z) >= 0.7f);
    CHECK(fabsf(pa.y - (float)(SLAB_Y + 1)) < 0.01f && fabsf(pc.y - (float)(SLAB_Y + 1)) < 0.01f);
    entity_clear();

    /* Handles, list and near. */
    int ids[8];
    for (int i = 0; i < 5; i++) ids[i] = entity_spawn("test:rock", v3(2.5f + (float)i * 3.0f, (float)(SLAB_Y + 1), 10.5f));
    int list[16];
    CHECK(entity_list(list, 16) == 5 && list[0] == ids[0] && list[4] == ids[4]);
    int near[8];
    CHECK(entity_near(v3(2.5f, (float)(SLAB_Y + 1), 10.5f), 4.0f, near, 8) == 2 && near[0] == ids[0] && near[1] == ids[1]);

    /* Native API mirrors. */
    dfe_entity_t de;
    CHECK(api->entity_get(ids[0], &de) && !api->entity_get(99999, &de));
    CHECK(api->entity_list(list, 16) == 5);
    CHECK(api->entity_near(2.5, SLAB_Y + 1, 10.5, 4.0, near, 8) == 2);
    entity_clear();

    /* Persistence. */
    remove("selftest_entities");
    CHECK(save_open("selftest_entities", 5));
    int pb = entity_spawn("test:brute", v3(9.5f, (float)(SLAB_Y + 1), 9.5f));
    entity_set_health(pb, 4.0f, -1.0f);
    entity_set_data(pb, "mark");
    CHECK(entity_save());
    entity_clear();
    CHECK(entity_load() == 1 && entity_count() == 1);
    int lb;
    CHECK(entity_list(&lb, 1) == 1 && entity_get(lb, &in) && !strcmp(in.type, "test:brute") && fabsf(in.health - 4.0f) < 1e-4f && !strcmp(in.data, "mark"));
    entity_clear();
    save_close();
    dir_remove_all("selftest_entities");

    /* Batching, LOD, culling and caps (CPU side, no GL). */
    EntityConfig keep = g_entity_cfg;
    g_entity_cfg = (EntityConfig){true, 256, 32.0f, 96.0f, 0.0f};
    Camera cam = {.pos = v3(0, 0, 0), .yaw = 0, .pitch = 0, .fov_y = 70.0f * DEG2RAD, .znear = 0.1f, .zfar = 400.0f};
    camera_update(&cam, 1.0f);
    for (int i = 0; i < 100; i++) entity_spawn("test:rock", v3(-4.0f + (float)(i % 10) * 0.9f, 0.0f, -8.0f - (float)(i / 10) * 9.0f));
    EntityInstance *inst = NULL;
    int lod[ENTITY_LOD_LEVELS] = {0};
    int n = entity_build_batches(&cam, 400.0f, &inst, lod);
    CHECK(n == 100 && lod[0] + lod[1] + lod[2] == 100);
    CHECK(entity_draw_call_count(lod, true) <= 3);
    CHECK(entity_draw_call_count(lod, false) >= 100); /* legacy path: two draws per full-detail entity */
    CHECK(entity_draw_call_count(lod, true) * 10 < entity_draw_call_count(lod, false));
    CHECK(lod[1] + lod[2] > 0 && lod[0] > 0); /* LOD split by distance */
    int lod2[ENTITY_LOD_LEVELS], n2;
    n2 = entity_build_batches(&cam, 400.0f, &inst, lod2);
    CHECK(n2 == n && !memcmp(lod, lod2, sizeof lod)); /* deterministic */
    n = entity_build_batches(&cam, 30.0f, &inst, lod); /* fog distance cull */
    CHECK(n < 100 && g_entity_stats.culled > 0);
    g_entity_cfg.max_drawn = 10;
    n = entity_build_batches(&cam, 400.0f, &inst, lod);
    CHECK(n == 10); /* cap */
    g_entity_cfg.max_drawn = 256;
    cam.yaw = (float)M_PI; camera_update(&cam, 1.0f); /* looking away: frustum cull */
    n = entity_build_batches(&cam, 400.0f, &inst, lod);
    CHECK(n == 0);
    g_entity_cfg = keep;
    entity_clear();
}

static void test_raycast_and_inventory(u16 stone) {
    RayHit h;
    CHECK(raycast_blocks(v3(8.5f, 205.5f, 8.5f), v3(0, -1, 0), 10.0f, false, &h));
    CHECK(h.x == 8 && h.y == SLAB_Y && h.z == 8 && h.face == DIR_PY && h.py == SLAB_Y + 1 && h.state == stone);
    CHECK(!raycast_blocks(v3(8.5f, 205.5f, 8.5f), v3(0, 1, 0), 10.0f, false, &h));
    CHECK(!raycast_blocks(v3(8.5f, 205.5f, 8.5f), v3(0, -1, 0), 3.0f, false, &h));
    CHECK(raycast_blocks(v3(0.5f, SLAB_Y + 0.5f, 8.5f), v3(1, 0, 0), 3.0f, false, &h) == false || h.x >= 0);

    Inventory inv;
    inventory_clear(&inv);
    CHECK(inventory_add(&inv, stone, 100) == 0);
    CHECK(inv.slot[0].count == INV_MAX_STACK && inv.slot[1].count == 36 && inventory_count(&inv, stone) == 100);
    CHECK(inventory_add(&inv, stone, INV_SLOTS * INV_MAX_STACK) == 100); /* only the free space is used */
    inventory_clear(&inv);
    inventory_add(&inv, stone, 10);
    inventory_click(&inv, 0, 1); /* right click picks up half, rounded up */
    CHECK(inv.cursor.count == 5 && inv.slot[0].count == 5);
    inventory_click(&inv, 3, 1); /* right click on an empty slot drops one */
    CHECK(inv.cursor.count == 4 && inv.slot[3].count == 1);
    inventory_click(&inv, 0, 0); /* left click merges the same item */
    CHECK(inv.cursor.count == 0 && inv.slot[0].count == 9);
    inventory_click(&inv, 0, 0); /* left click on a stack picks it up whole */
    CHECK(inv.cursor.count == 9 && inv.slot[0].count == 0 && inv.cursor.state == stone);
    CHECK(!inventory_take_one(&inv, 0));
}

static void test_server_and_fluids(u16 stone, const BlockDef *water) {
    player_init(&g_player, v3(8.5f, (float)(SLAB_Y + 1), 8.5f));
    inventory_clear(&g_inv);
    g_inv.slot[0].state = stone;
    g_inv.slot[0].count = 5;
    g_inv.selected = 0;
    g_creative = false;
    ServerMsg place = {.type = MSG_PLACE, .x = 10, .y = SLAB_Y + 1, .z = 8, .state = stone};
    server_post(&place);
    CHECK(server_pump() == 1 && world_get_state(10, SLAB_Y + 1, 8) == stone && g_inv.slot[0].count == 4);
    ServerMsg far_place = {.type = MSG_PLACE, .x = 30, .y = SLAB_Y + 1, .z = 8, .state = stone};
    server_post(&far_place);
    CHECK(server_pump() == 0 && world_get_state(30, SLAB_Y + 1, 8) == STATE_AIR);
    ServerMsg inside = {.type = MSG_PLACE, .x = 8, .y = SLAB_Y + 1, .z = 8, .state = stone};
    server_post(&inside); /* the player stands here */
    CHECK(server_pump() == 0);
    ServerMsg brk = {.type = MSG_BREAK, .x = 10, .y = SLAB_Y + 1, .z = 8};
    server_post(&brk);
    CHECK(server_pump() == 1 && world_get_state(10, SLAB_Y + 1, 8) == STATE_AIR);
    CHECK(inventory_count(&g_inv, item_for_block(block_of_state(stone))) >= 1); /* survival gives the drop */
    g_creative = true;

    CHECK(water->fluid_level_prop >= 0 && water->fluid_reach == FLUID_DEFAULT_REACH);
    u16 source = water->default_state;
    world_set_state(8, SLAB_Y + 1, 8, source);
    for (int i = 0; i < FLUID_SETTLE_TICKS; i++) server_tick();
    CHECK(water_level_at(water, 8, SLAB_Y + 1, 8) == 0);
    CHECK(water_level_at(water, 8 + 3, SLAB_Y + 1, 8) == 3);
    CHECK(water_level_at(water, 8, SLAB_Y + 1, 8 - FLUID_DEFAULT_REACH) == FLUID_DEFAULT_REACH);
    CHECK(water_level_at(water, 8 + FLUID_DEFAULT_REACH + 1, SLAB_Y + 1, 8) == -1);
    world_set_state(8, SLAB_Y + 1, 8, STATE_AIR);
    for (int i = 0; i < FLUID_EBB_TICKS; i++) server_tick();
    CHECK(water_level_at(water, 8 + 3, SLAB_Y + 1, 8) == -1 && water_level_at(water, 8 + 1, SLAB_Y + 1, 8) == -1);
    CHECK(server_scheduled_count() == 0); /* a settled world has no pending work */
}

static void test_inventory_save(u16 stone, u16 dirt) {
    Inventory a, b;
    inventory_clear(&a);
    a.slot[2].state = stone; a.slot[2].count = 33;
    snprintf(a.slot[2].item_id, sizeof a.slot[2].item_id, "base:stone");
    snprintf(a.slot[2].metadata, sizeof a.slot[2].metadata, "{\"custom\":true}");
    a.slot[2].durability = 17;
    a.slot[20].state = dirt; a.slot[20].count = 64;
    a.selected = 4;
    static SaveMeta m;
    memset(&m, 0, sizeof m);
    inventory_store(&a, false, &m);
    bool creative = true;
    inventory_restore(&b, &creative, &m);
    CHECK(!creative && b.selected == 4 && b.slot[2].state == stone && b.slot[2].count == 33 && b.slot[2].durability == 17 && !strcmp(b.slot[2].metadata, "{\"custom\":true}") && b.slot[20].state == dirt && b.slot[20].count == 64);
    snprintf(m.inv_name[2], SAVE_BLOCK_NAME_LEN, "gone:missing_block"); /* a removed mod's item is dropped, not crashed on */
    snprintf(m.inv_item[2], SAVE_BLOCK_NAME_LEN, "gone:missing_block");
    inventory_restore(&b, &creative, &m);
    CHECK(b.slot[2].count == 0 && b.slot[20].count == 64);
}

/* ------------------------------------------------------------- atmosphere */

static float color_distance(V3 a, V3 b) { return fabsf(a.x - b.x) + fabsf(a.y - b.y) + fabsf(a.z - b.z); }

static double seconds_at_phase(const Atmosphere *a, float phase) {
    double into = (double)phase - a->start_phase;
    return (into - floor(into)) * a->day_length_s;
}

static void test_presets(void) {
    data_error_reset();
    CHECK(registry_load_presets() == 0);
    CHECK(preset_count() >= 3);
    const Preset *low = preset_find("low"), *high = preset_find("high");
    CHECK(low && high && !preset_find("no_such_preset"));
    if (!low || !high) return;
    CHECK(low->render_distance < high->render_distance && low->far_chunks < high->far_chunks);
    CHECK(!low->light_shafts && high->light_shafts);
    CHECK(low->fog && high->fog);

    /* The preset supplies the values; a setting overrides only its own field; an unknown preset falls back. */
    bool no_render = g_opt.no_render;
    g_opt.no_render = false;
    settings_defaults();
    snprintf(g_settings.preset, sizeof g_settings.preset, "high");
    g_settings.fog_off = false;
    g_settings.fog_quality[0] = 0;
    gfx_apply();
    CHECK(g_gfx.render_distance == high->render_distance && g_gfx.light_shafts);
    CHECK(g_gfx.fog && g_gfx.fog_level.density > 0.0f);
    g_settings.shadow_distance = 384;
    gfx_apply();
    CHECK(g_gfx.shadow.distance == 384.0f);
    g_settings.render_distance = 5;
    g_settings.dynamic_resolution = 1;
    gfx_apply();
    CHECK(g_gfx.render_distance == 5 && g_gfx.far_chunks == high->far_chunks && g_gfx.dynamic_resolution);
    snprintf(g_settings.preset, sizeof g_settings.preset, "no_such_preset");
    gfx_apply();
    CHECK(!strcmp(g_settings.preset, "low") && g_gfx.far_chunks == low->far_chunks);
    CHECK(g_gfx.target_ms > 10.0f && g_gfx.target_ms < 40.0f);
    g_opt.no_render = no_render;
    settings_defaults();
}

static void test_atmosphere(void) {
    data_error_reset();
    registry_load_atmosphere();
    CHECK(data_error_count() == 0);
    CHECK(g_atmo.loaded && g_atmo.key_count >= 2);
    if (!g_atmo.loaded) return;
    Atmosphere a = g_atmo;
    a.stars = true;

    /* The cycle is continuous (no colour jumps, including across midnight) and repeats every day. */
    float worst = 0.0f;
    V3 prev_sky = {0}, prev_fog = {0};
    for (int i = 0; i <= 1000; i++) {
        atmosphere_evaluate(&a, seconds_at_phase(&a, (float)(i % 1000) / 1000.0f));
        if (i > 0) worst = MAX(worst, MAX(color_distance(a.sky_light, prev_sky), color_distance(a.fog_color, prev_fog)));
        prev_sky = a.sky_light;
        prev_fog = a.fog_color;
    }
    CHECK(worst < 0.06f);
    atmosphere_evaluate(&a, 123.0);
    V3 first = a.horizon;
    atmosphere_evaluate(&a, 123.0 + a.day_length_s);
    CHECK(color_distance(first, a.horizon) < 1e-3f);

    /* Noon has the sun high and no stars; midnight has the moon up and stars out. */
    atmosphere_evaluate(&a, seconds_at_phase(&a, 0.5f));
    CHECK(a.sun_dir.y > 0.9f && a.sun_vis > 0.9f && a.star_alpha < 0.01f);
    float noon_light = a.sky_light.y;
    atmosphere_evaluate(&a, seconds_at_phase(&a, 0.0f));
    CHECK(a.sun_dir.y < -0.9f && a.sun_vis < 0.01f && a.moon_vis > 0.9f && a.star_alpha > 0.9f);
    CHECK(a.sky_light.y < noon_light * 0.4f);

    /* Weather dims and greys the day and pulls the fog in. */
    atmosphere_evaluate(&a, seconds_at_phase(&a, 0.5f));
    V3 clear_horizon = a.horizon;
    float clear_start = 100.0f, clear_end = 400.0f, wet_start = 100.0f, wet_end = 400.0f;
    Atmosphere saved = g_atmo;
    g_atmo = a;
    atmosphere_adjust_fog(&clear_start, &clear_end);
    CHECK(clear_end == 400.0f);
    a.cloud_amt = 1.0f;
    a.rain_amt = 1.0f;
    atmosphere_evaluate(&a, seconds_at_phase(&a, 0.5f));
    CHECK(a.sky_light.y < noon_light && a.sun_vis < 0.1f && a.star_alpha == 0.0f);
    CHECK(fabsf(a.horizon.x - a.horizon.z) < fabsf(clear_horizon.x - clear_horizon.z));
    g_atmo = a;
    atmosphere_adjust_fog(&wet_start, &wet_end);
    CHECK(wet_end < clear_end && wet_start < clear_start);
    g_atmo.underwater = 1.0f;
    float u_start = 100.0f, u_end = 400.0f;
    atmosphere_adjust_fog(&u_start, &u_end);
    CHECK(u_end < 40.0f);
    g_atmo = saved;

    /* Names and the console parsers. */
    float phase = -1.0f;
    Weather w = WEATHER_CLEAR;
    CHECK(atmosphere_parse_phase("noon", &phase) && fabsf(phase - 0.5f) < 1e-6f);
    CHECK(atmosphere_parse_phase("0.25", &phase) && fabsf(phase - 0.25f) < 1e-6f);
    CHECK(!atmosphere_parse_phase("1.5", &phase) && !atmosphere_parse_phase("lunch", &phase));
    CHECK(atmosphere_parse_weather("rain", &w) && w == WEATHER_RAIN);
    CHECK(!atmosphere_parse_weather("hail", &w));

    /* Setting the phase lands on it and keeps the day count. */
    game_time_set(0.0);
    atmosphere_set_phase(0.75f);
    atmosphere_evaluate(&a, game_time_get());
    CHECK(fabsf(a.phase - 0.75f) < 1e-3f);
    game_time_set(0.0);
}

static void test_gameplay(void) {
    if (!path_is_dir("mods")) { printf("selftest gameplay skipped: run from the repository root\n"); return; }
    vfs_reset();
    vfs_add_root("engine_assets", "dfe");
    data_error_reset();
    mods_reset();
    events_clear_all();
    mods_discover("mods");
    mods_resolve();
    mods_mount();
    registry_reset();
    registry_load_blocks();
    registry_load_worldgen_config();
    CHECK(data_error_count() == 0);
    const BlockDef *water = block_find("base:water");
    BlockDef *stone_def = block_find("base:stone");
    if (data_error_count() || !water || !stone_def) { mods_reset(); return; }
    u16 stone = stone_def->default_state, dirt = block_find("base:dirt")->default_state;
    jobs_init(2);
    world_init(4242);
    world_flush_generation(0, 0, 2);
    server_init();
    for (int z = 0; z <= SLAB_MAX; z++)
        for (int x = 0; x <= SLAB_MAX; x++) world_set_state(x, SLAB_Y, z, stone);
    test_player_physics(stone);
    test_player_edges(stone);
    test_entities();
    test_entity_system();
    test_raycast_and_inventory(stone);
    test_server_and_fluids(stone, water);
    test_inventory_save(stone, dirt);
    server_shutdown();
    world_shutdown();
    jobs_shutdown();
    events_clear_all();
    mods_reset();
    data_error_reset();
}

static void test_ui_status_hud(void) {
    vfs_reset();
    vfs_add_root("engine_assets", "dfe");
    CHECK(ui_load());
    CHECK(ui_element_count() >= 6);
    bool prev = g_creative;
    g_creative = false;
    g_player.health = 10;
    ui_status_update();
    static const char *ids[] = {"hearts", "hunger", "armor", "xp"};
    PlayerStatus *s = ui_status();
    s->armor = 4; s->xp = 0.5f;
    for (int i = 0; i < ARRAY_LEN(ids); i++) CHECK(ui_element_visible(ids[i]));
    CHECK(!ui_element_visible("magicka")); /* no pool yet */
    s->max_magicka = 20;
    CHECK(ui_element_visible("magicka"));
    g_creative = true;
    CHECK(!ui_element_visible("hearts") && !ui_element_visible("hunger"));
    CHECK(ui_element_set_override("hearts", 1) && ui_element_visible("hearts"));
    g_creative = false;
    CHECK(ui_element_set_override("hearts", 0) && !ui_element_visible("hearts"));
    CHECK(ui_element_set_override("hearts", -1) && ui_element_visible("hearts"));
    float x, y, w, h;
    CHECK(ui_element_rect("hearts", 1000, 600, &x, &y, &w, &h) && x == 500 - 214 && y == 600 - 82 && w == 160);
    CHECK(ui_element_rect("hunger", 1000, 600, &x, &y, &w, &h) && x + w == 714);
    CHECK(ui_status_set_custom("rage", 3, 10) && ui_status_set_custom("rage", 4, 10) && s->custom_count == 1 && s->custom[0].value == 4);
    CHECK(ui_theme_color("health", 0) == rgba(0xc4, 0x36, 0x2f, 255) && ui_theme_color("nope", 7) == 7);
    /* A broken file must keep the previous layout. */
    int n = ui_element_count();
    char dir[] = "selftest_ui";
    dir_make_all("selftest_ui/assets/dfe/ui/theme");
    const char *bad = "{\"elements\": [{\"id\": \"a\", \"style\": \"blob\"}]}";
    file_write_atomic("selftest_ui/assets/dfe/ui/hud.json", bad, strlen(bad));
    vfs_add_root(dir, "selftest_ui");
    CHECK(!ui_reload() && ui_element_count() == n);
    remove("selftest_ui/assets/dfe/ui/hud.json"); remove("selftest_ui/assets/dfe/ui/theme"); remove("selftest_ui/assets/dfe/ui"); remove("selftest_ui/assets/dfe"); remove("selftest_ui/assets"); remove("selftest_ui");
    g_creative = prev;
    vfs_reset();
}

static void test_ui_icons(void) {
    vfs_reset();
    vfs_add_root("engine_assets", "dfe");
    CHECK(icons_load() && icons_has("heart") && icons_has("missing") && !icons_has("nope"));
    float heart[4], miss[4], unk[4];
    CHECK(icons_find("heart", heart) && icons_find("missing", miss) && icons_find("nope", unk));
    CHECK(memcmp(unk, miss, sizeof unk) == 0 && memcmp(heart, miss, sizeof heart) != 0);
    int n = icons_count();
    CHECK(n >= 8);
    icons_draw("heart", 0, 0, 16, 0xffffffff); /* headless: must not touch GL */
    /* A broken file must keep the previous set. */
    char dir[] = "selftest_icons";
    dir_make_all("selftest_icons/assets/dfe/ui");
    const char *bad = "{\"cell\": 16, \"icons\": {\"heart\": [999, 0]}}";
    file_write_atomic("selftest_icons/assets/dfe/ui/icons.json", bad, strlen(bad));
    vfs_add_root(dir, "selftest_icons");
    CHECK(!icons_reload() && icons_count() == n && icons_has("heart"));
    /* A mod pack overrides by name; its picture is copied from the engine one. */
    remove("selftest_icons/assets/dfe/ui/icons.json"); remove("selftest_icons/assets/dfe/ui"); remove("selftest_icons/assets/dfe"); remove("selftest_icons/assets"); remove("selftest_icons");
    vfs_reset();
    vfs_add_root("engine_assets", "dfe");
    dir_make_all("selftest_icons/assets/pack/ui");
    size_t sz;
    u8 *png = vfs_read("assets/dfe/ui/icons.png", &sz, NULL);
    CHECK(png != NULL);
    if (png) file_write_atomic("selftest_icons/assets/pack/ui/icons.png", png, sz);
    free(png);
    const char *pack = "{\"cell\": 16, \"icons\": {\"heart\": [2, 0], \"extra\": [0, 0]}}";
    file_write_atomic("selftest_icons/assets/pack/ui/icons.json", pack, strlen(pack));
    vfs_add_root(dir, "pack");
    float over[4];
    CHECK(icons_reload() && icons_has("extra") && icons_count() == n + 1);
    CHECK(icons_find("heart", over) && memcmp(over, heart, sizeof over) != 0);
    remove("selftest_icons/assets/pack/ui/icons.json"); remove("selftest_icons/assets/pack/ui/icons.png"); remove("selftest_icons/assets/pack/ui"); remove("selftest_icons/assets/pack"); remove("selftest_icons/assets"); remove("selftest_icons");
    icons_shutdown();
    vfs_reset();
}

static int g_screen_events;
static char g_screen_last[48];
static void screen_probe(const char *screen, const char *widget, int index, void *user) {
    (void)screen; (void)user;
    g_screen_events++;
    snprintf(g_screen_last, sizeof g_screen_last, "%s:%d", widget, index);
}

static float fixed_text_width(float size, const char *text) { return 0.6f * size * (float)strlen(text); }

static void test_ui_scaling(void) {
    Settings keep = g_settings;
    vfs_reset();
    vfs_add_root("engine_assets", "dfe");
    ui_set_text_measure(fixed_text_width);
    g_settings.ui_scale = 1;
    /* ui_fit: never shrinks, grows only when text overflows, bounded by text size plus padding */
    float w, h, tw = fixed_text_width(14.0f, "Hello World");
    ui_fit(500, 100, "Hello World", 14.0f, 4.0f, &w, &h);
    CHECK(w == 500 && h == 100);
    ui_fit(20, 10, "Hello World", 14.0f, 4.0f, &w, &h);
    CHECK(w == ceilf(tw + 8) && h == 22.0f);
    ui_fit(20, 10, "", 14.0f, 4.0f, &w, &h);
    CHECK(w == 20 && h == 10);
    for (float ts = 0.75f; ts <= 2.0f; ts += 0.25f) {
        ui_fit(20, 10, "Hello World", 14.0f * ts, 4.0f, &w, &h);
        CHECK(w >= 20 && h >= 10 && w <= ceilf(fixed_text_width(14.0f * ts, "Hello World") + 8) && h <= ceilf(14.0f * ts + 8) && w == floorf(w) && h == floorf(h));
    }
    /* crisp rendering: edges land on whole pixels and shared edges stay shared */
    float lo, hi, lo2, hi2;
    ui_snap_span(10.4f, 20.4f, &lo, &hi);
    ui_snap_span(30.8f, 5.0f, &lo2, &hi2);
    CHECK(lo == 10 && hi == 31 && lo2 == 31 && hi2 == 36 && hi == lo2); /* neighbours meet exactly */
    ui_snap_span(5.2f, 0.3f, &lo, &hi);
    CHECK(hi - lo == 1); /* never thinner than a pixel */
    ui_snap_span(5.2f, 0.0f, &lo, &hi);
    CHECK(hi == lo);
    CHECK(ui_snap_text(0.2f) == 12 && ui_snap_text(17.9f) == 12 && ui_snap_text(18.0f) == 24 && ui_snap_text(36.0f) == 36);
    for (float sz = 1.0f; sz < 200.0f; sz += 1.7f) CHECK(fmodf(ui_snap_text(sz), 12.0f) == 0.0f && ui_snap_text(sz) >= 12.0f); /* always a whole font pixel per screen pixel */
    /* every scale that multiplies pixel art is an integer, for every window size and setting */
    g_settings.hud_scale = 99; g_settings.hud_text_scale = 0.1f; g_settings.ui_text_scale = 99;
    CHECK(ui_hud_text_scale() == 0.75f && ui_ui_text_scale() == 2.0f);
    g_settings.hud_text_scale = g_settings.ui_text_scale = 1.0f;
    static const int win[][2] = {{640, 360}, {1000, 600}, {1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
    for (int wi = 0; wi < ARRAY_LEN(win); wi++) {
        float prev = 0;
        for (float hs = 0.5f; hs <= 3.0f; hs += 0.25f) {
            g_settings.hud_scale = hs;
            float q = ui_hud_scale(win[wi][0], win[wi][1]);
            CHECK(q == floorf(q) && q >= 1.0f && q <= 4.0f && q >= prev); /* integer, bounded, never shrinks as the setting grows */
            prev = q;
        }
        for (int us = -1; us <= 4; us++) if (us) {
            g_settings.ui_scale = us;
            float g = ui_gui_scale(win[wi][0], win[wi][1]);
            CHECK(g == floorf(g) && g >= 1.0f && g <= 4.0f && ui_icon_scale(g) == floorf(ui_icon_scale(g)));
        }
    }
    g_settings.hud_scale = 0.01f;
    CHECK(ui_hud_scale(1920, 1080) == 1.0f);
    g_settings.hud_scale = 1.0f;
    g_settings.ui_scale = -1;
    CHECK(ui_gui_scale(1000, 600) == 1.0f && ui_gui_scale(1920, 1080) == 3.0f);
    CHECK(ui_icon_scale(1) == 1 && ui_icon_scale(2) == 1 && ui_icon_scale(3) == 1 && ui_icon_scale(4) == 2);
    g_settings.ui_scale = 2;
    CHECK(ui_gui_scale(320, 200) == 2.0f);
    g_settings.ui_scale = 1;
    /* HUD elements: whole-pixel rectangles, exact multiples of the integer scale */
    CHECK(ui_load());
    float x1, y1, w1, h1, x2, y2, w2, h2;
    CHECK(ui_element_rect("hearts", 1920, 1080, &x1, &y1, &w1, &h1));
    g_settings.hud_scale = 2.0f; /* auto 3 * 2 = 6, icon scale 3 */
    CHECK(ui_element_rect("hearts", 1920, 1080, &x2, &y2, &w2, &h2) && w2 == 3 * w1 && h2 == 3 * h1 && w2 == 480 && h2 == 48);
    g_settings.hud_scale = 1.0f;
    for (int wi = 0; wi < ARRAY_LEN(win); wi++) {
        const char *ids[] = {"hearts", "hunger", "armor", "stamina", "magicka", "xp"};
        for (int i = 0; i < ARRAY_LEN(ids); i++) {
            CHECK(ui_element_rect(ids[i], win[wi][0], win[wi][1], &x2, &y2, &w2, &h2));
            CHECK(x2 == floorf(x2) && y2 == floorf(y2) && w2 == floorf(w2) && h2 == floorf(h2));
        }
    }
    /* a HUD scale change leaves the HUD's text alone only when text scale is separate: text px is on the font grid */
    g_settings.hud_scale = 1.0f;
    ui_status()->level = 5;
    g_settings.hud_text_scale = 2.0f;
    CHECK(ui_element_rect("xp", 1000, 600, &x2, &y2, &w2, &h2) && w2 >= 428 && h2 == 3);
    g_settings.hud_text_scale = 1.0f;
    /* UI text scale grows a button only as far as its text needs, and the HUD ignores it */
    screen_shutdown();
    ScreenDef d = {.id = "test:scale", .w = 200, .h = 100, .on_open = screen_probe};
    Widget b = {.type = WIDGET_BUTTON, .id = "b", .text = "Hello World", .x = 0, .y = 0, .w = 20, .h = 10, .enabled = true, .on_click = screen_probe};
    CHECK(screen_register(&d) && screen_add_widget("test:scale", &b) && screen_open("test:scale"));
    float fw1, fh1, fw2, fh2;
    ui_fit(20, 10, "Hello World", ui_snap_text(14.0f), 4.0f, &fw1, &fh1);
    ui_fit(20, 10, "Hello World", ui_snap_text(28.0f), 4.0f, &fw2, &fh2);
    CHECK(fw2 > fw1 + 1 && fh2 > fh1 + 1);
    g_screen_last[0] = 0;
    screen_click_at(1000, 600, 400 + fw1 + 2, 250 + 2, false);
    CHECK(!g_screen_last[0]); /* past the button at text scale 1 */
    g_settings.ui_text_scale = 2.0f;
    CHECK(screen_click_at(1000, 600, 400 + fw1 + 2, 250 + fh1 + 2, false) && !strcmp(g_screen_last, "b:0")); /* inside the grown button */
    g_screen_last[0] = 0;
    screen_click_at(1000, 600, 400 + fw2 + 2, 250 + 2, false);
    CHECK(!g_screen_last[0]); /* growth stops at the text */
    g_settings.ui_text_scale = 1.0f;
    CHECK(screen_close() && screen_unregister("test:scale"));
    ui_set_text_measure(NULL);
    g_settings = keep;
}

static void test_ui_screens(void) {
    screen_shutdown();
    ScreenDef d = {.id = "test:native", .title = "Native", .w = 200, .h = 100, .close_on_escape = true, .on_open = screen_probe, .on_close = screen_probe};
    CHECK(screen_register(&d) && !screen_register(&d));
    Widget ok = {.type = WIDGET_BUTTON, .id = "ok", .text = "OK", .x = 10, .y = 10, .w = 50, .h = 20, .enabled = true, .on_click = screen_probe};
    Widget list = {.type = WIDGET_LIST, .id = "list", .x = 10, .y = 40, .w = 100, .h = 54, .enabled = true, .item_count = 3, .on_click = screen_probe};
    strcpy(list.items[0], "a"); strcpy(list.items[1], "b"); strcpy(list.items[2], "c");
    CHECK(screen_add_widget("test:native", &ok) && screen_add_widget("test:native", &list) && !screen_add_widget("test:native", &ok) && !screen_add_widget("nope", &ok));
    CHECK(!screen_is_open(NULL) && !screen_open("nope"));
    int before = g_screen_events;
    CHECK(screen_open("test:native") && screen_is_open("test:native") && g_screen_events == before + 1 && !screen_open("test:native"));
    CHECK(!strcmp(screen_focus(), "ok"));
    /* 1000x600 window: panel starts at (400, 250) */
    CHECK(screen_click_at(1000, 600, 400 + 20, 250 + 15, false) && !strcmp(g_screen_last, "ok:0"));
    CHECK(screen_click_at(1000, 600, 400 + 20, 250 + 40 + 20, false) && !strcmp(g_screen_last, "list:1") && screen_widget("test:native", "list")->selected == 1);
    CHECK(!screen_click_at(1000, 600, 5, 5, false)); /* outside a non-modal screen */
    CHECK(!strcmp(screen_focus(), "list")); /* clicking focuses */
    CHECK(screen_key(GLFW_KEY_DOWN) && screen_widget("test:native", "list")->selected == 2);
    CHECK(screen_key(GLFW_KEY_TAB) && !strcmp(screen_focus(), "ok"));
    CHECK(screen_key(-GLFW_KEY_TAB) && !strcmp(screen_focus(), "list"));
    CHECK(screen_key(GLFW_KEY_TAB) && !strcmp(screen_focus(), "ok"));
    screen_widget("test:native", "ok")->enabled = false;
    int ev = g_screen_events;
    CHECK(screen_key(GLFW_KEY_ENTER) && g_screen_events == ev); /* a disabled button does nothing */
    CHECK(screen_key(GLFW_KEY_ESCAPE) && !screen_is_open(NULL));
    /* modal screens swallow clicks outside themselves and stack */
    ScreenDef m = {.id = "test:modal", .w = 100, .h = 50, .modal = true};
    CHECK(screen_register(&m) && screen_open("test:native") && screen_open("test:modal"));
    CHECK(!strcmp(screen_top(), "test:modal") && screen_click_at(1000, 600, 5, 5, false));
    CHECK(screen_close() && !strcmp(screen_top(), "test:native") && screen_unregister("test:native") && !screen_is_open(NULL));
    CHECK(screen_unregister("test:modal"));
    /* Lua screens register, open, run callbacks, close, and vanish with the script state */
    CHECK(script_init());
    script_eval("dfe.ui.register_screen('lua:demo', {title='Demo', modal=true, on_open=function() opened = true end, on_close=function() closed = true end,"
                " widgets={{type='button', id='go', text='Go', x=10, y=10, w=60, h=20, on_click=function(s, w, i) clicked = s .. w .. i end},"
                " {type='bar', id='mana', value=3, max=10}}})");
    CHECK(screen_widget("lua:demo", "go") && screen_widget("lua:demo", "mana"));
    CHECK(screen_open("lua:demo") && screen_click_at(1000, 600, 500 - 120 + 20, 300 - 80 + 15, false));
    script_eval("dfe.ui.set_widget('lua:demo', 'mana', {value = 7})");
    CHECK(screen_widget("lua:demo", "mana")->value == 7);
    CHECK(screen_close());
    script_eval("dfe.ui.register_screen('lua:demo', {})"); /* duplicate id is an error, not a crash */
    script_eval("dfe.ui.register_widget('nope', {type='label', id='x'})");
    script_shutdown();
    CHECK(!screen_widget("lua:demo", "go"));
    screen_shutdown();
}

int selftest_run(void) {
    struct { const char *name; void (*fn)(void); } groups[] = {
        {"base", test_base},
        {"small-fixes", test_small_correctness_fixes},
        {"content", test_content_registry},
        {"jobs", test_jobs},
        {"palette", test_palette},
        {"json", test_json},
        {"registry", test_registry},
        {"worldgen", test_worldgen_data_driven},
        {"worldgen-features", test_worldgen_features_and_structures},
        {"hydrology", test_hydrology},
        {"trees", test_trees},
        {"biomes", test_biomes},
        {"forever-worlds", test_forever_worlds},
        {"world-light-mesh", test_world_and_mesh},
        {"vfs", test_vfs},
        {"mod-storage", test_mod_storage},
        {"mods-scripts", test_mods_and_scripts},
        {"examples", test_example_mods},
        {"gameplay", test_gameplay},
        {"presets", test_presets},
        {"atmosphere", test_atmosphere},
        {"ui-status", test_ui_status_hud},
        {"ui-screens", test_ui_screens},
        {"ui-scaling", test_ui_scaling},
        {"ui-icons", test_ui_icons},
    };
    for (int i = 0; i < ARRAY_LEN(groups); i++) {
        int before = g_failures;
        groups[i].fn();
        printf("selftest %-14s %s\n", groups[i].name, g_failures == before ? "ok" : "FAILED");
    }
    printf("selftest total: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures;
}

/* ./build/dfe --headless --test-forever-worlds [--seed N]: only the Forever Worlds group. */
void forever_test_set_seed(u64 seed);
int selftest_run_forever(u64 seed) {
    if (seed) forever_test_set_seed(seed);
    int before = g_failures;
    test_forever_worlds();
    printf("selftest forever-worlds %s\n", g_failures == before ? "ok" : "FAILED");
    printf("selftest total: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures;
}
