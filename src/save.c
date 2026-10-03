/* World persistence. A world directory holds world.json (seed, player, block name table) and region files
 * with 16x16 columns each. Only columns that were edited are written: untouched terrain regenerates from the
 * seed, which keeps saves tiny and lets generation improve between versions without invalidating old worlds.
 *
 * Columns are serialised on the main thread (chunk storage is main-thread-only), then compressed with LZ4 and
 * written by a worker job, so a save never stalls a frame. Block ids never reach the disk: every state is stored
 * as (index into the world's block name table, state index) and remapped on load.
 *
 * Rejected: one file per chunk (millions of inodes, slow directory scans) and a single monolithic world file
 * (rewrites grow with the world). Region files rewrite in place when a column still fits its old slot and are
 * compacted on close when more than half the file is dead space. */
#include "dfe.h"
#include <lz4.h>

#define REGION_SHIFT 4
#define REGION_COLS (1 << (2 * REGION_SHIFT))
#define REGION_MAGIC 0x52454644u /* "DFER" */
#define REGION_VERSION 1u
#define REGION_HEADER_BYTES 16
#define REGION_ENTRY_BYTES 12
#define REGION_DATA_START (REGION_HEADER_BYTES + REGION_COLS * REGION_ENTRY_BYTES)
#define REGION_CACHE 8
#define COLUMN_MAGIC 0x31434644u /* "DFC1" */
#define CHUNK_PALETTED 1u
#define CHUNK_HAS_LIGHT 2u
#define META_VERSION 1
#define COMPACT_MIN_WASTE (1u << 20)
#define MAX_COLUMN_LAYERS 4096

typedef struct RegionEntry { u32 offset, comp_size, raw_size; } RegionEntry;

typedef struct Region {
    int rx, rz;
    FILE *f;
    bool exists;
    RegionEntry entries[REGION_COLS];
    u32 file_end;
    u64 stamp;
} Region;

typedef struct Pending {
    int cx, cz;
    u64 seq;
    u8 *raw;
    size_t raw_len;
} Pending;

static struct {
    bool open;
    char dir[512];
    u64 seed;
    SaveMeta meta;
    Mutex *lock;
    Region cache[REGION_CACHE];
    int cache_used;
    u64 stamp, seq;
    VEC(Pending) pending;
    BlockNameTable names;
    StrMap name_index;
    u16 *rt_to_saved; /* runtime block id to name table index */
    int jobs_inflight;
} S;

/* ------------------------------------------------------------ byte buffer */

typedef struct Buf { u8 *d; size_t n, cap; } Buf;

static void buf_put(Buf *b, const void *p, size_t n) {
    if (b->n + n > b->cap) {
        b->cap = MAX(b->cap * 2, b->n + n + 4096);
        b->d = xrealloc(b->d, b->cap);
    }
    memcpy(b->d + b->n, p, n);
    b->n += n;
}
static void buf_u8(Buf *b, u8 v) { buf_put(b, &v, 1); }
static void buf_u16(Buf *b, u16 v) { buf_put(b, &v, 2); }
static void buf_u32(Buf *b, u32 v) { buf_put(b, &v, 4); }

typedef struct Cursor { const u8 *p; size_t left; bool bad; } Cursor;

static bool cur_take(Cursor *c, void *out, size_t n) {
    if (c->bad || c->left < n) { c->bad = true; return false; }
    memcpy(out, c->p, n);
    c->p += n;
    c->left -= n;
    return true;
}
static u8 cur_u8(Cursor *c) { u8 v = 0; cur_take(c, &v, 1); return v; }
static u16 cur_u16(Cursor *c) { u16 v = 0; cur_take(c, &v, 2); return v; }
static u32 cur_u32(Cursor *c) { u32 v = 0; cur_take(c, &v, 4); return v; }

/* ----------------------------------------------------------- name mapping */

static void state_to_pair(u16 state, u16 *block, u16 *index) {
    const BlockDef *b = block_of_state(state);
    *block = S.rt_to_saved[b->id];
    *index = (u16)(state - b->first_state);
}

static void build_name_table(void) {
    strmap_init(&S.name_index);
    for (int i = 0; i < S.names.names.n; i++) strmap_set(&S.name_index, S.names.names.d[i], (u32)i);
    /* Every registered block is appended now so the table never changes while workers are loading columns. */
    for (int i = 0; i < g_block_count; i++) {
        u32 at;
        if (strmap_get(&S.name_index, g_blocks[i]->name, &at)) continue;
        strmap_set(&S.name_index, g_blocks[i]->name, (u32)S.names.names.n);
        vec_push(S.names.names, xstrdup(g_blocks[i]->name));
    }
    S.rt_to_saved = xcalloc((size_t)g_block_count, sizeof(u16));
    for (int i = 0; i < g_block_count; i++) {
        u32 at = 0;
        strmap_get(&S.name_index, g_blocks[i]->name, &at);
        S.rt_to_saved[g_blocks[i]->id] = (u16)at;
    }
}

/* ------------------------------------------------------------------- meta */

static void meta_path(char *out, size_t cap) { snprintf(out, cap, "%s/world.json", S.dir); }

static bool meta_read(void) {
    char path[600];
    meta_path(path, sizeof path);
    size_t len;
    u8 *text = file_read(path, &len);
    if (!text) return false;
    char err[160];
    int line = 0;
    Json *j = json_parse((const char *)text, len, err, sizeof err, &line);
    free(text);
    if (!j) {
        LOGE("World file %s line %d is unreadable (%s). Fix or delete it; the region files beside it are untouched.", path, line, err);
        return false;
    }
    S.seed = (u64)json_num(j, "seed", 0) + ((u64)json_num(j, "seed_high", 0) * 4294967296.0);
    const Json *p = json_get(j, "player");
    if (p) {
        S.meta.has_player = true;
        S.meta.x = json_num(p, "x", 0); S.meta.y = json_num(p, "y", 0); S.meta.z = json_num(p, "z", 0);
        S.meta.yaw = (float)json_num(p, "yaw", 0); S.meta.pitch = (float)json_num(p, "pitch", 0);
        S.meta.flying = json_bool(p, "flying", false);
    }
    const Json *inv = json_get(j, "inventory");
    if (inv) {
        S.meta.has_inventory = true;
        S.meta.creative = json_bool(inv, "creative", true);
        S.meta.selected = (int)json_num(inv, "selected", 0);
        const Json *slots = json_get(inv, "slots");
        for (int i = 0; slots && i < json_len(slots) && i < SAVE_INV_SLOTS; i++) {
            const Json *e = json_at(slots, i);
            snprintf(S.meta.inv_name[i], SAVE_BLOCK_NAME_LEN, "%s", json_as_str(json_get(e, "block"), ""));
            S.meta.inv_count[i] = (u8)CLAMP((int)json_num(e, "count", 0), 0, 255);
        }
    }
    S.meta.day_time = json_num(j, "time", 0.3);
    const Json *names = json_get(j, "blocks");
    for (int i = 0; names && i < json_len(names); i++) vec_push(S.names.names, xstrdup(json_as_str(json_at(names, i), "dfe:missing")));
    json_free(j);
    return true;
}

static bool meta_write(void) {
    JsonWriter w = {0};
    jw_begin_obj(&w);
    jw_key(&w, "version"); jw_num(&w, META_VERSION);
    jw_key(&w, "seed"); jw_num(&w, (double)(S.seed & 0xFFFFFFFFull));
    jw_key(&w, "seed_high"); jw_num(&w, (double)(S.seed >> 32));
    jw_key(&w, "time"); jw_num(&w, S.meta.day_time);
    if (S.meta.has_player) {
        jw_key(&w, "player");
        jw_begin_obj(&w);
        jw_key(&w, "x"); jw_num(&w, S.meta.x);
        jw_key(&w, "y"); jw_num(&w, S.meta.y);
        jw_key(&w, "z"); jw_num(&w, S.meta.z);
        jw_key(&w, "yaw"); jw_num(&w, S.meta.yaw);
        jw_key(&w, "pitch"); jw_num(&w, S.meta.pitch);
        jw_key(&w, "flying"); jw_bool(&w, S.meta.flying);
        jw_end_obj(&w);
    }
    if (S.meta.has_inventory) {
        jw_key(&w, "inventory");
        jw_begin_obj(&w);
        jw_key(&w, "creative"); jw_bool(&w, S.meta.creative);
        jw_key(&w, "selected"); jw_num(&w, S.meta.selected);
        jw_key(&w, "slots");
        jw_begin_arr(&w);
        for (int i = 0; i < SAVE_INV_SLOTS; i++) {
            jw_begin_obj(&w);
            jw_key(&w, "block"); jw_str(&w, S.meta.inv_name[i]);
            jw_key(&w, "count"); jw_num(&w, S.meta.inv_count[i]);
            jw_end_obj(&w);
        }
        jw_end_arr(&w);
        jw_end_obj(&w);
    }
    jw_key(&w, "blocks");
    jw_begin_arr(&w);
    for (int i = 0; i < S.names.names.n; i++) jw_str(&w, S.names.names.d[i]);
    jw_end_arr(&w);
    jw_end_obj(&w);
    char path[600];
    meta_path(path, sizeof path);
    bool ok = file_write_atomic(path, w.buf, w.len);
    if (!ok) LOGE("Could not write %s. Check that the save folder is writable and the disk is not full.", path);
    jw_free(&w);
    return ok;
}

/* ---------------------------------------------------------------- regions */

static void region_path(const Region *r, char *out, size_t cap) { snprintf(out, cap, "%s/region/r.%d.%d.dfr", S.dir, r->rx, r->rz); }

static void region_flush_header(Region *r) {
    u32 head[4] = {REGION_MAGIC, REGION_VERSION, 0, 0};
    fseek(r->f, 0, SEEK_SET);
    fwrite(head, sizeof head, 1, r->f);
    fwrite(r->entries, sizeof r->entries, 1, r->f);
}

static u32 region_live_bytes(const Region *r) {
    u32 live = 0;
    for (int i = 0; i < REGION_COLS; i++) live += r->entries[i].comp_size;
    return live;
}

/* Rewrites the file with only live blobs. Runs under the lock when a region leaves the cache. */
static void region_compact(Region *r) {
    char path[600], tmp[620];
    region_path(r, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *out = fopen(tmp, "wb");
    if (!out) return;
    Region n = *r;
    n.f = out;
    u32 at = REGION_DATA_START;
    fseek(out, REGION_DATA_START, SEEK_SET);
    bool ok = true;
    for (int i = 0; i < REGION_COLS && ok; i++) {
        if (!r->entries[i].comp_size) continue;
        u8 *blob = xmalloc(r->entries[i].comp_size);
        fseek(r->f, (long)r->entries[i].offset, SEEK_SET);
        ok = fread(blob, 1, r->entries[i].comp_size, r->f) == r->entries[i].comp_size && fwrite(blob, 1, r->entries[i].comp_size, out) == r->entries[i].comp_size;
        free(blob);
        n.entries[i].offset = at;
        at += r->entries[i].comp_size;
    }
    n.file_end = at;
    if (ok) region_flush_header(&n);
    ok = (fclose(out) == 0) && ok;
    fclose(r->f);
    r->f = NULL;
    if (!ok) { remove(tmp); return; }
#ifdef _WIN32
    remove(path);
#endif
    rename(tmp, path);
}

static void region_close(Region *r) {
    if (r->f) {
        fflush(r->f);
        u32 live = region_live_bytes(r);
        u32 waste = r->file_end > REGION_DATA_START + live ? r->file_end - REGION_DATA_START - live : 0;
        if (waste > COMPACT_MIN_WASTE && waste > live) region_compact(r);
        if (r->f) fclose(r->f);
    }
    memset(r, 0, sizeof *r);
}

static Region *region_get(int rx, int rz) {
    for (int i = 0; i < S.cache_used; i++)
        if (S.cache[i].rx == rx && S.cache[i].rz == rz) { S.cache[i].stamp = ++S.stamp; return &S.cache[i]; }
    Region *slot;
    if (S.cache_used < REGION_CACHE) slot = &S.cache[S.cache_used++];
    else {
        slot = &S.cache[0];
        for (int i = 1; i < REGION_CACHE; i++) if (S.cache[i].stamp < slot->stamp) slot = &S.cache[i];
        region_close(slot);
    }
    memset(slot, 0, sizeof *slot);
    slot->rx = rx; slot->rz = rz; slot->stamp = ++S.stamp;
    char path[600];
    region_path(slot, path, sizeof path);
    slot->f = fopen(path, "r+b");
    if (!slot->f) return slot;
    u32 head[4];
    fseek(slot->f, 0, SEEK_END);
    long size = ftell(slot->f);
    fseek(slot->f, 0, SEEK_SET);
    if (fread(head, sizeof head, 1, slot->f) != 1 || head[0] != REGION_MAGIC || head[1] != REGION_VERSION ||
        fread(slot->entries, sizeof slot->entries, 1, slot->f) != 1 || size < REGION_DATA_START) {
        LOGE("Region file %s is damaged or from a newer version. It was set aside as .bad and its columns will regenerate.", path);
        fclose(slot->f);
        slot->f = NULL;
        char bad[620];
        snprintf(bad, sizeof bad, "%s.bad", path);
        remove(bad);
        rename(path, bad);
        memset(slot->entries, 0, sizeof slot->entries);
        return slot;
    }
    slot->exists = true;
    slot->file_end = (u32)size;
    return slot;
}

static int column_slot(int cx, int cz) { return (floor_mod(cz, 1 << REGION_SHIFT) << REGION_SHIFT) | floor_mod(cx, 1 << REGION_SHIFT); }

static bool region_create_if_needed(Region *r) {
    if (r->f) return true;
    char path[600];
    region_path(r, path, sizeof path);
    r->f = fopen(path, "w+b");
    if (!r->f) { LOGE("Could not create region file %s. Check that the save folder is writable.", path); return false; }
    memset(r->entries, 0, sizeof r->entries);
    r->file_end = REGION_DATA_START;
    region_flush_header(r);
    r->exists = true;
    return true;
}

/* --------------------------------------------------------- serialisation */

static void write_state_pair(Buf *b, u16 state) {
    u16 blk, idx;
    state_to_pair(state, &blk, &idx);
    buf_u16(b, blk);
    buf_u16(b, idx);
}

static void write_chunk(Buf *b, const Chunk *c) {
    u8 flags = (c->bits ? CHUNK_PALETTED : 0) | (c->light ? CHUNK_HAS_LIGHT : 0);
    buf_u8(b, flags);
    if (!c->bits) write_state_pair(b, c->uniform);
    else {
        buf_u8(b, c->bits);
        if (c->bits == 16) {
            const u16 *s = (const u16 *)c->data;
            u16 *planes = xmalloc(CHUNK_VOL * 2 * sizeof(u16));
            for (int i = 0; i < CHUNK_VOL; i++) state_to_pair(s[i], &planes[i], &planes[CHUNK_VOL + i]);
            buf_put(b, planes, CHUNK_VOL * 2 * sizeof(u16));
            free(planes);
        } else {
            buf_u16(b, c->pal_n);
            for (int i = 0; i < c->pal_n; i++) write_state_pair(b, c->pal[i]);
            buf_put(b, c->data, (size_t)CHUNK_VOL * c->bits / 8);
        }
    }
    if (c->light) buf_put(b, c->light, CHUNK_VOL * sizeof(u16));
    else buf_u16(b, c->light_uniform);
}

static u16 read_state_pair(Cursor *c) {
    u16 blk = cur_u16(c), idx = cur_u16(c);
    return block_table_remap_state(&S.names, blk, idx);
}

static bool read_chunk(Cursor *c, Chunk *ch) {
    u8 flags = cur_u8(c);
    if (!(flags & CHUNK_PALETTED)) ch->uniform = read_state_pair(c);
    else {
        u8 bits = cur_u8(c);
        if (bits == 16) {
            u16 *planes = xmalloc(CHUNK_VOL * 2 * sizeof(u16));
            if (!cur_take(c, planes, CHUNK_VOL * 2 * sizeof(u16))) { free(planes); return false; }
            u16 *flat = xmalloc(CHUNK_VOL * sizeof(u16));
            for (int i = 0; i < CHUNK_VOL; i++) flat[i] = block_table_remap_state(&S.names, planes[i], planes[CHUNK_VOL + i]);
            free(planes);
            ch->bits = 16;
            ch->data = (u32 *)flat;
        } else {
            if (bits != 1 && bits != 2 && bits != 4 && bits != 8) return false;
            u16 n = cur_u16(c);
            if (n == 0 || n > (1u << bits)) return false;
            ch->pal = xmalloc(((size_t)1 << bits) * sizeof(u16));
            for (int i = 0; i < n; i++) ch->pal[i] = read_state_pair(c);
            ch->pal_n = n;
            ch->bits = bits;
            size_t bytes = (size_t)CHUNK_VOL * bits / 8;
            ch->data = xmalloc(bytes);
            if (!cur_take(c, ch->data, bytes)) return false;
        }
    }
    if (flags & CHUNK_HAS_LIGHT) {
        ch->light = xmalloc(CHUNK_VOL * sizeof(u16));
        if (!cur_take(c, ch->light, CHUNK_VOL * sizeof(u16))) return false;
    } else ch->light_uniform = cur_u16(c);
    return !c->bad;
}

static bool deserialize_column(const u8 *raw, size_t len, int cx, int cz, SavedColumn *out) {
    Cursor c = {raw, len, false};
    if (cur_u32(&c) != COLUMN_MAGIC) return false;
    int lo = (int)cur_u32(&c), hi = (int)cur_u32(&c);
    if (hi < lo || hi - lo + 1 > MAX_COLUMN_LAYERS) return false;
    u16 deep = read_state_pair(&c);
    int layers = hi - lo + 1;
    Chunk **chunks = xcalloc((size_t)layers, sizeof(Chunk *));
    bool ok = !c.bad;
    for (int k = 0; k < layers && ok; k++) {
        chunks[k] = chunk_create(cx, lo + k, cz);
        chunks[k]->flags = CF_GENERATED | CF_PERSISTENT;
        ok = read_chunk(&c, chunks[k]);
    }
    if (!ok) {
        for (int k = 0; k < layers; k++) chunk_destroy(chunks[k]);
        free(chunks);
        return false;
    }
    out->lo = lo; out->hi = hi; out->deep_state = deep; out->chunks = chunks;
    return true;
}

/* ------------------------------------------------------------- public API */

bool save_open(const char *dir, u64 default_seed) {
    memset(&S, 0, sizeof S);
    snprintf(S.dir, sizeof S.dir, "%s", dir);
    S.lock = mutex_create();
    S.seed = default_seed;
    S.meta.day_time = 0.3;
    char regions[600];
    snprintf(regions, sizeof regions, "%s/region", dir);
    if (!dir_make_all(regions)) {
        LOGE("Could not create the save folder %s. Check the path and its permissions.", dir);
        mutex_destroy(S.lock);
        memset(&S, 0, sizeof S);
        return false;
    }
    char path[600];
    meta_path(path, sizeof path);
    bool existed = path_exists(path);
    if (existed && !meta_read()) {
        mutex_destroy(S.lock);
        memset(&S, 0, sizeof S);
        return false;
    }
    build_name_table();
    S.open = true;
    if (!existed) meta_write();
    LOGI("World '%s' %s, seed %llu, %d block names", dir, existed ? "loaded" : "created", (unsigned long long)S.seed, S.names.names.n);
    return true;
}

bool save_active(void) { return S.open; }
u64 save_seed(void) { return S.seed; }
SaveMeta *save_meta(void) { return &S.meta; }

static void pending_remove(int cx, int cz, u64 seq) {
    for (int i = 0; i < S.pending.n; i++) {
        Pending *p = &S.pending.d[i];
        if (p->cx == cx && p->cz == cz && p->seq == seq) {
            free(p->raw);
            S.pending.d[i] = S.pending.d[--S.pending.n];
            return;
        }
    }
}

static bool pending_has_newer(int cx, int cz, u64 seq) {
    for (int i = 0; i < S.pending.n; i++)
        if (S.pending.d[i].cx == cx && S.pending.d[i].cz == cz && S.pending.d[i].seq > seq) return true;
    return false;
}

typedef struct SaveJob { int cx, cz; u64 seq; } SaveJob;

static void write_column(int cx, int cz, const u8 *comp, int comp_size, size_t raw_size) {
    Region *r = region_get(cx >> REGION_SHIFT, cz >> REGION_SHIFT);
    if (!region_create_if_needed(r)) return;
    RegionEntry *e = &r->entries[column_slot(cx, cz)];
    u32 at = e->offset;
    if (!e->comp_size || (u32)comp_size > e->comp_size) { at = r->file_end; r->file_end += (u32)comp_size; }
    fseek(r->f, (long)at, SEEK_SET);
    fwrite(comp, 1, (size_t)comp_size, r->f);
    e->offset = at;
    e->comp_size = (u32)comp_size;
    e->raw_size = (u32)raw_size;
    fseek(r->f, (long)(REGION_HEADER_BYTES + column_slot(cx, cz) * REGION_ENTRY_BYTES), SEEK_SET);
    fwrite(e, sizeof *e, 1, r->f);
    fflush(r->f);
}

static void save_job_run(void *data, int worker) {
    SaveJob *j = data;
    u8 *raw = NULL;
    size_t raw_len = 0;
    mutex_lock(S.lock);
    for (int i = 0; i < S.pending.n; i++)
        if (S.pending.d[i].seq == j->seq) { raw = S.pending.d[i].raw; raw_len = S.pending.d[i].raw_len; }
    mutex_unlock(S.lock);
    if (!raw) return;
    /* The blob stays owned by the pending list until this job removes it, so it is safe to read unlocked. */
    int bound = LZ4_compressBound((int)raw_len);
    u8 *comp = xmalloc((size_t)bound);
    int comp_size = LZ4_compress_default((const char *)raw, (char *)comp, (int)raw_len, bound);
    mutex_lock(S.lock);
    if (comp_size > 0 && !pending_has_newer(j->cx, j->cz, j->seq)) write_column(j->cx, j->cz, comp, comp_size, raw_len);
    pending_remove(j->cx, j->cz, j->seq);
    mutex_unlock(S.lock);
    free(comp);
}

static void save_job_complete(void *data) {
    S.jobs_inflight--;
    free(data);
}

void save_store_column(const Column *col, Chunk *const *chunks) {
    if (!S.open) return;
    int layers = col->hi_cy - col->lo_cy + 1;
    Buf b = {0};
    buf_u32(&b, COLUMN_MAGIC);
    buf_u32(&b, (u32)col->lo_cy);
    buf_u32(&b, (u32)col->hi_cy);
    write_state_pair(&b, col->deep_state);
    for (int k = 0; k < layers; k++) write_chunk(&b, chunks[k]);
    SaveJob *j = xcalloc(1, sizeof *j);
    j->cx = col->cx; j->cz = col->cz;
    mutex_lock(S.lock);
    j->seq = ++S.seq;
    Pending p = {col->cx, col->cz, j->seq, b.d, b.n};
    vec_push(S.pending, p);
    mutex_unlock(S.lock);
    S.jobs_inflight++;
    jobs_submit(JOB_KIND_SAVE, 0.0f, save_job_run, save_job_complete, j);
}

bool save_load_column(int cx, int cz, SavedColumn *out) {
    if (!S.open) return false;
    u8 *raw = NULL, *comp = NULL;
    size_t raw_len = 0;
    u32 comp_len = 0;
    mutex_lock(S.lock);
    u64 best = 0;
    for (int i = 0; i < S.pending.n; i++) {
        const Pending *p = &S.pending.d[i];
        if (p->cx != cx || p->cz != cz || p->seq < best) continue;
        best = p->seq;
        free(raw);
        raw = xmalloc(p->raw_len);
        memcpy(raw, p->raw, p->raw_len);
        raw_len = p->raw_len;
    }
    if (!raw) {
        Region *r = region_get(cx >> REGION_SHIFT, cz >> REGION_SHIFT);
        const RegionEntry *e = &r->entries[column_slot(cx, cz)];
        if (r->f && e->comp_size && e->offset >= REGION_DATA_START && (u64)e->offset + e->comp_size <= r->file_end && e->raw_size < (1u << 28)) {
            comp = xmalloc(e->comp_size);
            fseek(r->f, (long)e->offset, SEEK_SET);
            if (fread(comp, 1, e->comp_size, r->f) != e->comp_size) { free(comp); comp = NULL; }
            raw_len = e->raw_size;
            comp_len = e->comp_size;
        }
    }
    mutex_unlock(S.lock);
    if (comp) {
        raw = xmalloc(raw_len);
        int got = LZ4_decompress_safe((const char *)comp, (char *)raw, (int)comp_len, (int)raw_len);
        free(comp);
        if (got != (int)raw_len) { free(raw); raw = NULL; }
    }
    if (!raw) return false;
    bool ok = deserialize_column(raw, raw_len, cx, cz, out);
    free(raw);
    if (!ok) LOGE("Column (%d, %d) in %s/region is corrupt and will be regenerated. Your edits to it are lost; restore the region file from a backup to recover them.", cx, cz, S.dir);
    return ok;
}

int save_jobs_inflight(void) { return S.jobs_inflight; }

void save_close(void) {
    if (!S.open) return;
    world_save_all(); /* a no-op once the world is shut down, which saves everything itself */
    while (S.jobs_inflight > 0) {
        jobs_pump(0.05);
        sleep_ms(1);
    }
    meta_write();
    mutex_lock(S.lock);
    for (int i = 0; i < S.cache_used; i++) region_close(&S.cache[i]);
    mutex_unlock(S.lock);
    block_table_free(&S.names);
    strmap_free(&S.name_index);
    free(S.rt_to_saved);
    vec_free(S.pending);
    mutex_destroy(S.lock);
    memset(&S, 0, sizeof S);
}
