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
    for (int k = 0; k < 40 && !open_sky_spot(x, z); k++) { x = 8 + 5 * (k % 6); z = 8 + 5 * (k / 6); }
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
    test_save_budget();
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
    CHECK(mods_loaded_count() == 8);
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
    /* A wall stops a walking player and a one block ledge is stepped over only when 0.6 or lower. */
    for (int y = SLAB_Y + 1; y < SLAB_Y + 4; y++) world_set_state(12, y, 8, stone);
    PlayerInput fwd = {.strafe = 1.0f};
    p.yaw = 0;
    for (int i = 0; i < 240; i++) player_step(&p, &fwd, 1.0f / 60.0f);
    CHECK(p.pos.x < 12.0f - PLAYER_WIDTH * 0.5f + 0.01f && p.pos.x > 10.0f);
    for (int y = SLAB_Y + 1; y < SLAB_Y + 4; y++) world_set_state(12, y, 8, STATE_AIR);
    CHECK(!player_box_blocked(v3(8.5f, (float)(SLAB_Y + 1) + 0.01f, 8.5f)));
    CHECK(player_box_blocked(v3(8.5f, (float)SLAB_Y + 0.5f, 8.5f)));
}

static void test_entities(void) {
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
    a.slot[20].state = dirt; a.slot[20].count = 64;
    a.selected = 4;
    static SaveMeta m;
    memset(&m, 0, sizeof m);
    inventory_store(&a, false, &m);
    bool creative = true;
    inventory_restore(&b, &creative, &m);
    CHECK(!creative && b.selected == 4 && b.slot[2].state == stone && b.slot[2].count == 33 && b.slot[20].state == dirt && b.slot[20].count == 64);
    snprintf(m.inv_name[2], SAVE_BLOCK_NAME_LEN, "gone:missing_block"); /* a removed mod's item is dropped, not crashed on */
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

    /* The preset supplies the values; a setting overrides only its own field; an unknown preset falls back. */
    bool no_render = g_opt.no_render;
    g_opt.no_render = false;
    settings_defaults();
    snprintf(g_settings.preset, sizeof g_settings.preset, "high");
    gfx_apply();
    CHECK(g_gfx.render_distance == high->render_distance && g_gfx.light_shafts);
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
    test_entities();
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

int selftest_run(void) {
    struct { const char *name; void (*fn)(void); } groups[] = {
        {"base", test_base},
        {"jobs", test_jobs},
        {"palette", test_palette},
        {"json", test_json},
        {"registry", test_registry},
        {"world-light-mesh", test_world_and_mesh},
        {"vfs", test_vfs},
        {"mods-scripts", test_mods_and_scripts},
        {"examples", test_example_mods},
        {"gameplay", test_gameplay},
        {"presets", test_presets},
        {"atmosphere", test_atmosphere},
    };
    for (int i = 0; i < ARRAY_LEN(groups); i++) {
        int before = g_failures;
        groups[i].fn();
        printf("selftest %-14s %s\n", groups[i].name, g_failures == before ? "ok" : "FAILED");
    }
    printf("selftest total: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures;
}
