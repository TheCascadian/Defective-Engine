/* Entities: data-driven creatures that walk the voxel world.
 *
 * Decisions:
 *  - An entity is a Player body with a different box size. Walking, swimming, step-up and collision against the
 *    grid are the same code the player uses, so they cannot drift apart and an entity never needs its own physics.
 *  - Types are plain data (data/<namespace>/entities/<id>.json) and are replaced one by one on reload. A file with
 *    a mistake is reported and the previous definition stays, so a live edit never removes a running type.
 *  - Drawing is batched. Visible entities are culled (fog distance, frustum, screen size), sorted nearest first,
 *    capped and given an LOD; every LOD level is one instanced draw. Per-entity state travels in an instance array
 *    uploaded once per frame. With instancing off each entity costs two draws, the original cost, which is also the
 *    baseline the benchmark compares against. LOD and culling never touch the simulation.
 *  - Health, damage, collision and behaviour live in the simulation half of this file. Entities collide with the
 *    world through the shared player body, and with the player and each other in a fixed order (see separate()).
 *  - Handlers may call any entity function while entity_update runs; removals are deferred to the end of the update.
 *  - Unloaded terrain counts as solid for the box test, so an entity beyond the streamed area simply waits. It
 *    never falls out of the world.
 *  - Entities are saved with the world in entities.json when their type says "save". They stay resident while their
 *    column is unloaded, frozen because unloaded terrain is solid, so a chunk reload can never duplicate one. */
#include "dfe.h"

#define TURN_RATE 4.0f
#define IDLE_CHANCE 0.3f
#define WANDER_MIN_S 1.5f
#define WANDER_SPAN_S 2.5f
#define STUCK_JUMP_S 0.25f
#define STUCK_TURN_S 1.0f
#define REFERENCE_SPEED 4.3f      /* the player's walking speed, which type speed multiplies */
#define STUCK_SPEED_FRACTION 0.4f
#define MAX_SIZE 4.0f
#define MAX_SPEED_SCALE 4.0f
#define DEFAULT_WIDTH 0.6f
#define DEFAULT_HEIGHT 0.9f
#define DEFAULT_SPEED 0.4f
#define BODY_FRACTION 0.62f       /* body takes the lower part of the height, the head the rest */
#define HEAD_WIDTH_FRACTION 0.7f
#define HEAD_FORWARD_FRACTION 0.25f

#define INVULNERABLE_S 0.5f
#define MAX_ORPHANS 1024
#define DEFAULT_HOSTILE_SIGHT 16.0f
#define DEFAULT_PASSIVE_SIGHT 8.0f
#define ATTACK_COOLDOWN_S 1.0f
#define ATTACK_REACH 0.6f
#define MAX_HEALTH 100000.0f
#define SAVE_VERSION 1

static float wrap_angle(float a);

typedef struct Entity {
    int id;
    int type;
    Player body;
    float age, ai_timer, heading, stuck, attack_cd, max_health;
    bool moving, removed;
    u8 lod;
    EntityBehaviour behaviour;
    char data[ENTITY_DATA_MAX];
} Entity;

static EntityType g_types[MAX_ENTITY_TYPES];
static int g_type_count;
static Entity g_entities[MAX_ENTITIES];
static int g_entity_count, g_next_id = 1;
static bool g_updating;
static Rng g_rng = {1};
static Json *g_orphans[MAX_ORPHANS]; /* saved entities whose type is not installed; written back untouched */
static int g_orphan_count;

static const char *const BEHAVIOUR_NAMES[ENT_BEHAVE_COUNT] = {"wander", "static", "hostile", "passive"};
const char *entity_behaviour_name(EntityBehaviour b) { return b >= 0 && b < ENT_BEHAVE_COUNT ? BEHAVIOUR_NAMES[b] : "wander"; }
bool entity_behaviour_parse(const char *name, EntityBehaviour *out) {
    for (int i = 0; i < ENT_BEHAVE_COUNT; i++) if (name && !strcmp(name, BEHAVIOUR_NAMES[i])) { *out = (EntityBehaviour)i; return true; }
    return false;
}

/* -------------------------------------------------------------------- types */

int entity_type_count(void) { return g_type_count; }
const EntityType *entity_type_at(int i) { return i >= 0 && i < g_type_count ? &g_types[i] : NULL; }

static int type_index(const char *id) {
    for (int i = 0; i < g_type_count; i++) if (!strcmp(g_types[i].id, id)) return i;
    return -1;
}

static bool read_color(const Json *root, const char *key, float out[3], const char *rel, const char *owner) {
    const Json *a = json_get(root, key);
    if (!a) return true;
    if (a->type != JSON_ARRAY || json_len(a) != 3) { data_error(owner, rel, root->line, "\"%s\" must be three numbers from 0 to 1, such as [0.8, 0.6, 0.4].", key); return false; }
    for (int i = 0; i < 3; i++) {
        out[i] = (float)json_as_num(json_at(a, i), -1.0);
        if (out[i] < 0.0f || out[i] > 1.0f) { data_error(owner, rel, root->line, "\"%s\" components must be from 0 to 1.", key); return false; }
    }
    return true;
}

static bool read_type(EntityType *t, const char *full_id, const char *rel, const char *owner, const Json *root) {
    memset(t, 0, sizeof *t);
    snprintf(t->id, sizeof t->id, "%s", full_id);
    snprintf(t->name, sizeof t->name, "%s", json_str(root, "name", full_id));
    t->width = DEFAULT_WIDTH;
    t->height = DEFAULT_HEIGHT;
    const Json *size = json_get(root, "size");
    if (size) {
        if (size->type != JSON_ARRAY || json_len(size) != 2) { data_error(owner, rel, root->line, "\"size\" must be [width, height] in blocks, such as [0.6, 0.9]."); return false; }
        t->width = (float)json_as_num(json_at(size, 0), 0);
        t->height = (float)json_as_num(json_at(size, 1), 0);
    }
    if (t->width < 0.1f || t->width > MAX_SIZE || t->height < 0.1f || t->height > MAX_SIZE) {
        data_error(owner, rel, root->line, "\"size\" values must be from 0.1 to %.0f blocks.", MAX_SIZE);
        return false;
    }
    t->color[0] = t->color[1] = t->color[2] = 0.7f;
    memcpy(t->accent, t->color, sizeof t->accent);
    if (!read_color(root, "color", t->color, rel, owner)) return false;
    memcpy(t->accent, t->color, sizeof t->accent);
    if (!read_color(root, "accent", t->accent, rel, owner)) return false;
    t->speed = (float)json_num(root, "speed", DEFAULT_SPEED);
    if (t->speed < 0.0f || t->speed > MAX_SPEED_SCALE) { data_error(owner, rel, root->line, "\"speed\" is a multiplier of the player's walking speed, from 0 to %.0f.", MAX_SPEED_SCALE); return false; }
    const Json *bounds = json_get(root, "bounds");
    if (bounds) {
        if (bounds->type != JSON_ARRAY || json_len(bounds) != 3) { data_error(owner, rel, root->line, "\"bounds\" must be [width, height, depth] in blocks, such as [0.8, 1.6, 0.8]."); return false; }
        float w = (float)json_as_num(json_at(bounds, 0), 0), h = (float)json_as_num(json_at(bounds, 1), 0), d = (float)json_as_num(json_at(bounds, 2), 0);
        if (fabsf(w - d) > 1e-4f) { data_error(owner, rel, root->line, "\"bounds\" width and depth must be equal; entity boxes are square."); return false; }
        t->width = w;
        t->height = h;
        if (t->width < 0.1f || t->width > MAX_SIZE || t->height < 0.1f || t->height > MAX_SIZE) { data_error(owner, rel, root->line, "\"bounds\" values must be from 0.1 to %.0f blocks.", MAX_SIZE); return false; }
    }
    const char *model = json_str(root, "model", "box");
    if (strcmp(model, "box")) { data_error(owner, rel, root->line, "\"model\" \"%s\" is not available. Only \"box\" (a body and a head box) exists; use \"color\", \"accent\" and \"size\" to style it.", model); return false; }
    t->wander = json_bool(root, "wander", true);
    t->behaviour = t->wander ? ENT_BEHAVE_WANDER : ENT_BEHAVE_STATIC;
    const Json *bj = json_get(root, "behaviour");
    if (bj) {
        if (bj->type != JSON_STRING || !entity_behaviour_parse(bj->str, &t->behaviour)) { data_error(owner, rel, root->line, "\"behaviour\" must be one of \"wander\", \"static\", \"hostile\", \"passive\"."); return false; }
        t->wander = t->behaviour != ENT_BEHAVE_STATIC;
    }
    t->health = (float)json_num(root, "health", 0.0);
    if (!(t->health >= 0.0f && t->health <= MAX_HEALTH)) { data_error(owner, rel, root->line, "\"health\" must be from 0 to %.0f; 0 means the entity cannot be damaged.", MAX_HEALTH); return false; }
    t->attack = (float)json_num(root, "attack", 1.0);
    if (!(t->attack >= 0.0f && t->attack <= MAX_HEALTH)) { data_error(owner, rel, root->line, "\"attack\" is damage per hit to the player, from 0 to %.0f.", MAX_HEALTH); return false; }
    t->sight = (float)json_num(root, "sight", 0.0);
    if (!(t->sight >= 0.0f && t->sight <= 128.0f)) { data_error(owner, rel, root->line, "\"sight\" is a distance in blocks, from 0 to 128; 0 uses the default for the behaviour."); return false; }
    const char *habitat = json_str(root, "habitat", "any");
    if (!strcmp(habitat, "any")) t->habitat = 0;
    else if (!strcmp(habitat, "land")) t->habitat = 1;
    else if (!strcmp(habitat, "water")) t->habitat = 2;
    else { data_error(owner, rel, root->line, "\"habitat\" must be \"any\", \"land\" or \"water\"."); return false; }
    t->save = json_bool(root, "save", true);
    const Json *drops = json_get(root, "drops");
    if (drops) {
        if (drops->type != JSON_ARRAY || json_len(drops) > ENTITY_MAX_DROPS) { data_error(owner, rel, root->line, "\"drops\" must be a list of at most %d entries like {\"item\": \"base:stone\", \"min\": 1, \"max\": 3}.", ENTITY_MAX_DROPS); return false; }
        for (int i = 0; i < json_len(drops); i++) {
            const Json *d = json_at(drops, i);
            const char *item = d && d->type == JSON_OBJECT ? json_str(d, "item", "") : "";
            if (!item[0] || strlen(item) >= sizeof t->drops[0].item) { data_error(owner, rel, root->line, "drops entry %d needs an \"item\" id such as \"base:stone\".", i + 1); return false; }
            if (!item_find(item) && !block_find(item)) { data_error(owner, rel, root->line, "drops entry %d names \"%s\", which is neither an item nor a block. Check the spelling or install the mod that defines it.", i + 1, item); return false; }
            EntityDrop *dr = &t->drops[t->drop_count++];
            snprintf(dr->item, sizeof dr->item, "%s", item);
            dr->min = json_int(d, "min", 1);
            dr->max = json_int(d, "max", dr->min);
            if (dr->min < 0 || dr->max < dr->min || dr->max > 64) { data_error(owner, rel, root->line, "drops entry %d needs 0 <= min <= max <= 64.", i + 1); return false; }
        }
    }
    t->lifetime = (float)json_num(root, "lifetime", 0.0);
    if (t->lifetime < 0.0f) { data_error(owner, rel, root->line, "\"lifetime\" is in seconds; use 0 to live until removed."); return false; }
    return true;
}

static void store_type(const EntityType *t) {
    int i = type_index(t->id);
    if (i >= 0) g_types[i] = *t;
    else if (g_type_count < MAX_ENTITY_TYPES) g_types[g_type_count++] = *t;
    else LOGW("entity type '%s' ignored: at most %d types are supported", t->id, MAX_ENTITY_TYPES);
}

static void load_type_file(const char *ns, const char *rel, const char *stem) {
    size_t size;
    const char *owner = "?";
    u8 *text = vfs_read(rel, &size, &owner);
    if (!text) return;
    char err[200], full[64];
    int err_line = 0;
    Json *root = json_parse((const char *)text, size, err, sizeof err, &err_line);
    free(text);
    snprintf(full, sizeof full, "%s:%s", ns, stem);
    if (!root) { data_error(owner, rel, err_line, "%s. Fix the JSON syntax at that line.", err); return; }
    EntityType t;
    if (root->type != JSON_OBJECT) data_error(owner, rel, 1, "an entity file must contain one JSON object like {\"size\": [0.6, 0.9], \"color\": [0.8, 0.6, 0.4]}");
    else if (read_type(&t, full, rel, owner, root)) store_type(&t);
    json_free(root);
}

bool entity_type_register_json(const char *full_id, const char *json, char *err, size_t err_size) {
    if (err_size) err[0] = 0;
    if (!full_id || !json || !strchr(full_id, ':') || strlen(full_id) >= sizeof g_types[0].id) {
        snprintf(err, err_size, "the entity id must be namespaced like \"gems:crystal_golem\"");
        return false;
    }
    char perr[160];
    int line = 0;
    Json *root = json_parse(json, strlen(json), perr, sizeof perr, &line);
    if (!root) { snprintf(err, err_size, "%s (line %d)", perr, line); return false; }
    int before = data_error_count();
    EntityType t;
    bool ok = root->type == JSON_OBJECT && read_type(&t, full_id, "(entity definition)", "api", root);
    if (ok) store_type(&t);
    else snprintf(err, err_size, "%s", data_error_count() > before ? data_error_text(data_error_count() - 1) : "an entity definition must be a JSON object");
    json_free(root);
    return ok;
}

int registry_load_entities(void) {
    int errors_before = data_error_count();
    StrList namespaces = {0};
    vfs_list("data", &namespaces);
    for (int n = 0; n < namespaces.n; n++) {
        char dir[160];
        snprintf(dir, sizeof dir, "data/%s/entities", namespaces.d[n]);
        StrList files = {0};
        vfs_list(dir, &files);
        for (int f = 0; f < files.n; f++) {
            size_t len = strlen(files.d[f]);
            if (len < 6 || strcmp(files.d[f] + len - 5, ".json")) continue;
            char rel[260], stem[48];
            snprintf(rel, sizeof rel, "%s/%s", dir, files.d[f]);
            snprintf(stem, sizeof stem, "%.*s", (int)MIN(len - 5, sizeof stem - 1), files.d[f]);
            load_type_file(namespaces.d[n], rel, stem);
        }
        strlist_free(&files);
    }
    strlist_free(&namespaces);
    return data_error_count() - errors_before;
}

/* ---------------------------------------------------------------- lifecycle */

EntityConfig g_entity_cfg = {.instancing = true, .max_drawn = MAX_ENTITIES, .lod1_distance = 32.0f, .lod2_distance = 96.0f, .min_screen = 0.01f};
EntityStats g_entity_stats;

static int find_index(int id) {
    for (int i = 0; i < g_entity_count; i++) if (g_entities[i].id == id && !g_entities[i].removed) return i;
    return -1;
}
static Entity *find(int id) { int i = find_index(id); return i < 0 ? NULL : &g_entities[i]; }

int entity_count(void) {
    int n = 0;
    for (int i = 0; i < g_entity_count; i++) n += !g_entities[i].removed;
    return n;
}

/* Drops removed entries while keeping the order of the rest, so iteration stays in spawn order. */
static void compact(void) {
    int w = 0;
    for (int i = 0; i < g_entity_count; i++) if (!g_entities[i].removed) { if (w != i) g_entities[w] = g_entities[i]; w++; }
    g_entity_count = w;
}

void entity_clear(void) { g_entity_count = 0; }

void entity_world_init(u64 seed) {
    g_rng.s = seed ^ 0xE17177E5ull;
    g_entity_count = 0;
    g_next_id = 1;
    for (int i = 0; i < g_orphan_count; i++) json_free(g_orphans[i]);
    g_orphan_count = 0;
    memset(&g_entity_stats, 0, sizeof g_entity_stats);
}

static void apply_type_to_body(Player *p, const EntityType *t) {
    p->half_width = t->width * 0.5f;
    p->height = t->height;
}

static void init_entity(Entity *e, int id, int ti, V3 pos) {
    memset(e, 0, sizeof *e);
    e->id = id;
    e->type = ti;
    player_init(&e->body, pos);
    apply_type_to_body(&e->body, &g_types[ti]);
    e->body.health = e->max_health = g_types[ti].health;
    e->behaviour = g_types[ti].behaviour;
    e->heading = e->body.yaw = rng_float(&g_rng) * 6.2831853f;
}

int entity_spawn(const char *type_id, V3 pos) {
    int ti = type_id ? type_index(type_id) : -1;
    if (ti < 0) { LOGW("unknown entity type \"%s\". Types are named namespace:file, for example base:hopper; the console command 'entities' lists them.", type_id ? type_id : "(null)"); return 0; }
    if (!isfinite(pos.x) || !isfinite(pos.y) || !isfinite(pos.z)) { LOGW("entity spawn at a non-finite position ignored"); return 0; }
    if (g_types[ti].habitat) {
        /* Water and land creatures are placed where the drainage model says that medium is. */
        GenHydrologySample hy;
        gen_hydrology_at(pos.x, pos.z, &hy);
        bool in_water = hy.wet || (hy.flags & GEN_HYD_OCEAN);
        if (in_water != (g_types[ti].habitat == 2)) { LOGW("entity \"%s\" needs %s and was not spawned at %.0f, %.0f", type_id, g_types[ti].habitat == 2 ? "water" : "dry land", pos.x, pos.z); return 0; }
    }
    if (g_entity_count >= MAX_ENTITIES && !g_updating) compact();
    if (g_entity_count >= MAX_ENTITIES) { LOGW("entity limit of %d reached; remove some before spawning more", MAX_ENTITIES); return 0; }
    dfe_event_t ev = {.name = "entity_spawn", .text = type_id, .entity_id = g_next_id};
    if (event_fire(&ev)) return 0;
    if (g_entity_count >= MAX_ENTITIES) return 0; /* a handler may have spawned entities itself */
    Entity *e = &g_entities[g_entity_count++];
    init_entity(e, g_next_id++, ti, pos);
    return e->id;
}

/* Removal fires entity_despawn. A handler can cancel it when cancellable is set. Inside an update the entry is only
 * marked, so loops that hold indices stay valid; it is dropped when the update ends. */
static bool despawn(Entity *e, const char *reason, bool cancellable) {
    if (e->removed) return false;
    dfe_event_t ev = {.name = "entity_despawn", .text = reason, .entity_id = e->id};
    if (event_fire(&ev) && cancellable) return false;
    e->removed = true;
    if (!g_updating) compact();
    return true;
}

bool entity_remove(int id) {
    Entity *e = find(id);
    return e && despawn(e, "removed", true);
}

bool entity_position(int id, V3 *out) {
    Entity *e = find(id);
    if (!e) return false;
    *out = e->body.pos;
    return true;
}

bool entity_get(int id, EntityInfo *out) {
    Entity *e = find(id);
    if (!e) return false;
    memset(out, 0, sizeof *out);
    out->id = e->id;
    snprintf(out->type, sizeof out->type, "%s", g_types[e->type].id);
    out->pos = e->body.pos;
    out->vel = e->body.vel;
    out->yaw = e->body.yaw;
    out->health = e->body.health;
    out->max_health = e->max_health;
    out->age = e->age;
    out->behaviour = e->behaviour;
    snprintf(out->data, sizeof out->data, "%s", e->data);
    return true;
}

bool entity_set_position(int id, V3 feet) {
    Entity *e = find(id);
    return e && player_teleport(&e->body, feet);
}

bool entity_set_velocity(int id, V3 vel) {
    Entity *e = find(id);
    if (!e || !isfinite(vel.x) || !isfinite(vel.y) || !isfinite(vel.z)) return false;
    float lim = 100.0f;
    e->body.vel = v3(CLAMP(vel.x, -lim, lim), CLAMP(vel.y, -lim, lim), CLAMP(vel.z, -lim, lim));
    return true;
}

bool entity_set_yaw(int id, float yaw) {
    Entity *e = find(id);
    if (!e || !isfinite(yaw)) return false;
    e->body.yaw = e->heading = wrap_angle(yaw);
    return true;
}

bool entity_set_behaviour(int id, EntityBehaviour b) {
    Entity *e = find(id);
    if (!e || b < 0 || b >= ENT_BEHAVE_COUNT) return false;
    e->behaviour = b;
    return true;
}

bool entity_set_data(int id, const char *text) {
    Entity *e = find(id);
    if (!e || !text || strlen(text) >= sizeof e->data) return false;
    snprintf(e->data, sizeof e->data, "%s", text);
    return true;
}

/* ------------------------------------------------------------------- health */

static void roll_drops(const EntityType *t) {
    for (int i = 0; i < t->drop_count; i++) {
        const EntityDrop *d = &t->drops[i];
        int n = d->min + (int)(rng_float(&g_rng) * (float)(d->max - d->min + 1));
        n = CLAMP(n, d->min, d->max);
        if (n > 0) inventory_add_item(&g_inv, d->item, n);
    }
}

static void entity_death(Entity *e, const DamageSource *src) {
    dfe_event_t ev = {.name = "entity_death", .text = g_types[e->type].id, .entity_id = e->id};
    if (event_fire(&ev)) { e->body.health = MIN(1.0f, e->max_health); return; } /* cancelled: it survives on a sliver */
    if (src && src->kind == DAMAGE_PLAYER) roll_drops(&g_types[e->type]);
    despawn(e, "death", false);
}

int entity_raycast(V3 o, V3 d, float max_dist, float *dist) {
    int best = 0;
    float best_t = max_dist;
    for (int i = 0; i < g_entity_count; i++) {
        const Entity *e = &g_entities[i];
        if (e->removed) continue;
        float lo[3] = {e->body.pos.x - e->body.half_width, e->body.pos.y, e->body.pos.z - e->body.half_width};
        float hi[3] = {e->body.pos.x + e->body.half_width, e->body.pos.y + e->body.height, e->body.pos.z + e->body.half_width};
        float oo[3] = {o.x, o.y, o.z}, dd[3] = {d.x, d.y, d.z};
        float t0 = 0.0f, t1 = best_t;
        bool ok = true;
        for (int a = 0; a < 3 && ok; a++) {
            if (fabsf(dd[a]) < 1e-8f) { ok = oo[a] >= lo[a] && oo[a] <= hi[a]; continue; }
            float ta = (lo[a] - oo[a]) / dd[a], tb = (hi[a] - oo[a]) / dd[a];
            if (ta > tb) { float tmp = ta; ta = tb; tb = tmp; }
            t0 = MAX(t0, ta); t1 = MIN(t1, tb);
            ok = t0 <= t1;
        }
        if (ok && t0 <= best_t) { best_t = t0; best = e->id; }
    }
    if (best && dist) *dist = best_t;
    return best;
}

bool entity_damage(int id, float amount, const DamageSource *src) {
    Entity *e = find(id);
    if (!e || !(amount > 0.0f) || !isfinite(amount) || g_types[e->type].health <= 0.0f) return false;
    if (e->body.invulnerability_timer > 0.0f) return false;
    dfe_event_t ev = {.name = "entity_damage", .text = g_types[e->type].id, .entity_id = id, .damage = amount};
    if (event_fire(&ev)) return false;
    e->body.health = MAX(e->body.health - amount, 0.0f);
    e->body.hurt_timer = 0.3f;
    e->body.invulnerability_timer = INVULNERABLE_S;
    if (e->body.health <= 0.0f) entity_death(e, src);
    return true;
}

bool entity_heal(int id, float amount) {
    Entity *e = find(id);
    if (!e || !(amount > 0.0f) || !isfinite(amount) || g_types[e->type].health <= 0.0f || e->body.health >= e->max_health) return false;
    e->body.health = MIN(e->body.health + amount, e->max_health);
    return true;
}

bool entity_set_health(int id, float health, float max_health) {
    Entity *e = find(id);
    if (!e || !isfinite(health) || !isfinite(max_health) || g_types[e->type].health <= 0.0f) return false;
    if (max_health >= 0.0f) e->max_health = CLAMP(max_health, 1.0f, MAX_HEALTH);
    e->body.health = MIN(health, e->max_health);
    if (e->body.health <= 0.0f) { DamageSource s = {.kind = DAMAGE_MOD}; entity_death(e, &s); }
    return true;
}

/* ------------------------------------------------------------------ queries */

int entity_list(int *out, int cap) {
    int n = 0;
    for (int i = 0; i < g_entity_count; i++) {
        if (g_entities[i].removed) continue;
        if (out && n < cap) out[n] = g_entities[i].id;
        n++;
    }
    return n;
}

typedef struct Near { float d2; int id; } Near;
static int near_cmp(const void *a, const void *b) {
    const Near *x = a, *y = b;
    if (x->d2 != y->d2) return x->d2 < y->d2 ? -1 : 1;
    return (x->id > y->id) - (x->id < y->id);
}

int entity_near(V3 pos, float radius, int *out, int cap) {
    Near hits[MAX_ENTITIES];
    int n = 0;
    if (!(radius >= 0.0f) || !isfinite(radius)) return 0;
    for (int i = 0; i < g_entity_count; i++) {
        const Entity *e = &g_entities[i];
        if (e->removed) continue;
        V3 d = v3_sub(e->body.pos, pos);
        float d2 = d.x * d.x + d.y * d.y + d.z * d.z;
        if (d2 <= radius * radius) hits[n++] = (Near){d2, e->id};
    }
    qsort(hits, (size_t)n, sizeof hits[0], near_cmp);
    for (int i = 0; i < n && out && i < cap; i++) out[i] = hits[i].id;
    return n;
}

/* -------------------------------------------------------------------- update */

static float wrap_angle(float a) {
    while (a > (float)M_PI) a -= 2.0f * (float)M_PI;
    while (a < -(float)M_PI) a += 2.0f * (float)M_PI;
    return a;
}

static void pick_new_heading(Entity *e) {
    e->moving = rng_float(&g_rng) >= IDLE_CHANCE;
    e->heading = rng_float(&g_rng) * 2.0f * (float)M_PI;
    e->ai_timer = WANDER_MIN_S + rng_float(&g_rng) * WANDER_SPAN_S;
}

/* Turns toward e->heading, walks when moving, hops over what blocks the way and swims up out of water. A wanderer
 * that stays blocked picks a new heading; a chaser keeps its target direction and only hops. */
static void walk(Entity *e, const EntityType *t, PlayerInput *in, float dt, bool retarget) {
    e->body.yaw += CLAMP(wrap_angle(e->heading - e->body.yaw), -TURN_RATE * dt, TURN_RATE * dt);
    in->forward = e->moving ? 1.0f : 0.0f;
    float speed = hypotf(e->body.vel.x, e->body.vel.z);
    bool blocked = e->moving && e->body.on_ground && speed < t->speed * REFERENCE_SPEED * STUCK_SPEED_FRACTION;
    e->stuck = blocked ? e->stuck + dt : 0.0f;
    in->jump = e->body.in_water || e->stuck > STUCK_JUMP_S;
    if (retarget && e->stuck > STUCK_TURN_S) { pick_new_heading(e); e->stuck = 0.0f; }
}

/* Yaw that looks along (dx, dz); yaw 0 looks down -Z. */
static float yaw_toward(float dx, float dz) { return atan2f(-dx, -dz); }

static PlayerInput think(Entity *e, const EntityType *t, float dt) {
    PlayerInput in = {.speed_scale = t->speed};
    if (e->behaviour == ENT_BEHAVE_STATIC) return in;
    e->attack_cd = MAX(e->attack_cd - dt, 0.0f);
    if (e->behaviour == ENT_BEHAVE_HOSTILE || e->behaviour == ENT_BEHAVE_PASSIVE) {
        float sight = t->sight > 0.0f ? t->sight : (e->behaviour == ENT_BEHAVE_HOSTILE ? DEFAULT_HOSTILE_SIGHT : DEFAULT_PASSIVE_SIGHT);
        float dx = g_player.pos.x - e->body.pos.x, dz = g_player.pos.z - e->body.pos.z, dy = g_player.pos.y - e->body.pos.y;
        float dist = hypotf(dx, dz);
        if (!g_player.dead && dist < sight && fabsf(dy) < sight) {
            if (e->behaviour == ENT_BEHAVE_HOSTILE) {
                float reach = e->body.half_width + g_player.half_width + ATTACK_REACH;
                e->heading = yaw_toward(dx, dz);
                e->moving = dist > reach * 0.8f;
                walk(e, t, &in, dt, false);
                if (dist <= reach && fabsf(dy) < MAX(e->body.height, g_player.height) && e->attack_cd <= 0.0f && t->attack > 0.0f) {
                    player_hurt(&g_player, t->attack);
                    e->attack_cd = ATTACK_COOLDOWN_S;
                }
            } else {
                e->heading = yaw_toward(-dx, -dz);
                e->moving = true;
                walk(e, t, &in, dt, false);
            }
            return in;
        }
    }
    if (!t->wander && e->behaviour == ENT_BEHAVE_WANDER) return in;
    e->ai_timer -= dt;
    if (e->ai_timer <= 0.0f) pick_new_heading(e);
    walk(e, t, &in, dt, true);
    return in;
}

/* Collision between bodies. Order is fixed so a run replays exactly: every entity is first pushed out of the player,
 * then pairs are resolved in index (spawn) order with i < j. The smaller horizontal overlap decides the axis and ties
 * go to x; a pair splits the push evenly, a static entity or a push into a wall gives the whole push to the other
 * side. Vertical overlap is required, so an entity standing on another is not shoved. */
static bool overlap_xz(V3 a, float ahw, float ah, V3 b, float bhw, float bh, float *ox, float *oz) {
    if (a.y >= b.y + bh || b.y >= a.y + ah) return false;
    *ox = ahw + bhw - fabsf(a.x - b.x);
    *oz = ahw + bhw - fabsf(a.z - b.z);
    return *ox > 0.0f && *oz > 0.0f;
}

static bool nudge(Entity *e, float dx, float dz) {
    V3 to = v3(e->body.pos.x + dx, e->body.pos.y, e->body.pos.z + dz);
    if (box_blocked(to, e->body.half_width, e->body.height)) return false;
    e->body.pos = to;
    return true;
}

/* Pushes `e` by a fraction of the overlap along the axis of least overlap; if a wall is there the other axis is tried. */
static bool push(Entity *e, float ox, float oz, float sx, float sz, float fraction) {
    const float margin = 0.001f;
    bool x_first = ox <= oz;
    for (int k = 0; k < 2; k++) {
        bool use_x = (k == 0) == x_first;
        if (nudge(e, use_x ? sx * (ox + margin) * fraction : 0.0f, use_x ? 0.0f : sz * (oz + margin) * fraction)) return true;
    }
    return false;
}

static void separate(void) {
    for (int i = 0; i < g_entity_count; i++) {
        Entity *e = &g_entities[i];
        if (e->removed || g_player.dead) continue;
        float ox, oz;
        if (!overlap_xz(e->body.pos, e->body.half_width, e->body.height, g_player.pos, g_player.half_width, g_player.height, &ox, &oz)) continue;
        float sx = e->body.pos.x < g_player.pos.x ? -1.0f : 1.0f, sz = e->body.pos.z < g_player.pos.z ? -1.0f : 1.0f;
        push(e, ox, oz, sx, sz, 1.0f);
    }
    for (int i = 0; i < g_entity_count; i++) {
        Entity *a = &g_entities[i];
        if (a->removed) continue;
        for (int j = i + 1; j < g_entity_count; j++) {
            Entity *b = &g_entities[j];
            if (b->removed) continue;
            float ox, oz;
            if (!overlap_xz(a->body.pos, a->body.half_width, a->body.height, b->body.pos, b->body.half_width, b->body.height, &ox, &oz)) continue;
            /* a is on the negative side unless it is strictly greater; exact ties send the lower index negative. */
            float sx = a->body.pos.x > b->body.pos.x ? 1.0f : -1.0f, sz = a->body.pos.z > b->body.pos.z ? 1.0f : -1.0f;
            bool a_fixed = a->behaviour == ENT_BEHAVE_STATIC, b_fixed = b->behaviour == ENT_BEHAVE_STATIC;
            if (a_fixed && b_fixed) continue;
            float fa = a_fixed ? 0.0f : (b_fixed ? 1.0f : 0.5f);
            float fb = 1.0f - fa;
            bool moved_a = fa > 0.0f && push(a, ox, oz, sx, sz, fa);
            bool moved_b = fb > 0.0f && push(b, ox, oz, -sx, -sz, fb);
            if (fa > 0.0f && fb > 0.0f && moved_a != moved_b) {
                /* One side was walled in: give the other the share it could not take. */
                if (moved_a) push(a, ox, oz, sx, sz, 0.5f); else push(b, ox, oz, -sx, -sz, 0.5f);
            }
        }
    }
}

void entity_update(float dt) {
    g_updating = true;
    int n = g_entity_count; /* entities spawned by handlers start ticking next update */
    for (int i = 0; i < n; i++) {
        Entity *e = &g_entities[i];
        if (e->removed) continue;
        const EntityType *t = &g_types[e->type];
        e->age += dt;
        if (t->lifetime > 0.0f && e->age >= t->lifetime) {
            if (!despawn(e, "lifetime", true)) e->age = 0.0f; /* cancelled: the lifetime starts over */
            continue;
        }
        dfe_event_t tick = {.name = "entity_tick", .text = t->id, .entity_id = e->id, .dt = dt};
        if (event_fire(&tick)) continue; /* cancelled: this entity does nothing this update */
        if (e->removed) continue;
        PlayerInput in = think(e, t, dt);
        player_step(&e->body, &in, dt);
        /* Anything that ends up below the world is removed rather than simulated forever. */
        if (e->body.pos.y < -64.0f) despawn(e, "void", false);
    }
    separate();
    g_updating = false;
    compact();
}

/* -------------------------------------------------------------- persistence */

static void write_entity(JsonWriter *w, const Entity *e) {
    jw_begin_obj(w);
    jw_key(w, "id"); jw_num(w, e->id);
    jw_key(w, "type"); jw_str(w, g_types[e->type].id);
    jw_key(w, "x"); jw_num(w, e->body.pos.x);
    jw_key(w, "y"); jw_num(w, e->body.pos.y);
    jw_key(w, "z"); jw_num(w, e->body.pos.z);
    jw_key(w, "vx"); jw_num(w, e->body.vel.x);
    jw_key(w, "vy"); jw_num(w, e->body.vel.y);
    jw_key(w, "vz"); jw_num(w, e->body.vel.z);
    jw_key(w, "yaw"); jw_num(w, e->body.yaw);
    jw_key(w, "health"); jw_num(w, e->body.health);
    jw_key(w, "max_health"); jw_num(w, e->max_health);
    jw_key(w, "age"); jw_num(w, e->age);
    jw_key(w, "behaviour"); jw_str(w, entity_behaviour_name(e->behaviour));
    jw_key(w, "heading"); jw_num(w, e->heading);
    jw_key(w, "ai_timer"); jw_num(w, e->ai_timer);
    jw_key(w, "moving"); jw_bool(w, e->moving);
    jw_key(w, "data"); jw_str(w, e->data);
    /* The column the entity stands in. It is informational: entities stay resident when their column unloads. */
    jw_key(w, "cx"); jw_num(w, floorf(e->body.pos.x / CHUNK_SIZE));
    jw_key(w, "cz"); jw_num(w, floorf(e->body.pos.z / CHUNK_SIZE));
    jw_end_obj(w);
}

bool entity_save(void) {
    if (!save_active()) return false;
    JsonWriter w = {0};
    jw_begin_obj(&w);
    jw_key(&w, "version"); jw_num(&w, SAVE_VERSION);
    jw_key(&w, "next_id"); jw_num(&w, g_next_id);
    jw_key(&w, "entities");
    jw_begin_arr(&w);
    for (int i = 0; i < g_entity_count; i++) if (!g_entities[i].removed && g_types[g_entities[i].type].save) write_entity(&w, &g_entities[i]);
    for (int i = 0; i < g_orphan_count; i++) json_write(&w, g_orphans[i]);
    jw_end_arr(&w);
    jw_end_obj(&w);
    bool ok = save_write_entities(w.buf, w.len);
    jw_free(&w);
    return ok;
}

static bool restore_one(const Json *r) {
    const char *type = json_str(r, "type", "");
    int id = json_int(r, "id", 0);
    int ti = type_index(type);
    if (ti < 0) {
        if (g_orphan_count < MAX_ORPHANS) g_orphans[g_orphan_count++] = json_clone(r);
        LOGW("saved entity %d has type \"%s\", which is not installed. It is kept in the save and returns when the type does.", id, type);
        return false;
    }
    if (id <= 0 || find(id)) return false; /* a duplicate id is never loaded twice */
    if (g_entity_count >= MAX_ENTITIES) { LOGW("saved entity %d skipped: the limit of %d entities is reached", id, MAX_ENTITIES); return false; }
    V3 pos = v3((float)json_num(r, "x", 0), (float)json_num(r, "y", 0), (float)json_num(r, "z", 0));
    if (!isfinite(pos.x) || !isfinite(pos.y) || !isfinite(pos.z)) return false;
    Entity *e = &g_entities[g_entity_count++];
    init_entity(e, id, ti, pos);
    e->body.vel = v3((float)json_num(r, "vx", 0), (float)json_num(r, "vy", 0), (float)json_num(r, "vz", 0));
    e->body.yaw = (float)json_num(r, "yaw", e->body.yaw);
    e->max_health = (float)json_num(r, "max_health", e->max_health);
    e->body.health = (float)json_num(r, "health", e->max_health);
    if (g_types[ti].health > 0.0f) {
        if (!(e->max_health >= 1.0f && e->max_health <= MAX_HEALTH)) e->max_health = g_types[ti].health;
        e->body.health = CLAMP(e->body.health, 0.0f, e->max_health);
        if (!isfinite(e->body.health)) e->body.health = e->max_health;
    }
    e->age = (float)json_num(r, "age", 0);
    EntityBehaviour b;
    if (entity_behaviour_parse(json_str(r, "behaviour", ""), &b)) e->behaviour = b;
    e->heading = (float)json_num(r, "heading", e->heading);
    e->ai_timer = (float)json_num(r, "ai_timer", 0);
    e->moving = json_bool(r, "moving", false);
    snprintf(e->data, sizeof e->data, "%s", json_str(r, "data", ""));
    if (id >= g_next_id) g_next_id = id + 1;
    return true;
}

int entity_load(void) {
    size_t size = 0;
    u8 *text = save_read_entities(&size);
    if (!text) return 0; /* a world saved before entities were persisted has no file, which is fine */
    char err[160];
    int line = 0;
    Json *root = json_parse((const char *)text, size, err, sizeof err, &line);
    free(text);
    if (!root || root->type != JSON_OBJECT) {
        LOGW("entities.json could not be read (%s, line %d); the world loads without saved entities and the file is left as it is.", root ? "not an object" : err, line);
        json_free(root);
        return 0;
    }
    const Json *list = json_get(root, "entities");
    int restored = 0;
    for (int i = 0; list && i < json_len(list); i++) if (restore_one(json_at(list, i))) restored++;
    int next = json_int(root, "next_id", 1);
    if (next > g_next_id) g_next_id = next;
    json_free(root);
    return restored;
}

/* ------------------------------------------------------------------ drawing */

static const float CUBE_FACES[6][4][3] = {
    {{0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}},     /* +X */
    {{-0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, -0.5f}}, /* -X */
    {{-0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}},     /* +Y */
    {{-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, 0.5f}, {-0.5f, -0.5f, 0.5f}}, /* -Y */
    {{-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}},     /* +Z */
    {{0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}}, /* -Z */
};
static const float CUBE_NORMALS[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
#define CUBE_VERTS 36
/* One vertex buffer holds all three meshes: body+head (part 0, 1), one merged box (2), an impostor quad (3). */
#define MESH_FLOATS 7
#define LOD0_FIRST 0
#define LOD0_VERTS (CUBE_VERTS * 2)
#define LOD1_FIRST LOD0_VERTS
#define LOD2_FIRST (LOD0_VERTS + CUBE_VERTS)
#define LOD2_VERTS 6
#define TOTAL_VERTS (LOD0_VERTS + CUBE_VERTS + LOD2_VERTS)
#define LOD_HYSTERESIS 0.9f

static struct { Shader sh; GLuint vao, vbo, ibo; bool ready; } G;

static void build_cube(float *out, float part) {
    static const int QUAD[6] = {0, 1, 2, 0, 2, 3};
    int n = 0;
    for (int f = 0; f < 6; f++)
        for (int k = 0; k < 6; k++) {
            memcpy(out + n * MESH_FLOATS, CUBE_FACES[f][QUAD[k]], 3 * sizeof(float));
            memcpy(out + n * MESH_FLOATS + 3, CUBE_NORMALS[f], 3 * sizeof(float));
            out[n * MESH_FLOATS + 6] = part;
            n++;
        }
}

static void build_meshes(float *out) {
    build_cube(out, 0.0f);
    build_cube(out + CUBE_VERTS * MESH_FLOATS, 1.0f);
    build_cube(out + LOD0_VERTS * MESH_FLOATS, 2.0f);
    static const float Q[6][2] = {{-0.5f, 0}, {0.5f, 0}, {0.5f, 1}, {-0.5f, 0}, {0.5f, 1}, {-0.5f, 1}};
    float *q = out + (LOD0_VERTS + CUBE_VERTS) * MESH_FLOATS;
    for (int i = 0; i < 6; i++) {
        float *v = q + i * MESH_FLOATS;
        v[0] = Q[i][0]; v[1] = Q[i][1]; v[2] = 0; v[3] = v[4] = 0; v[5] = 1; v[6] = 3.0f;
    }
}

bool entity_gl_init(void) {
    static float mesh[TOTAL_VERTS * MESH_FLOATS];
    build_meshes(mesh);
    glGenVertexArrays(1, &G.vao);
    glGenBuffers(1, &G.vbo);
    glGenBuffers(1, &G.ibo);
    glBindVertexArray(G.vao);
    glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof mesh, mesh, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, MESH_FLOATS * sizeof(float), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, MESH_FLOATS * sizeof(float), (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, MESH_FLOATS * sizeof(float), (void *)(6 * sizeof(float)));
    glBindVertexArray(0);
    G.ready = entity_reload_shaders();
    return G.ready;
}

bool entity_reload_shaders(void) {
    Shader fresh = {0};
    if (!shader_load(&fresh, "entity", "assets/dfe/shaders/entity.vert", "assets/dfe/shaders/entity.frag", "")) return false;
    if (G.sh.program) shader_destroy(&G.sh);
    G.sh = fresh;
    return true;
}

void entity_gl_shutdown(void) {
    if (G.sh.program) shader_destroy(&G.sh);
    if (G.vbo) glDeleteBuffers(1, &G.vbo);
    if (G.ibo) glDeleteBuffers(1, &G.ibo);
    if (G.vao) glDeleteVertexArrays(1, &G.vao);
    memset(&G, 0, sizeof G);
}

static void sample_light(const Entity *e, float out[4]) {
    V3 c = v3(e->body.pos.x, e->body.pos.y + e->body.height * 0.5f, e->body.pos.z);
    out[0] = 1.0f; out[1] = out[2] = out[3] = 0.0f;
    if (world_get_state(ifloor(c.x), ifloor(c.y), ifloor(c.z)) != STATE_UNLOADED) {
        u16 l = world_get_light(ifloor(c.x), ifloor(c.y), ifloor(c.z));
        out[0] = (float)LIGHT_SKY(l) / 15.0f; out[1] = (float)LIGHT_R(l) / 15.0f; out[2] = (float)LIGHT_G(l) / 15.0f; out[3] = (float)LIGHT_B(l) / 15.0f;
    }
}

typedef struct Cand { int index, id; float d2; } Cand;
static int cand_cmp(const void *a, const void *b) {
    const Cand *x = a, *y = b;
    if (x->d2 != y->d2) return x->d2 < y->d2 ? -1 : 1;
    return (x->id > y->id) - (x->id < y->id); /* equal distance falls back on the handle, so the order is stable */
}

static EntityInstance g_instances[MAX_ENTITIES];

int entity_build_batches(const Camera *cam, float fog_end, EntityInstance **out, int lod_counts[ENTITY_LOD_LEVELS]) {
    memset(&g_entity_stats, 0, sizeof g_entity_stats);
    for (int l = 0; l < ENTITY_LOD_LEVELS; l++) lod_counts[l] = 0;
    Cand cands[MAX_ENTITIES];
    int nc = 0, total = 0;
    float proj_scale = cam->proj.m[5] > 0.0f ? cam->proj.m[5] : 1.0f;
    for (int i = 0; i < g_entity_count; i++) {
        const Entity *e = &g_entities[i];
        if (e->removed) continue;
        total++;
        float w = e->body.half_width, h = e->body.height;
        float radius = MAX(w * 1.4143f, h * 0.5f + w);
        V3 rel = v3_sub(e->body.pos, cam->pos);
        float d2 = rel.x * rel.x + rel.y * rel.y + rel.z * rel.z;
        float reach = fog_end + radius;
        if (d2 > reach * reach) continue;
        V3 lo = v3(e->body.pos.x - w, e->body.pos.y, e->body.pos.z - w), hi = v3(e->body.pos.x + w, e->body.pos.y + h, e->body.pos.z + w);
        if (!frustum_box_visible(&cam->frustum, lo, hi)) continue;
        float dist = sqrtf(d2);
        if (dist > radius && radius * proj_scale / dist < g_entity_cfg.min_screen) continue;
        cands[nc++] = (Cand){i, e->id, d2};
    }
    qsort(cands, (size_t)nc, sizeof cands[0], cand_cmp);
    int cap = CLAMP(g_entity_cfg.max_drawn, 0, MAX_ENTITIES);
    int drawn = MIN(nc, cap);
    float lod1 = g_entity_cfg.lod1_distance, lod2 = MAX(g_entity_cfg.lod2_distance, lod1);
    int level[MAX_ENTITIES];
    for (int c = 0; c < drawn; c++) {
        Entity *e = &g_entities[cands[c].index];
        float d = sqrtf(cands[c].d2);
        int want = d > lod2 ? 2 : d > lod1 ? 1 : 0;
        /* Moving to a finer level needs a margin, so an entity at a threshold does not flicker between two. */
        if (want < e->lod && d > (e->lod == 2 ? lod2 : lod1) * LOD_HYSTERESIS) want = e->lod;
        e->lod = (u8)want;
        level[c] = want;
        lod_counts[want]++;
    }
    int start[ENTITY_LOD_LEVELS] = {0, lod_counts[0], lod_counts[0] + lod_counts[1]}, fill[ENTITY_LOD_LEVELS] = {0};
    for (int c = 0; c < drawn; c++) {
        const Entity *e = &g_entities[cands[c].index];
        const EntityType *t = &g_types[e->type];
        EntityInstance *in = &g_instances[start[level[c]] + fill[level[c]]++];
        V3 rel = v3_sub(e->body.pos, cam->pos);
        in->origin[0] = rel.x; in->origin[1] = rel.y; in->origin[2] = rel.z;
        in->yaw_sc[0] = sinf(e->body.yaw); in->yaw_sc[1] = cosf(e->body.yaw);
        in->size[0] = t->width; in->size[1] = t->height;
        memcpy(in->color, t->color, sizeof in->color);
        memcpy(in->accent, t->accent, sizeof in->accent);
        sample_light(e, in->light);
    }
    g_entity_stats.drawn = drawn;
    g_entity_stats.culled = total - drawn;
    g_entity_stats.instances = drawn;
    for (int l = 0; l < ENTITY_LOD_LEVELS; l++) g_entity_stats.lod[l] = lod_counts[l];
    *out = g_instances;
    return drawn;
}

int entity_draw_call_count(const int lod_counts[ENTITY_LOD_LEVELS], bool instanced) {
    int calls = 0;
    for (int l = 0; l < ENTITY_LOD_LEVELS; l++) calls += instanced ? (lod_counts[l] > 0) : lod_counts[l] * (l == 0 ? 2 : 1);
    return calls;
}

static void bind_instances(size_t first) {
    const size_t stride = sizeof(EntityInstance);
    size_t base = first * stride;
    static const struct { int size; size_t off; } A[6] = {
        {3, offsetof(EntityInstance, origin)}, {2, offsetof(EntityInstance, yaw_sc)}, {2, offsetof(EntityInstance, size)},
        {3, offsetof(EntityInstance, color)}, {3, offsetof(EntityInstance, accent)}, {4, offsetof(EntityInstance, light)}};
    for (int i = 0; i < 6; i++) {
        glEnableVertexAttribArray(3 + i);
        glVertexAttribPointer(3 + i, A[i].size, GL_FLOAT, GL_FALSE, (GLsizei)stride, (void *)(base + A[i].off));
        glVertexAttribDivisor(3 + i, 1);
    }
}

static void set_constant_instance(const EntityInstance *in) {
    glVertexAttrib3fv(3, in->origin);
    glVertexAttrib2fv(4, in->yaw_sc);
    glVertexAttrib2fv(5, in->size);
    glVertexAttrib3fv(6, in->color);
    glVertexAttrib3fv(7, in->accent);
    glVertexAttrib4fv(8, in->light);
}

int entity_draw(const Camera *cam, float fog_start, float fog_end) {
    if (!G.ready) return 0;
    EntityInstance *inst;
    int lod_n[ENTITY_LOD_LEVELS];
    int n = entity_build_batches(cam, fog_end, &inst, lod_n);
    if (!n) return 0;
    shader_use(&G.sh);
    M4 rot_view = m4_look_dir(v3(0, 0, 0), cam->forward, v3(0, 1, 0));
    M4 vp = m4_mul(cam->proj, rot_view);
    glUniformMatrix4fv(shader_uniform(&G.sh, "u_viewproj"), 1, GL_FALSE, vp.m);
    glUniform3f(shader_uniform(&G.sh, "u_cam_right"), cam->right.x, cam->right.y, cam->right.z);
    glUniform3f(shader_uniform(&G.sh, "u_cam_forward"), cam->forward.x, cam->forward.y, cam->forward.z);
    atmosphere_set_uniforms(&G.sh);
    glUniform1f(shader_uniform(&G.sh, "u_fog_start"), fog_start);
    glUniform1f(shader_uniform(&G.sh, "u_fog_end"), fog_end);
    glBindVertexArray(G.vao);
    static const int FIRST[ENTITY_LOD_LEVELS] = {LOD0_FIRST, LOD1_FIRST, LOD2_FIRST};
    static const int VERTS[ENTITY_LOD_LEVELS] = {LOD0_VERTS, CUBE_VERTS, LOD2_VERTS};
    int calls = 0;
    if (g_entity_cfg.instancing) {
        glBindBuffer(GL_ARRAY_BUFFER, G.ibo);
        size_t bytes = (size_t)n * sizeof(EntityInstance);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(MAX_ENTITIES * sizeof(EntityInstance)), NULL, GL_STREAM_DRAW); /* orphan the old store */
        glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)bytes, inst);
        g_entity_stats.upload_bytes = (u32)bytes;
        size_t first = 0;
        for (int l = 0; l < ENTITY_LOD_LEVELS; l++) {
            if (!lod_n[l]) continue;
            bind_instances(first);
            glDrawArraysInstanced(GL_TRIANGLES, FIRST[l], VERTS[l], lod_n[l]);
            first += (size_t)lod_n[l];
            calls++;
        }
    } else {
        /* Fallback: the same instance data as constant attributes, one entity at a time. A full-detail entity is two
         * draws, a body and a head, which is what the engine did before batching. */
        for (int i = 3; i < 9; i++) { glDisableVertexAttribArray(i); glVertexAttribDivisor(i, 0); }
        int idx = 0;
        for (int l = 0; l < ENTITY_LOD_LEVELS; l++)
            for (int k = 0; k < lod_n[l]; k++, idx++) {
                set_constant_instance(&inst[idx]);
                if (l == 0) {
                    glDrawArrays(GL_TRIANGLES, FIRST[0], CUBE_VERTS);
                    glDrawArrays(GL_TRIANGLES, FIRST[0] + CUBE_VERTS, CUBE_VERTS);
                    calls += 2;
                } else {
                    glDrawArrays(GL_TRIANGLES, FIRST[l], VERTS[l]);
                    calls++;
                }
            }
    }
    glBindVertexArray(0);
    g_entity_stats.draw_calls = calls;
    return calls;
}
