/* Entities: data-driven creatures that walk the voxel world.
 *
 * Decisions:
 *  - An entity is a Player body with a different box size. Walking, swimming, step-up and collision against the
 *    grid are the same code the player uses, so they cannot drift apart and an entity never needs its own physics.
 *  - Types are plain data (data/<namespace>/entities/<id>.json) and are replaced one by one on reload. A file with
 *    a mistake is reported and the previous definition stays, so a live edit never removes a running type.
 *  - Drawing is two boxes per entity through one small shader. Entities are few and large, so the cost is draw
 *    calls, not vertices, and a distance cull keeps those to what the fog leaves visible.
 *  - Unloaded terrain counts as solid for the box test, so an entity beyond the streamed area simply waits. It
 *    never falls out of the world.
 *  - Entities are not saved. They are mod-spawned and transient for now; see the limitations in docs/MODDING.md. */
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

typedef struct Entity {
    int id;
    int type;
    Player body;
    float age, ai_timer, heading, stuck;
    bool moving;
} Entity;

static EntityType g_types[MAX_ENTITY_TYPES];
static int g_type_count;
static Entity g_entities[MAX_ENTITIES];
static int g_entity_count, g_next_id = 1;
static Rng g_rng = {1};

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
    t->wander = json_bool(root, "wander", true);
    t->lifetime = (float)json_num(root, "lifetime", 0.0);
    if (t->lifetime < 0.0f) { data_error(owner, rel, root->line, "\"lifetime\" is in seconds; use 0 to live until removed."); return false; }
    return true;
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
    else if (read_type(&t, full, rel, owner, root)) {
        int i = type_index(full);
        if (i >= 0) g_types[i] = t;
        else if (g_type_count < MAX_ENTITY_TYPES) g_types[g_type_count++] = t;
        else LOGW("entity type '%s' ignored: at most %d types are supported", full, MAX_ENTITY_TYPES);
    }
    json_free(root);
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

int entity_count(void) { return g_entity_count; }

void entity_clear(void) { g_entity_count = 0; }

void entity_world_init(u64 seed) {
    g_rng.s = seed ^ 0xE17177E5ull;
    g_entity_count = 0;
}

static void apply_type_to_body(Player *p, const EntityType *t) {
    p->half_width = t->width * 0.5f;
    p->height = t->height;
}

int entity_spawn(const char *type_id, V3 pos) {
    int ti = type_index(type_id);
    if (ti < 0) { LOGW("unknown entity type \"%s\". Types are named namespace:file, for example base:hopper; the console command 'entities' lists them.", type_id); return 0; }
    if (g_entity_count >= MAX_ENTITIES) { LOGW("entity limit of %d reached; remove some before spawning more", MAX_ENTITIES); return 0; }
    dfe_event_t ev = {.name = "entity_spawn", .text = type_id};
    if (event_fire(&ev)) return 0;
    Entity *e = &g_entities[g_entity_count++];
    memset(e, 0, sizeof *e);
    e->id = g_next_id++;
    e->type = ti;
    player_init(&e->body, pos);
    apply_type_to_body(&e->body, &g_types[ti]);
    e->heading = e->body.yaw = rng_float(&g_rng) * 6.2831853f;
    return e->id;
}

static int find_index(int id) {
    for (int i = 0; i < g_entity_count; i++) if (g_entities[i].id == id) return i;
    return -1;
}

bool entity_remove(int id) {
    int i = find_index(id);
    if (i < 0) return false;
    g_entities[i] = g_entities[--g_entity_count];
    return true;
}

bool entity_position(int id, V3 *out) {
    int i = find_index(id);
    if (i < 0) return false;
    *out = g_entities[i].body.pos;
    return true;
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

/* Wandering: walk in a random direction for a while, rest sometimes, hop over what blocks the way and turn away
 * from what cannot be hopped. Water is escaped by swimming up. */
static PlayerInput think(Entity *e, const EntityType *t, float dt) {
    PlayerInput in = {.speed_scale = t->speed};
    if (!t->wander) return in;
    e->ai_timer -= dt;
    if (e->ai_timer <= 0.0f) pick_new_heading(e);
    e->body.yaw += CLAMP(wrap_angle(e->heading - e->body.yaw), -TURN_RATE * dt, TURN_RATE * dt);
    in.forward = e->moving ? 1.0f : 0.0f;
    float speed = hypotf(e->body.vel.x, e->body.vel.z);
    bool blocked = e->moving && e->body.on_ground && speed < t->speed * REFERENCE_SPEED * STUCK_SPEED_FRACTION;
    e->stuck = blocked ? e->stuck + dt : 0.0f;
    in.jump = e->body.in_water || e->stuck > STUCK_JUMP_S;
    if (e->stuck > STUCK_TURN_S) { pick_new_heading(e); e->stuck = 0.0f; }
    return in;
}

void entity_update(float dt) {
    for (int i = 0; i < g_entity_count;) {
        Entity *e = &g_entities[i];
        const EntityType *t = &g_types[e->type];
        e->age += dt;
        if (t->lifetime > 0.0f && e->age >= t->lifetime) { g_entities[i] = g_entities[--g_entity_count]; continue; }
        PlayerInput in = think(e, t, dt);
        player_step(&e->body, &in, dt);
        /* Anything that ends up below the world is removed rather than simulated forever. */
        if (e->body.pos.y < -64.0f) { g_entities[i] = g_entities[--g_entity_count]; continue; }
        i++;
    }
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

static struct { Shader sh; GLuint vao, vbo; bool ready; } G;

static void build_cube(float out[CUBE_VERTS * 6]) {
    static const int QUAD[6] = {0, 1, 2, 0, 2, 3};
    int n = 0;
    for (int f = 0; f < 6; f++)
        for (int k = 0; k < 6; k++) {
            memcpy(out + n * 6, CUBE_FACES[f][QUAD[k]], 3 * sizeof(float));
            memcpy(out + n * 6 + 3, CUBE_NORMALS[f], 3 * sizeof(float));
            n++;
        }
}

bool entity_gl_init(void) {
    float cube[CUBE_VERTS * 6];
    build_cube(cube);
    glGenVertexArrays(1, &G.vao);
    glGenBuffers(1, &G.vbo);
    glBindVertexArray(G.vao);
    glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof cube, cube, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)(3 * sizeof(float)));
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
    if (G.vao) glDeleteVertexArrays(1, &G.vao);
    memset(&G, 0, sizeof G);
}

static void draw_part(V3 size, V3 offset) {
    glUniform3f(shader_uniform(&G.sh, "u_part_size"), size.x, size.y, size.z);
    glUniform3f(shader_uniform(&G.sh, "u_part_offset"), offset.x, offset.y, offset.z);
    glDrawArrays(GL_TRIANGLES, 0, CUBE_VERTS);
}

static void set_light(const Entity *e) {
    V3 c = v3(e->body.pos.x, e->body.pos.y + e->body.height * 0.5f, e->body.pos.z);
    float sky = 1.0f, r = 0, g = 0, b = 0;
    if (world_get_state(ifloor(c.x), ifloor(c.y), ifloor(c.z)) != STATE_UNLOADED) {
        u16 l = world_get_light(ifloor(c.x), ifloor(c.y), ifloor(c.z));
        sky = (float)LIGHT_SKY(l) / 15.0f; r = (float)LIGHT_R(l) / 15.0f; g = (float)LIGHT_G(l) / 15.0f; b = (float)LIGHT_B(l) / 15.0f;
    }
    glUniform4f(shader_uniform(&G.sh, "u_light"), sky, r, g, b);
}

int entity_draw(const Camera *cam, float fog_start, float fog_end) {
    if (!G.ready || !g_entity_count) return 0;
    shader_use(&G.sh);
    M4 rot_view = m4_look_dir(v3(0, 0, 0), cam->forward, v3(0, 1, 0));
    M4 vp = m4_mul(cam->proj, rot_view);
    glUniformMatrix4fv(shader_uniform(&G.sh, "u_viewproj"), 1, GL_FALSE, vp.m);
    atmosphere_set_uniforms(&G.sh);
    glUniform1f(shader_uniform(&G.sh, "u_fog_start"), fog_start);
    glUniform1f(shader_uniform(&G.sh, "u_fog_end"), fog_end);
    glBindVertexArray(G.vao);
    int drawn = 0;
    for (int i = 0; i < g_entity_count; i++) {
        const Entity *e = &g_entities[i];
        const EntityType *t = &g_types[e->type];
        V3 rel = v3_sub(e->body.pos, cam->pos);
        float radius = MAX(t->width, t->height);
        float dist2 = rel.x * rel.x + rel.y * rel.y + rel.z * rel.z;
        if (dist2 > (fog_end + radius) * (fog_end + radius)) continue;
        if (v3_dot(rel, cam->forward) < -radius * 2.0f) continue; /* behind the camera */
        glUniform3f(shader_uniform(&G.sh, "u_origin"), rel.x, rel.y, rel.z);
        glUniform2f(shader_uniform(&G.sh, "u_yaw"), sinf(e->body.yaw), cosf(e->body.yaw));
        set_light(e);
        float w = t->width, h = t->height;
        glUniform3f(shader_uniform(&G.sh, "u_color"), t->color[0], t->color[1], t->color[2]);
        draw_part(v3(w, h * BODY_FRACTION, w), v3(0, h * BODY_FRACTION * 0.5f, 0));
        float head_h = h * (1.0f - BODY_FRACTION);
        glUniform3f(shader_uniform(&G.sh, "u_color"), t->accent[0], t->accent[1], t->accent[2]);
        draw_part(v3(w * HEAD_WIDTH_FRACTION, head_h, w * HEAD_WIDTH_FRACTION), v3(0, h * BODY_FRACTION + head_h * 0.5f, -w * HEAD_FORWARD_FRACTION));
        drawn += 2;
    }
    glBindVertexArray(0);
    return drawn;
}
