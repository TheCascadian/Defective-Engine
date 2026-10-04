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
int modtool_run(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: dfe mod validate|test|package PATH [-o OUTPUT]\n"); return 2; }
    const char *op = argv[2], *root = argv[3]; int rc = mod_validate(root);
    if (rc || !strcmp(op, "validate")) return rc;
    if (!strcmp(op, "test")) { puts("mod tests passed (schema and registry validation)"); return 0; }
    if (!strcmp(op, "package")) {
        const char *out = argc >= 6 && !strcmp(argv[4], "-o") ? argv[5] : "mod.dfe.zip";
        char cmd[1800]; snprintf(cmd, sizeof cmd, "cd \"%s\" && zip -q -r \"%s\" . -x '*.git*'", root, out);
        rc = system(cmd); if (!rc) puts(out); else fprintf(stderr, "error: package creation failed\n"); return rc ? 1 : 0;
    }
    fprintf(stderr, "unknown mod command: %s\n", op); return 2;
}
