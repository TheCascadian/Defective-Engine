/* Forever Worlds core: generation epoch registry (GER), chunk generation metadata (CGM), blend plans and the seam
 * min-cut.  Pure C with no GL or world dependency; docs/FOREVER_WORLDS.md describes the design. */
#include "epoch.h"

/* --------------------------------------------------------------- SHA-256 */

static const u32 K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

typedef struct Sha256 { u32 h[8]; u8 buf[64]; u64 total; size_t fill; } Sha256;

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(Sha256 *s, const u8 *p) {
    u32 w[64];
    for (int i = 0; i < 16; i++) w[i] = (u32)p[4 * i] << 24 | (u32)p[4 * i + 1] << 16 | (u32)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        u32 s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        u32 t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        u32 t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha_init(Sha256 *s) {
    static const u32 iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, iv, sizeof iv);
    s->total = 0;
    s->fill = 0;
}

static void sha_update(Sha256 *s, const void *data, size_t len) {
    const u8 *p = data;
    s->total += len;
    while (len) {
        size_t n = MIN(len, 64 - s->fill);
        memcpy(s->buf + s->fill, p, n);
        s->fill += n; p += n; len -= n;
        if (s->fill == 64) { sha_block(s, s->buf); s->fill = 0; }
    }
}

static void sha_final(Sha256 *s, u8 out[32]) {
    u64 bits = s->total * 8;
    u8 pad = 0x80;
    sha_update(s, &pad, 1);
    u8 zero = 0;
    while (s->fill != 56) sha_update(s, &zero, 1);
    u8 len[8];
    for (int i = 0; i < 8; i++) len[i] = (u8)(bits >> (56 - 8 * i));
    sha_update(s, len, 8);
    for (int i = 0; i < 8; i++) { out[4 * i] = (u8)(s->h[i] >> 24); out[4 * i + 1] = (u8)(s->h[i] >> 16); out[4 * i + 2] = (u8)(s->h[i] >> 8); out[4 * i + 3] = (u8)s->h[i]; }
}

void sha256_bytes(const void *data, size_t len, u8 out[32]) {
    Sha256 s;
    sha_init(&s);
    sha_update(&s, data, len);
    sha_final(&s, out);
}

void epoch_hex(const u8 *h, size_t n, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = hex[h[i] >> 4]; out[2 * i + 1] = hex[h[i] & 15]; }
    out[2 * n] = 0;
}

/* -------------------------------------------------------------- registry */

/* Sorted by byte order, which is the order the hash covers. */
static const char *const EPOCH_FILES[6] = {"biomes.json", "caves.json", "features.json", "noise.json", "structures.json", "surface.json"};

static Epoch g_epochs[EP_MAX_EPOCHS];
static int g_epoch_count;
static u64 g_generation = 1;
static float g_radius = EP_RADIUS_DEFAULT;

void epoch_params_default(EpochParams *p) {
    memset(p, 0, sizeof *p);
    p->sea_level = 62.0f;
    p->height_scale = 1.0f;
    p->tunnel_width_scale = 1.0f;
    p->tree_scale = p->plant_scale = p->ore_scale = p->structure_scale = 1.0f;
    p->identity = true;
}

int epoch_count(void) { return g_epoch_count; }
const Epoch *epoch_get(u32 id) { return id < (u32)g_epoch_count ? &g_epochs[id] : NULL; }
u32 epoch_current(void) { return g_epoch_count ? (u32)g_epoch_count - 1 : 0; }
u64 epoch_generation(void) { return g_generation; }
void epoch_invalidate(void) { g_generation++; }
float epoch_blend_radius(void) { return g_radius; }
void epoch_set_blend_radius(float r) {
    float v = CLAMP(r, EP_RADIUS_MIN, EP_RADIUS_MAX);
    if (v != g_radius) { g_radius = v; g_generation++; }
}

void epoch_registry_clear(void) {
    memset(g_epochs, 0, sizeof g_epochs);
    g_epoch_count = 0;
    g_generation++;
}

typedef struct Src { const char *root; } Src;

static u8 *src_read(const Src *s, u32 id, const char *name, size_t *len) {
    if (s->root) {
        char path[700];
        snprintf(path, sizeof path, "%s/%u/%s", s->root, id, name);
        return file_read(path, len);
    }
    char rel[200];
    snprintf(rel, sizeof rel, "data/dfe/epochs/%u/%s", id, name);
    const char *owner;
    return vfs_read(rel, len, &owner);
}

static void src_list(const Src *s, u32 id, StrList *out) {
    char path[700];
    if (s->root) { snprintf(path, sizeof path, "%s/%u", s->root, id); dir_list(path, out); }
    else { snprintf(path, sizeof path, "data/dfe/epochs/%u", id); vfs_list(path, out); }
}

static void src_list_ids(const Src *s, StrList *out) {
    if (s->root) dir_list(s->root, out);
    else vfs_list("data/dfe/epochs", out);
}

#define FAIL(...) do { snprintf(err, err_cap, __VA_ARGS__); return false; } while (0)

static bool hash_epoch(const Src *src, u32 id, u8 out[32], char *err, size_t err_cap) {
    Sha256 sh;
    sha_init(&sh);
    for (int i = 0; i < 6; i++) {
        size_t len = 0;
        u8 *text = src_read(src, id, EPOCH_FILES[i], &len);
        if (!text) FAIL("epoch %u is missing %s. Every epoch directory needs biomes, caves, features, noise, structures and surface JSON plus hash.txt.", id, EPOCH_FILES[i]);
        sha_update(&sh, EPOCH_FILES[i], strlen(EPOCH_FILES[i]) + 1);
        u8 l[8];
        for (int k = 0; k < 8; k++) l[k] = (u8)((u64)len >> (8 * k));
        sha_update(&sh, l, 8);
        sha_update(&sh, text, len);
        free(text);
    }
    sha_final(&sh, out);
    return true;
}

bool epoch_hash_dir(const char *dir, u8 out[32], char *err, size_t err_cap) {
    /* `dir` is one epoch directory; reuse the loader's layout by treating its parent as the root. */
    char parent[600];
    snprintf(parent, sizeof parent, "%s", dir);
    char *slash = strrchr(parent, '/');
    if (!slash) FAIL("epoch directory '%s' must be <root>/<id>.", dir);
    *slash = 0;
    u32 id = (u32)strtoul(slash + 1, NULL, 10);
    Src s = {parent};
    return hash_epoch(&s, id, out, err, err_cap);
}

static bool num_field(const Json *o, const char *key, float lo, float hi, float *dst, u32 id, const char *file, char *err, size_t err_cap) {
    const Json *v = json_get(o, key);
    if (!v) return true;
    if (v->type != JSON_NUMBER || v->num != v->num) FAIL("epoch %u %s: \"%s\" must be a number.", id, file, key);
    if (v->num < lo || v->num > hi) FAIL("epoch %u %s: \"%s\" is %g, outside the allowed range %g to %g.", id, file, key, v->num, lo, hi);
    *dst = (float)v->num;
    return true;
}

static bool known_keys(const Json *o, const char *const *keys, int n, u32 id, const char *file, char *err, size_t err_cap) {
    for (int i = 0; i < o->count; i++) {
        bool ok = false;
        for (int k = 0; k < n; k++) if (!strcmp(o->keys[i], keys[k])) ok = true;
        if (!ok) FAIL("epoch %u %s: unknown key \"%s\".", id, file, o->keys[i]);
    }
    return true;
}

static Json *parse_file(const Src *src, u32 id, const char *file, char *err, size_t err_cap) {
    size_t len = 0;
    u8 *text = src_read(src, id, file, &len);
    if (!text) { snprintf(err, err_cap, "epoch %u is missing %s.", id, file); return NULL; }
    char perr[120];
    int line = 0;
    Json *j = json_parse((const char *)text, len, perr, sizeof perr, &line);
    free(text);
    if (!j) { snprintf(err, err_cap, "epoch %u %s line %d: %s", id, file, line, perr); return NULL; }
    if (j->type != JSON_OBJECT) { snprintf(err, err_cap, "epoch %u %s must be a JSON object.", id, file); json_free(j); return NULL; }
    return j;
}

static bool parse_params(const Src *src, u32 id, EpochParams *p, char *err, size_t err_cap) {
    epoch_params_default(p);
    Json *j;
    static const char *const nk[] = {"kernel", "description", "sea_level", "height_scale", "height_offset", "continentalness_bias", "erosion_bias", "weirdness_bias"};
    if (!(j = parse_file(src, id, "noise.json", err, err_cap))) return false;
    bool ok = known_keys(j, nk, 8, id, "noise.json", err, err_cap);
    if (ok && strcmp(json_str(j, "kernel", ""), "terrain_v1")) { snprintf(err, err_cap, "epoch %u noise.json: \"kernel\" must be \"terrain_v1\", the only terrain kernel this engine has.", id); ok = false; }
    ok = ok && num_field(j, "sea_level", 0, 255, &p->sea_level, id, "noise.json", err, err_cap)
            && num_field(j, "height_scale", 0.25f, 4.0f, &p->height_scale, id, "noise.json", err, err_cap)
            && num_field(j, "height_offset", -64, 64, &p->height_offset, id, "noise.json", err, err_cap)
            && num_field(j, "continentalness_bias", -0.5f, 0.5f, &p->cont_bias, id, "noise.json", err, err_cap)
            && num_field(j, "erosion_bias", -0.9f, 0.9f, &p->erosion_bias, id, "noise.json", err, err_cap)
            && num_field(j, "weirdness_bias", -0.9f, 2.0f, &p->weirdness_bias, id, "noise.json", err, err_cap);
    json_free(j);
    if (!ok) return false;

    static const char *const bk[] = {"description", "kernel", "temperature_bias", "humidity_bias"};
    float kernel_f = 0.0f;
    if (!(j = parse_file(src, id, "biomes.json", err, err_cap))) return false;
    ok = known_keys(j, bk, 4, id, "biomes.json", err, err_cap)
      && num_field(j, "kernel", 0, 1, &kernel_f, id, "biomes.json", err, err_cap)
      && num_field(j, "temperature_bias", -0.5f, 0.5f, &p->temp_bias, id, "biomes.json", err, err_cap)
      && num_field(j, "humidity_bias", -0.5f, 0.5f, &p->humid_bias, id, "biomes.json", err, err_cap);
    json_free(j);
    if (!ok) return false;
    if (kernel_f != (float)(int)kernel_f) FAIL("epoch %u biomes.json: \"kernel\" must be the integer 0 or 1.", id);
    p->kernel = (int)kernel_f;

    static const char *const ck[] = {"description", "tunnel_width_scale", "cheese_threshold_delta"};
    if (!(j = parse_file(src, id, "caves.json", err, err_cap))) return false;
    ok = known_keys(j, ck, 3, id, "caves.json", err, err_cap)
      && num_field(j, "tunnel_width_scale", 0, 4, &p->tunnel_width_scale, id, "caves.json", err, err_cap)
      && num_field(j, "cheese_threshold_delta", -0.3f, 0.3f, &p->cheese_threshold_delta, id, "caves.json", err, err_cap);
    json_free(j);
    if (!ok) return false;

    static const char *const sk[] = {"description", "snowline_offset", "treeline_offset"};
    if (!(j = parse_file(src, id, "surface.json", err, err_cap))) return false;
    ok = known_keys(j, sk, 3, id, "surface.json", err, err_cap)
      && num_field(j, "snowline_offset", -64, 64, &p->snowline_offset, id, "surface.json", err, err_cap)
      && num_field(j, "treeline_offset", -64, 64, &p->treeline_offset, id, "surface.json", err, err_cap);
    json_free(j);
    if (!ok) return false;

    static const char *const fk[] = {"description", "tree_chance_scale", "plant_chance_scale", "ore_chance_scale"};
    if (!(j = parse_file(src, id, "features.json", err, err_cap))) return false;
    ok = known_keys(j, fk, 4, id, "features.json", err, err_cap)
      && num_field(j, "tree_chance_scale", 0, 4, &p->tree_scale, id, "features.json", err, err_cap)
      && num_field(j, "plant_chance_scale", 0, 4, &p->plant_scale, id, "features.json", err, err_cap)
      && num_field(j, "ore_chance_scale", 0, 4, &p->ore_scale, id, "features.json", err, err_cap);
    json_free(j);
    if (!ok) return false;

    static const char *const stk[] = {"description", "chance_scale"};
    if (!(j = parse_file(src, id, "structures.json", err, err_cap))) return false;
    ok = known_keys(j, stk, 2, id, "structures.json", err, err_cap)
      && num_field(j, "chance_scale", 0, 4, &p->structure_scale, id, "structures.json", err, err_cap);
    json_free(j);
    if (!ok) return false;

    EpochParams d;
    epoch_params_default(&d);
    p->identity = p->height_scale == 1.0f && p->height_offset == 0 && p->cont_bias == 0 && p->erosion_bias == 0 && p->weirdness_bias == 0
        && p->temp_bias == 0 && p->humid_bias == 0 && p->tunnel_width_scale == 1.0f && p->cheese_threshold_delta == 0
        && p->snowline_offset == 0 && p->treeline_offset == 0 && p->tree_scale == 1.0f && p->plant_scale == 1.0f
        && p->ore_scale == 1.0f && p->structure_scale == 1.0f && p->kernel == 0;
    return true;
}

static int cmp_u32(const void *a, const void *b) { u32 x = *(const u32 *)a, y = *(const u32 *)b; return x < y ? -1 : x > y; }

bool epoch_registry_load(const char *fs_root, char *err, size_t err_cap) {
    epoch_registry_clear();
    Src src = {fs_root};
    StrList names = {0};
    src_list_ids(&src, &names);
    u32 ids[EP_MAX_EPOCHS];
    int n = 0;
    for (int i = 0; i < names.n; i++) {
        char *end = NULL;
        unsigned long v = strtoul(names.d[i], &end, 10);
        if (!*names.d[i] || *end) continue; /* not an epoch directory */
        if (n >= EP_MAX_EPOCHS || v >= (unsigned long)EP_MAX_EPOCHS) { strlist_free(&names); FAIL("epoch directory '%s' is out of range; at most %d epochs are supported.", names.d[i], EP_MAX_EPOCHS); }
        ids[n++] = (u32)v;
    }
    strlist_free(&names);
    qsort(ids, (size_t)n, sizeof ids[0], cmp_u32); /* load order independent: ids are sorted before use */
    if (n == 0) FAIL("no epochs found. Expected data/dfe/epochs/0/ with biomes, caves, features, noise, structures, surface and hash.txt.");
    for (int i = 0; i < n; i++) {
        if (ids[i] != (u32)i) FAIL("epoch ids must run 0..N-1 without gaps, but epoch %d is missing.", i);
        Epoch *e = &g_epochs[i];
        e->id = (u32)i;
        StrList files = {0};
        src_list(&src, (u32)i, &files);
        for (int f = 0; f < files.n; f++) {
            bool known = !strcmp(files.d[f], "hash.txt");
            for (int k = 0; k < 6; k++) known |= !strcmp(files.d[f], EPOCH_FILES[k]);
            if (!known) { snprintf(err, err_cap, "epoch %d contains unexpected file '%s'; the hash would not cover it.", i, files.d[f]); strlist_free(&files); epoch_registry_clear(); return false; }
        }
        strlist_free(&files);
        char sub[240];
        if (!hash_epoch(&src, (u32)i, e->hash, sub, sizeof sub)) { snprintf(err, err_cap, "%s", sub); epoch_registry_clear(); return false; }
        size_t len = 0;
        u8 *text = src_read(&src, (u32)i, "hash.txt", &len);
        if (!text) { snprintf(err, err_cap, "epoch %d is missing hash.txt.", i); epoch_registry_clear(); return false; }
        char want[65] = {0}, have[65];
        for (size_t k = 0; k < len && k < 64; k++) want[k] = (char)text[k];
        free(text);
        epoch_hex(e->hash, 32, have);
        if (strncmp(want, have, 64)) {
            snprintf(err, err_cap, "epoch %d files do not match hash.txt (expected %.16s..., computed %.16s...). Epochs are frozen; the engine will not blend with a changed epoch.", i, want, have);
            epoch_registry_clear();
            return false;
        }
        if (!parse_params(&src, (u32)i, &e->p, sub, sizeof sub)) { snprintf(err, err_cap, "%s", sub); epoch_registry_clear(); return false; }
    }
    g_epoch_count = n;
    g_generation++;
    err[0] = 0;
    return true;
}

/* ------------------------------------------------------------------- CGM */

const char *cgm_status_text(CgmStatus s) {
    switch (s) {
    case CGM_OK: return "ok";
    case CGM_ERR_OVERFLOW: return "chunk generation metadata exceeds its size bound";
    case CGM_ERR_FORMAT: return "chunk generation metadata is malformed";
    case CGM_ERR_EPOCH: return "chunk generation metadata names an epoch this engine does not have";
    case CGM_ERR_HASH: return "chunk generation metadata epoch hash does not match the registry";
    }
    return "?";
}

void cgm_init(Cgm *c, u32 epoch_id, u64 seed) {
    memset(c, 0, sizeof *c);
    c->epoch_id = epoch_id;
    c->seed_snapshot = seed;
    const Epoch *e = epoch_get(epoch_id);
    if (e) memcpy(c->epoch_hash, e->hash, 32);
}

CgmStatus cgm_set_terrain(Cgm *c, const void *data, size_t len) {
    if (len > CGM_MAX_TERRAIN_BLOB) return CGM_ERR_OVERFLOW;
    memcpy(c->terrain_params, data, len);
    c->terrain_len = (u8)len;
    return CGM_OK;
}

CgmStatus cgm_set_biomes(Cgm *c, const void *data, size_t len) {
    if (len > CGM_MAX_BIOME_BLOB) return CGM_ERR_OVERFLOW;
    memcpy(c->biome_weights, data, len);
    c->biome_len = (u8)len;
    return CGM_OK;
}

CgmStatus cgm_add_claim(Cgm *c, const StructureClaim *claim) {
    if (c->claim_count >= CGM_MAX_CLAIMS) return CGM_ERR_OVERFLOW;
    c->claims[c->claim_count++] = *claim;
    return CGM_OK;
}

static void put32(u8 **p, u32 v) { for (int i = 0; i < 4; i++) *(*p)++ = (u8)(v >> (8 * i)); }
static void put64(u8 **p, u64 v) { for (int i = 0; i < 8; i++) *(*p)++ = (u8)(v >> (8 * i)); }
static u32 get32(const u8 **p) { u32 v = 0; for (int i = 0; i < 4; i++) v |= (u32) * (*p)++ << (8 * i); return v; }
static u64 get64(const u8 **p) { u64 v = 0; for (int i = 0; i < 8; i++) v |= (u64) * (*p)++ << (8 * i); return v; }

static size_t cgm_size(const Cgm *c) { return 4 + 4 + 32 + 8 + 1 + c->terrain_len + 1 + c->biome_len + 1 + (size_t)c->claim_count * (32 + 24 + 8); }

size_t cgm_serialize(const Cgm *c, u8 *out, size_t cap) {
    size_t need = cgm_size(c);
    if (need > cap || need > CGM_MAX_BYTES) return 0;
    u8 *p = out;
    memcpy(p, "CGM1", 4); p += 4;
    put32(&p, c->epoch_id);
    memcpy(p, c->epoch_hash, 32); p += 32;
    put64(&p, c->seed_snapshot);
    *p++ = c->terrain_len; memcpy(p, c->terrain_params, c->terrain_len); p += c->terrain_len;
    *p++ = c->biome_len; memcpy(p, c->biome_weights, c->biome_len); p += c->biome_len;
    *p++ = c->claim_count;
    for (int i = 0; i < c->claim_count; i++) {
        memcpy(p, c->claims[i].id, 32); p += 32;
        for (int k = 0; k < 3; k++) put32(&p, (u32)c->claims[i].min[k]);
        for (int k = 0; k < 3; k++) put32(&p, (u32)c->claims[i].max[k]);
        put64(&p, c->claims[i].seed);
    }
    return (size_t)(p - out);
}

CgmStatus cgm_deserialize(const u8 *in, size_t len, Cgm *out) {
    memset(out, 0, sizeof *out);
    if (len < 4 + 4 + 32 + 8 + 3 || len > CGM_MAX_BYTES || memcmp(in, "CGM1", 4)) return CGM_ERR_FORMAT;
    const u8 *p = in + 4, *end = in + len;
    out->epoch_id = get32(&p);
    memcpy(out->epoch_hash, p, 32); p += 32;
    out->seed_snapshot = get64(&p);
    out->terrain_len = *p++;
    if (out->terrain_len > CGM_MAX_TERRAIN_BLOB || p + out->terrain_len + 2 > end) return CGM_ERR_FORMAT;
    memcpy(out->terrain_params, p, out->terrain_len); p += out->terrain_len;
    out->biome_len = *p++;
    if (out->biome_len > CGM_MAX_BIOME_BLOB || p + out->biome_len + 1 > end) return CGM_ERR_FORMAT;
    memcpy(out->biome_weights, p, out->biome_len); p += out->biome_len;
    out->claim_count = *p++;
    if (out->claim_count > CGM_MAX_CLAIMS || p + (size_t)out->claim_count * 64 != end) return CGM_ERR_FORMAT;
    for (int i = 0; i < out->claim_count; i++) {
        memcpy(out->claims[i].id, p, 32); p += 32;
        out->claims[i].id[31] = 0;
        for (int k = 0; k < 3; k++) out->claims[i].min[k] = (i32)get32(&p);
        for (int k = 0; k < 3; k++) out->claims[i].max[k] = (i32)get32(&p);
        out->claims[i].seed = get64(&p);
    }
    const Epoch *e = epoch_get(out->epoch_id);
    if (!e) return CGM_ERR_EPOCH;
    if (memcmp(e->hash, out->epoch_hash, 32)) return CGM_ERR_HASH;
    return CGM_OK;
}

/* ------------------------------------------------------------- CGM store */

static u32 crc32_bytes(const void *data, size_t len) {
    u32 c = 0xFFFFFFFFu;
    const u8 *p = data;
    for (size_t i = 0; i < len; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (u32)-(i32)(c & 1));
    }
    return ~c;
}


typedef struct StoreSlot { u64 key; bool used; Cgm cgm; } StoreSlot;

static struct {
    Mutex *lock;
    StoreSlot *slots;
    u32 cap, count;
    FILE *file;
    bool (*probe)(int, int);
} G;

static void store_lock(void) {
    if (!G.lock) G.lock = mutex_create();
    mutex_lock(G.lock);
}

static u64 col_key(int cx, int cz) { return ((u64)(u32)cx << 32) | (u32)cz; }
static u32 key_hash(u64 k) { k ^= k >> 33; k *= 0xff51afd7ed558ccdull; k ^= k >> 33; return (u32)k; }

static StoreSlot *store_find(u64 key, bool create) {
    if (create && (G.count + 1) * 2 > G.cap) {
        u32 ncap = G.cap ? G.cap * 2 : 256;
        StoreSlot *old = G.slots;
        u32 ocap = G.cap;
        G.slots = xcalloc(ncap, sizeof *G.slots);
        G.cap = ncap;
        G.count = 0;
        for (u32 i = 0; i < ocap; i++) if (old[i].used) { StoreSlot *s = store_find(old[i].key, true); s->cgm = old[i].cgm; }
        free(old);
    }
    if (!G.cap) return NULL;
    u32 i = key_hash(key) & (G.cap - 1);
    for (;;) {
        StoreSlot *s = &G.slots[i];
        if (s->used && s->key == key) return s;
        if (!s->used) {
            if (!create) return NULL;
            s->used = true; s->key = key; G.count++;
            return s;
        }
        i = (i + 1) & (G.cap - 1);
    }
}

void cgm_store_clear(void) {
    store_lock();
    free(G.slots);
    G.slots = NULL;
    G.cap = G.count = 0;
    mutex_unlock(G.lock);
    g_generation++;
}

bool cgm_store_get(int cx, int cz, Cgm *out) {
    store_lock();
    StoreSlot *s = store_find(col_key(cx, cz), false);
    if (s && out) *out = s->cgm;
    mutex_unlock(G.lock);
    return s != NULL;
}

bool cgm_store_epoch(int cx, int cz, u32 *epoch) {
    store_lock();
    StoreSlot *s = store_find(col_key(cx, cz), false);
    if (s) *epoch = s->cgm.epoch_id;
    bool (*probe)(int, int) = G.probe;
    mutex_unlock(G.lock);
    if (s) return true;
    if (!probe || epoch_current() == 0 || !probe(cx, cz)) return false;
    *epoch = 0; /* saved before the feature existed: all epoch 0 */
    return true;
}

int cgm_store_count(void) {
    store_lock();
    int n = (int)G.count;
    mutex_unlock(G.lock);
    return n;
}

void cgm_store_set_legacy_probe(bool (*probe)(int, int)) { G.probe = probe; }

/* cgm.dat record: u32 magic "CGR1", i32 cx, i32 cz, u16 len, payload, u32 crc32 of everything before the crc. */
#define REC_MAGIC 0x31524743u

static bool write_record(int cx, int cz, const Cgm *c) {
    if (!G.file) return true;
    u8 rec[CGM_MAX_BYTES + 32], *p = rec;
    u8 payload[CGM_MAX_BYTES];
    size_t n = cgm_serialize(c, payload, sizeof payload);
    if (!n) return false;
    put32(&p, REC_MAGIC); put32(&p, (u32)cx); put32(&p, (u32)cz);
    *p++ = (u8)n; *p++ = (u8)(n >> 8);
    memcpy(p, payload, n); p += n;
    put32(&p, crc32_bytes(rec, (size_t)(p - rec)));
    size_t total = (size_t)(p - rec);
    if (fwrite(rec, 1, total, G.file) != total) return false;
    fflush(G.file);
    return true;
}

bool cgm_store_put(int cx, int cz, const Cgm *c) {
    store_lock();
    StoreSlot *s = store_find(col_key(cx, cz), true);
    s->cgm = *c;
    bool ok = write_record(cx, cz, c);
    mutex_unlock(G.lock);
    g_generation++;
    return ok;
}

bool cgm_store_has_file(const char *dir) {
    char path[600];
    snprintf(path, sizeof path, "%s/cgm.dat", dir);
    return path_exists(path);
}

bool cgm_store_open(const char *dir, char *err, size_t err_cap) {
    cgm_store_close();
    char path[600];
    snprintf(path, sizeof path, "%s/cgm.dat", dir);
    size_t len = 0;
    u8 *data = path_exists(path) ? file_read(path, &len) : NULL;
    size_t off = 0;
    while (data && off < len) {
        const u8 *p = data + off;
        if (len - off < 15) { free(data); FAIL("%s: truncated record at byte %zu. The world's chunk generation records are damaged; restore cgm.dat from a backup.", path, off); }
        u32 magic = get32(&p);
        int cx = (int)get32(&p), cz = (int)get32(&p);
        size_t n = p[0] | (size_t)p[1] << 8;
        p += 2;
        if (magic != REC_MAGIC || off + 15 + n > len) { free(data); FAIL("%s: bad record at byte %zu.", path, off); }
        size_t body = 14 + n;
        const u8 *crcp = data + off + body;
        if (get32(&crcp) != crc32_bytes(data + off, body)) { free(data); FAIL("%s: checksum mismatch in the record at byte %zu.", path, off); }
        Cgm c;
        CgmStatus st = cgm_deserialize(p, n, &c);
        if (st != CGM_OK) { free(data); FAIL("%s: column (%d, %d): %s (epoch %u).", path, cx, cz, cgm_status_text(st), c.epoch_id); }
        store_lock();
        store_find(col_key(cx, cz), true)->cgm = c;
        mutex_unlock(G.lock);
        off += body + 4;
    }
    free(data);
    G.file = fopen(path, "ab");
    if (!G.file) FAIL("cannot open %s for appending.", path);
    g_generation++;
    return true;
}

void cgm_store_close(void) {
    if (G.file) { fclose(G.file); G.file = NULL; }
    cgm_store_clear();
}

/* ----------------------------------------------------------------- blend */

u64 g_blend_plan_builds, g_blend_cache_hits;

static float smooth_ramp(float s) { s = CLAMP(s, 0.0f, 1.0f); return s * s * (3.0f - 2.0f * s); }

BlendPlan *blend_plan_build(int cx, int cz, float radius) {
    u32 cur = epoch_current();
    radius = CLAMP(radius, EP_RADIUS_MIN, EP_RADIUS_MAX);
    int span = (int)ceilf(radius / CHUNK_SIZE) + 1;
    int side = 2 * span + 1;
    u32 *grid = xmalloc((size_t)side * side * sizeof *grid);
    const u32 NONE = 0xFFFFFFFFu;
    bool any = false;
    for (int dz = -span; dz <= span; dz++)
        for (int dx = -span; dx <= span; dx++) {
            u32 e = NONE;
            u32 found;
            if (cgm_store_epoch(cx + dx, cz + dz, &found) && found < cur) { e = found; any = true; }
            grid[(dz + span) * side + dx + span] = e;
        }
    if (!any) { free(grid); return NULL; }
    BlendPlan *p = xcalloc(1, sizeof *p);
    p->current = cur; p->radius = radius; p->cx = cx; p->cz = cz;
    p->cols = xmalloc((size_t)side * side * sizeof *p->cols);
    /* Only boundary columns can be the nearest column of their epoch, so interior ones are dropped. */
    for (int z = 0; z < side; z++)
        for (int x = 0; x < side; x++) {
            u32 e = grid[z * side + x];
            if (e == NONE) continue;
            bool edge = x == 0 || z == 0 || x == side - 1 || z == side - 1;
            if (!edge) {
                static const int d4[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (int k = 0; k < 4 && !edge; k++) edge = grid[(z + d4[k][1]) * side + x + d4[k][0]] != e;
            }
            if (edge) p->cols[p->n_cols++] = (BlendCol){cx + x - span, cz + z - span, e};
        }
    /* Claims: gather from every older column in the window; deterministic order (z, x). */
    StructureClaim *claims = NULL;
    int nc = 0, ccap = 0;
    for (int z = 0; z < side; z++)
        for (int x = 0; x < side; x++) {
            if (grid[z * side + x] == NONE) continue;
            Cgm c;
            if (!cgm_store_get(cx + x - span, cz + z - span, &c)) continue;
            for (int i = 0; i < c.claim_count; i++) {
                if (nc == ccap) { ccap = ccap ? ccap * 2 : 8; claims = xrealloc(claims, (size_t)ccap * sizeof *claims); }
                claims[nc++] = c.claims[i];
            }
        }
    p->claims = claims;
    p->n_claims = nc;
    free(grid);
    g_blend_plan_builds++;
    return p;
}

void blend_plan_free(BlendPlan *p) {
    if (!p) return;
    free(p->cols);
    free(p->claims);
    free(p);
}

/* Plan cache.  Entries are valid for one epoch_generation; a hit hands out a shared reference. */
#define PLAN_CACHE 64
typedef struct PlanSlot { BlendPlan *plan; int refs; u64 gen; u64 stamp; bool none; int cx, cz; } PlanSlot;
static struct { Mutex *lock; PlanSlot s[PLAN_CACHE]; u64 clock; } PC;

typedef struct PlanHandle { BlendPlan plan; PlanSlot *slot; } PlanHandle; /* unused; kept simple with refs in slots */

static void slot_drop(PlanSlot *s) {
    blend_plan_free(s->plan);
    memset(s, 0, sizeof *s);
}

BlendPlan *blend_plan_acquire(int cx, int cz) {
    if (!PC.lock) PC.lock = mutex_create();
    u64 gen = epoch_generation();
    mutex_lock(PC.lock);
    PlanSlot *victim = NULL;
    for (int i = 0; i < PLAN_CACHE; i++) {
        PlanSlot *s = &PC.s[i];
        if (s->gen && s->gen != gen && s->refs == 0) slot_drop(s);
        if (s->gen == gen && s->cx == cx && s->cz == cz && (s->plan || s->none)) {
            s->stamp = ++PC.clock;
            g_blend_cache_hits++;
            BlendPlan *p = s->plan;
            if (p) s->refs++;
            mutex_unlock(PC.lock);
            return p;
        }
        if (!s->gen && !victim) victim = s;
    }
    mutex_unlock(PC.lock);
    BlendPlan *p = blend_plan_build(cx, cz, epoch_blend_radius());
    mutex_lock(PC.lock);
    if (!victim || victim->gen) {
        victim = NULL;
        for (int i = 0; i < PLAN_CACHE; i++) {
            PlanSlot *s = &PC.s[i];
            if (s->refs == 0 && (!victim || !s->gen || s->stamp < victim->stamp)) victim = s;
        }
    }
    if (victim && victim->refs == 0) {
        if (victim->gen) slot_drop(victim);
        victim->gen = gen; victim->cx = cx; victim->cz = cz; victim->plan = p; victim->none = p == NULL;
        victim->stamp = ++PC.clock;
        if (p) victim->refs = 1;
    } else if (p) {
        /* No evictable slot: hand out an uncached plan; release frees it because it is not in the cache. */
    }
    mutex_unlock(PC.lock);
    return p;
}

void blend_plan_release(BlendPlan *p) {
    if (!p) return;
    mutex_lock(PC.lock);
    for (int i = 0; i < PLAN_CACHE; i++)
        if (PC.s[i].plan == p) { PC.s[i].refs--; mutex_unlock(PC.lock); return; }
    mutex_unlock(PC.lock);
    blend_plan_free(p); /* an uncached plan */
}

static float col_distance(const BlendCol *c, float px, float pz) {
    float x0 = (float)(c->cx * CHUNK_SIZE), z0 = (float)(c->cz * CHUNK_SIZE);
    float dx = MAX(MAX(x0 - px, 0.0f), px - (x0 + CHUNK_SIZE)), dz = MAX(MAX(z0 - pz, 0.0f), pz - (z0 + CHUNK_SIZE));
    return sqrtf(dx * dx + dz * dz);
}

float blend_old_distance(const BlendPlan *p, int wx, int wz) {
    if (!p) return 1e9f;
    float best = 1e9f, px = (float)wx + 0.5f, pz = (float)wz + 0.5f;
    for (int i = 0; i < p->n_cols; i++) best = MIN(best, col_distance(&p->cols[i], px, pz));
    return best;
}

void blend_weights(const BlendPlan *p, int wx, int wz, BlendCell *out) {
    out->n = 1;
    out->a[0] = 1.0f;
    out->id[0] = p ? p->current : epoch_current();
    if (!p) return;
    float px = (float)wx + 0.5f, pz = (float)wz + 0.5f;
    float de[EP_MAX_EPOCHS];
    u32 seen[EP_MAX_EPOCHS];
    int ns = 0;
    float dmin = 1e9f;
    int exact = -1;
    for (int i = 0; i < p->n_cols; i++) {
        float d = col_distance(&p->cols[i], px, pz);
        int k = 0;
        while (k < ns && seen[k] != p->cols[i].epoch) k++;
        if (k == ns) { seen[ns] = p->cols[i].epoch; de[ns++] = d; } else de[k] = MIN(de[k], d);
        if (d < dmin) dmin = d;
        if (d == 0.0f) exact = (int)p->cols[i].epoch;
    }
    if (exact >= 0) { /* inside an older column: that epoch's own terrain, exactly */
        out->n = 1; out->id[0] = (u32)exact; out->a[0] = 1.0f;
        return;
    }
    if (dmin >= p->radius) return;
    /* Keep the nearest EP_BLEND_MAX-1 older epochs; ties go to the lower id so the choice is order independent. */
    int idx[EP_MAX_EPOCHS], m = 0;
    for (int k = 0; k < ns; k++) if (de[k] < p->radius) idx[m++] = k;
    for (int i = 1; i < m; i++) {
        int v = idx[i], j = i - 1;
        while (j >= 0 && (de[idx[j]] > de[v] || (de[idx[j]] == de[v] && seen[idx[j]] > seen[v]))) { idx[j + 1] = idx[j]; j--; }
        idx[j + 1] = v;
    }
    m = MIN(m, EP_BLEND_MAX - 1);
    float w = smooth_ramp(dmin / p->radius); /* weight of the current epoch: 0 at the older border, 1 at the radius */
    float q[EP_BLEND_MAX], qsum = 0.0f;
    for (int i = 0; i < m; i++) { float d = de[idx[i]]; q[i] = 1.0f / (d * d + 0.25f); qsum += q[i]; }
    out->n = m + 1;
    out->id[0] = p->current;
    out->a[0] = w;
    float sum = w;
    for (int i = 0; i < m; i++) {
        out->id[i + 1] = seen[idx[i]];
        out->a[i + 1] = (1.0f - w) * q[i] / qsum;
        sum += out->a[i + 1];
    }
    for (int i = 0; i < out->n; i++) out->a[i] /= sum; /* exact normalisation, the sum is 1 to float rounding */
}

float blend_scalar(const BlendCell *c, const float *per_epoch) {
    float v = 0.0f;
    for (int i = 0; i < c->n; i++) v += c->a[i] * per_epoch[i];
    return v;
}

void blend_vector(const BlendCell *c, const float *per_epoch, int dim, float *out) {
    for (int k = 0; k < dim; k++) out[k] = 0.0f;
    for (int i = 0; i < c->n; i++)
        for (int k = 0; k < dim; k++) out[k] += c->a[i] * per_epoch[i * dim + k];
}

/* -------------------------------------------------------------- min cut */

/* FIFO push-relabel on a grid plus a source and a sink node.  Capacities are integers so every platform gets the
 * same cut.  After the first phase the cells that cannot reach the sink in the residual graph form the maximal
 * source side of a minimum cut. */
typedef struct FlowNet { int n, m; int *head, *next, *to; i64 *cap; } FlowNet;

static void net_edge(FlowNet *g, int u, int v, i64 c, i64 rc) {
    g->to[g->m] = v; g->cap[g->m] = c; g->next[g->m] = g->head[u]; g->head[u] = g->m++;
    g->to[g->m] = u; g->cap[g->m] = rc; g->next[g->m] = g->head[v]; g->head[v] = g->m++;
}

#define SEAM_INF ((i64)1 << 40)

i64 seam_mincut(int w, int h, const int *cap_h, const int *cap_v, const u8 *forced, u8 *label) {
    int cells = w * h, S = cells, T = cells + 1, n = cells + 2;
    int max_edges = 2 * ((w - 1) * h + w * (h - 1) + cells);
    FlowNet g = {n, 0, xmalloc((size_t)n * sizeof(int)), xmalloc((size_t)max_edges * sizeof(int)), xmalloc((size_t)max_edges * sizeof(int)), xmalloc((size_t)max_edges * sizeof(i64))};
    for (int i = 0; i < n; i++) g.head[i] = -1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int i = y * w + x;
            if (x + 1 < w) net_edge(&g, i, i + 1, cap_h[y * (w - 1) + x], cap_h[y * (w - 1) + x]);
            if (y + 1 < h) net_edge(&g, i, i + w, cap_v[y * w + x], cap_v[y * w + x]);
            if (forced[i] == 1) net_edge(&g, S, i, SEAM_INF, 0);
            if (forced[i] == 2) net_edge(&g, i, T, SEAM_INF, 0);
        }
    i64 *excess = xcalloc((size_t)n, sizeof(i64));
    int *height = xcalloc((size_t)n, sizeof(int)), *cur = xmalloc((size_t)n * sizeof(int)), *queue = xmalloc((size_t)(n + 1) * sizeof(int));
    bool *queued = xcalloc((size_t)n, sizeof(bool));
    int qh = 0, qt = 0, qn = n + 1;
    for (int i = 0; i < n; i++) cur[i] = g.head[i];
    height[S] = n;
    for (int e = g.head[S]; e >= 0; e = g.next[e]) {
        i64 f = g.cap[e];
        if (!f) continue;
        g.cap[e] -= f; g.cap[e ^ 1] += f; excess[g.to[e]] += f;
        int v = g.to[e];
        if (v != S && v != T && !queued[v]) { queue[qt] = v; qt = (qt + 1) % qn; queued[v] = true; }
    }
    while (qh != qt) {
        int u = queue[qh];
        qh = (qh + 1) % qn;
        queued[u] = false;
        while (excess[u] > 0) {
            if (cur[u] < 0) {
                int mh = 2 * n;
                for (int e = g.head[u]; e >= 0; e = g.next[e]) if (g.cap[e] > 0 && height[g.to[e]] < mh) mh = height[g.to[e]];
                height[u] = mh + 1;
                cur[u] = g.head[u];
                if (height[u] > 2 * n) break;
                continue;
            }
            int e = cur[u], v = g.to[e];
            if (g.cap[e] > 0 && height[u] == height[v] + 1) {
                i64 f = MIN(excess[u], g.cap[e]);
                g.cap[e] -= f; g.cap[e ^ 1] += f; excess[u] -= f; excess[v] += f;
                if (v != S && v != T && !queued[v]) { queue[qt] = v; qt = (qt + 1) % qn; queued[v] = true; }
            } else cur[u] = g.next[e];
        }
    }
    i64 flow = excess[T];
    /* Cells that can still reach T in the residual graph are on the sink side. */
    bool *reach = xcalloc((size_t)n, sizeof(bool));
    int *stack = xmalloc((size_t)n * sizeof(int)), sp = 0;
    stack[sp++] = T; reach[T] = true;
    while (sp) {
        int v = stack[--sp];
        for (int e = g.head[v]; e >= 0; e = g.next[e]) {
            int x = g.to[e];
            if (!reach[x] && g.cap[e ^ 1] > 0) { reach[x] = true; stack[sp++] = x; }
        }
    }
    for (int i = 0; i < cells; i++) label[i] = reach[i] ? 0 : 1;
    free(reach); free(stack); free(queued); free(queue); free(cur); free(height); free(excess);
    free(g.head); free(g.next); free(g.to); free(g.cap);
    return flow;
}
