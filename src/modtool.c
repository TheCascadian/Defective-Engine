#include "dfe.h"
#include <ctype.h>

static bool mod_id(const char *s) {
    if (!s || !s[0] || !islower((unsigned char)s[0])) return false;
    for (const char *p = s + 1; *p; p++) if (!islower((unsigned char)*p) && !isdigit((unsigned char)*p) && *p != '_') return false;
    return true;
}
static int validate_json_tree(const char *dir, const char *folder, const char *ns, StrMap *seen) {
    if (!path_is_dir(dir)) return 0;
    StrList files = {0}; dir_list(dir, &files); int errors = 0;
    for (int i = 0; i < files.n; i++) {
        size_t n = strlen(files.d[i]); if (n < 6 || strcmp(files.d[i] + n - 5, ".json")) continue;
        char path[1200]; snprintf(path, sizeof path, "%s/%s", dir, files.d[i]);
        size_t len = 0; u8 *raw = file_read(path, &len); char err[200] = {0}; int line = 0;
        Json *j = raw ? json_parse((const char *)raw, len, err, sizeof err, &line) : NULL;
        if (raw) free(raw);
        if (!j) { fprintf(stderr, "error: %s:%d: %s\n", path, line, err[0] ? err : "could not read JSON file"); errors++; continue; }
        char stem[128]; snprintf(stem, sizeof stem, "%.*s", (int)(n - 5), files.d[i]);
        char id[320]; snprintf(id, sizeof id, "%s:%s:%s", folder, ns, stem); u32 old;
        if (strmap_get(seen, id, &old)) { fprintf(stderr, "error: %s: duplicate id %s\n", path, id); errors++; }
        else strmap_set(seen, id, 1);
        json_free(j);
    }
    strlist_free(&files); return errors;
}

static int validate_data_tree(const char *root, StrMap *seen) {
    static const char *domains[] = {"blocks", "items", "entities", "biomes", "worldgen", "structures", "recipes", "loot_tables", "sounds", "effects"};
    char data[700]; snprintf(data, sizeof data, "%s/data", root);
    if (!path_is_dir(data)) return 0;
    StrList namespaces = {0}; dir_list(data, &namespaces); int errors = 0;
    for (int i = 0; i < namespaces.n; i++) {
        char ns_path[900]; snprintf(ns_path, sizeof ns_path, "%s/%s", data, namespaces.d[i]);
        if (!path_is_dir(ns_path)) continue;
        for (int d = 0; d < ARRAY_LEN(domains); d++) {
            char dir[1100]; snprintf(dir, sizeof dir, "%s/%s", ns_path, domains[d]);
            errors += validate_json_tree(dir, domains[d], namespaces.d[i], seen);
        }
        char tags[1000]; snprintf(tags, sizeof tags, "%s/tags", ns_path);
        StrList registries = {0}; dir_list(tags, &registries);
        for (int r = 0; r < registries.n; r++) {
            char dir[1200], kind[128]; snprintf(dir, sizeof dir, "%s/%s", tags, registries.d[r]);
            snprintf(kind, sizeof kind, "tags/%s", registries.d[r]);
            errors += validate_json_tree(dir, kind, namespaces.d[i], seen);
        }
        strlist_free(&registries);
    }
    strlist_free(&namespaces);
    return errors;
}
static int mod_validate(const char *root) {
    char path[700]; snprintf(path, sizeof path, "%s/mod.json", root);
    size_t len = 0; u8 *raw = file_read(path, &len); char err[200]; int line = 0;
    Json *m = raw ? json_parse((const char *)raw, len, err, sizeof err, &line) : NULL; if (raw) free(raw);
    if (!m) { fprintf(stderr, "error: %s: invalid or missing manifest (%s)\n", path, err); return 1; }
    int errors = 0; const char *id = json_str(m, "id", "");
    if (!mod_id(id)) { fprintf(stderr, "error: mod.json: id must be lowercase namespace text\n"); errors++; }
    if (json_int(m, "schema", DFE_MOD_SCHEMA_VERSION) != DFE_MOD_SCHEMA_VERSION) { fprintf(stderr, "error: mod.json: incompatible schema\n"); errors++; }
    if (!json_str(m, "version", "")[0]) { fprintf(stderr, "error: mod.json: version is required\n"); errors++; }
    StrMap seen; strmap_init(&seen);
    const char *folders[] = {"blocks","items","entities","biomes","worldgen","structures","recipes","loot_tables","sounds","effects"};
    for (int i = 0; i < ARRAY_LEN(folders); i++) {
        char dir[700]; snprintf(dir, sizeof dir, "%s/%s", root, folders[i]);
        errors += validate_json_tree(dir, folders[i], id, &seen);
    }
    char tags[700]; snprintf(tags, sizeof tags, "%s/tags", root);
    StrList registries = {0}; dir_list(tags, &registries);
    for (int r = 0; r < registries.n; r++) {
        char dir[900], kind[128]; snprintf(dir, sizeof dir, "%s/%s", tags, registries.d[r]);
        snprintf(kind, sizeof kind, "tags/%s", registries.d[r]);
        errors += validate_json_tree(dir, kind, id, &seen);
    }
    strlist_free(&registries);
    errors += validate_data_tree(root, &seen);
    strmap_free(&seen); json_free(m); if (!errors) printf("valid mod: %s\n", root); return errors ? 1 : 0;
}
static int test_option_index(int argc, char **argv) {
    return argc > 2 && !strcmp(argv[1], "--headless") ? 3 : 2;
}

static bool test_assets(char *out, size_t cap) {
    char exe[512]; path_exe_dir(exe, sizeof exe);
    const char *formats[] = {"%s/engine_assets", "%s/../engine_assets", "engine_assets"};
    for (int i = 0; i < ARRAY_LEN(formats); i++) {
        snprintf(out, cap, formats[i], exe);
        if (path_is_dir(out)) return true;
    }
    return false;
}

static int mod_test_run(int argc, char **argv, int op_i, const char *root) {
    int ticks = 100; u64 seed = 1; double timeout = 0; bool seed_set = false, json = false, verbose = false, allow_native = false, no_cleanup = false;
    const char *world = NULL;
    for (int i = op_i + 2; i < argc; i++) {
        if (!strcmp(argv[i], "--ticks") && i + 1 < argc) ticks = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) { seed = strtoull(argv[++i], NULL, 10); seed_set = true; }
        else if (!strcmp(argv[i], "--world") && i + 1 < argc) world = argv[++i];
        else if (!strcmp(argv[i], "--json")) json = true;
        else if (!strcmp(argv[i], "--verbose")) verbose = true;
        else if (!strcmp(argv[i], "--allow-native")) allow_native = true;
        else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) timeout = atof(argv[++i]);
        else if (!strcmp(argv[i], "--no-cleanup")) no_cleanup = true;
        else { fprintf(stderr, "unknown mod test option: %s\n", argv[i]); return 2; }
    }
    if (ticks < 0) ticks = 0;
    char parent[1200], *slash;
    snprintf(parent, sizeof parent, "%s", root);
    slash = strrchr(parent, '/');
    if (!slash) { snprintf(parent, sizeof parent, "."); }
    else if (slash == parent) slash[1] = '\0'; else *slash = '\0';
    char assets[700];
    if (!test_assets(assets, sizeof assets)) { fprintf(stderr, "error: engine_assets folder not found\n"); return 1; }
    vfs_reset(); vfs_add_root(assets, "dfe"); data_error_reset(); mods_reset();
    if (!mods_discover(parent) || !mods_resolve()) { fprintf(stderr, "error: no loadable mods (including base)\n"); return 1; }
    mods_mount();
    const ModInfo *target = NULL;
    for (int i = 0; i < mods_total(); i++) if (mods_at(i) && !strcmp(mods_at(i)->dir, root)) target = mods_at(i);
    if (!target) { fprintf(stderr, "error: target is not a discovered mod directory: %s\n", root); return 1; }
    if (target->plugin[0] && !allow_native) { fprintf(stderr, "error: mod '%s' requires native plugin '%s'; pass --allow-native\n", target->id, target->plugin); return 1; }
    if (data_error_count()) { fprintf(stderr, "error: mod load failed: %s\n", data_error_text(0)); return 1; }
    if (verbose) printf("load order:");
    for (int i = 0; i < mods_loaded_count(); i++) { if (verbose) printf(" %s", mods_loaded_at(i)->id); }
    if (verbose) putchar('\n');
    registry_reset(); registry_load_blocks(); content_load_all(); registry_load_worldgen_config(); registry_load_atmosphere(); registry_load_presets(); registry_load_entities();
    if (data_error_count()) { fprintf(stderr, "error: content load failed: %s\n", data_error_text(0)); return 1; }
    jobs_init(1); mods_load_plugins(allow_native);
    if (!script_init() || script_load_mods() || data_error_count()) { fprintf(stderr, "error: Lua initialization failed\n"); script_shutdown(); jobs_shutdown(); return 1; }
    char world_dir[1000];
    if (world) snprintf(world_dir, sizeof world_dir, "%s", world);
    else snprintf(world_dir, sizeof world_dir, "/tmp/dfe-mod-test-world-%s", target->id);
    if (!world && !no_cleanup) dir_remove_all(world_dir);
    if (!save_open(world_dir, seed_set ? seed : 1)) { fprintf(stderr, "error: could not create test world %s\n", world_dir); script_shutdown(); jobs_shutdown(); return 1; }
    u64 run_seed = save_seed();
    world_init(run_seed); entity_world_init(run_seed); server_init();
    { dfe_event_t ev = {.name = "world_load"}; event_fire(&ev); }
    double started = time_now_s(); bool timed_out = false;
    for (int i = 0; i < ticks; i++) {
        if (timeout > 0 && time_now_s() - started >= timeout) { timed_out = true; break; }
        game_tick();
    }
    StrList tests = {0}; char test_dir[1200]; snprintf(test_dir, sizeof test_dir, "%s/tests", root); dir_list(test_dir, &tests);
    int passed = 0, failed = timed_out ? 1 : 0;
    for (int i = 0; i < tests.n && !timed_out; i++) {
        size_t n = strlen(tests.d[i]);
        if (n < 5 || strcmp(tests.d[i] + n - 4, ".lua")) continue;
        char rel[300]; snprintf(rel, sizeof rel, "tests/%s", tests.d[i]);
        bool ok = script_run_mod_test(target->id, rel);
        if (ok) passed++; else failed++;
        if (!json) printf("  [%s] %s\n", ok ? "PASS" : "FAIL", tests.d[i]);
        if (timeout > 0 && time_now_s() - started >= timeout) timed_out = true;
    }
    if (timed_out) failed = MAX(failed, 1);
    strlist_free(&tests);
    { dfe_event_t ev = {.name = "world_unload"}; event_fire(&ev); }
    server_shutdown(); world_shutdown(); save_close(); script_shutdown(); events_clear_all(); jobs_shutdown(); mods_unload_plugins();
    if (!world && !no_cleanup) dir_remove_all(world_dir);
    if (timed_out && !json) fprintf(stderr, "error: mod test timed out after %.2f seconds\n", timeout);
    if (json) printf("{\"mod\":\"%s\",\"seed\":%llu,\"ticks\":%d,\"passed\":%d,\"failed\":%d,\"result\":\"%s\"}\n", target->id, (unsigned long long)run_seed, ticks, passed, failed, failed ? "fail" : "pass");
    else printf("Result: %d passed, %d failed (%d ticks)\n", passed, failed, ticks);
    return failed ? 1 : 0;
}

int modtool_run(int argc, char **argv) {
    int oi = test_option_index(argc, argv);
    if (argc < oi + 2) { fprintf(stderr, "usage: dfe [--headless] mod validate|test|package PATH [-o OUTPUT]\n"); return 2; }
    const char *op = argv[oi], *root = argv[oi + 1];
    if (!strcmp(op, "test")) return mod_test_run(argc, argv, oi, root);
    int rc = mod_validate(root);
    if (rc || !strcmp(op, "validate")) return rc;
    if (!strcmp(op, "package")) {
        const char *out = argc >= oi + 4 && (!strcmp(argv[oi + 2], "-o") || !strcmp(argv[oi + 2], "--output")) ? argv[oi + 3] : "mod.dfe.zip";
        char cmd[1800]; snprintf(cmd, sizeof cmd, "cd \"%s\" && zip -q -r \"%s\" . -x '*.git*'", root, out);
        rc = system(cmd); if (!rc) puts(out); else fprintf(stderr, "error: package creation failed\n"); return rc ? 1 : 0;
    }
    fprintf(stderr, "unknown mod command: %s\n", op); return 2;
}
