/* Self-test driver. Each module contributes a group of checks through a function
 * declared here; failures are counted and reported with file and line. */
#include "dfe.h"

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

    int x = 8, z = 8;
    int ground = ifloor(gen_height_at((float)x, (float)z));
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

/* Edits survive a save, unload and reload, and untouched columns are not written at all. */
static void remove_tree_files(const char *dir) {
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
    remove_tree_files(dir);
    jobs_init(2);
    CHECK(save_open(dir, 99));
    CHECK(save_seed() == 99);
    world_init(save_seed());
    world_flush_generation(0, 0, 2);
    int ground = ifloor(gen_height_at(5, 5));
    int ey = ground + 10;
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
    CHECK(save_seed() == 99); /* an existing world keeps its own seed */
    world_init(save_seed());
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

static void test_gen_determinism(void) {
    registry_reset();
    data_error_reset();
    registry_load_blocks();
    registry_load_worldgen_config();
    if (data_error_count()) { CHECK(false); return; }
    gen_init(777);
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

static void test_world_and_mesh(void) {
    test_world_light();
    test_save_roundtrip();
    test_gen_determinism();
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

int selftest_run(void) {
    struct { const char *name; void (*fn)(void); } groups[] = {
        {"base", test_base},
        {"jobs", test_jobs},
        {"palette", test_palette},
        {"json", test_json},
        {"registry", test_registry},
        {"world-light-mesh", test_world_and_mesh},
        {"vfs", test_vfs},
    };
    for (int i = 0; i < ARRAY_LEN(groups); i++) {
        int before = g_failures;
        groups[i].fn();
        printf("selftest %-14s %s\n", groups[i].name, g_failures == before ? "ok" : "FAILED");
    }
    printf("selftest total: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures;
}
