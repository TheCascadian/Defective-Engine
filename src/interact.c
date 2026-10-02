/* Block targeting and the player's mouse actions. The client only chooses what to do; every edit is posted to the
 * server (server.c), which checks reach and rules before changing the world. */
#include "dfe.h"

#include <GLFW/glfw3.h>

Interact g_interact;

#define BIG 1e30f

static bool ray_target(u16 s, bool hit_fluids) {
    if (s == STATE_UNLOADED || s == STATE_AIR) return false;
    return hit_fluids || !(g_state_flags[s] & BF_FLUID);
}

/* Amanatides and Woo grid traversal: visits each cell the ray crosses in order, with no sampling gaps. */
bool raycast_blocks(V3 o, V3 d, float max_dist, bool hit_fluids, RayHit *out) {
    memset(out, 0, sizeof *out);
    int x = ifloor(o.x), y = ifloor(o.y), z = ifloor(o.z);
    int sx = d.x > 0 ? 1 : -1, sy = d.y > 0 ? 1 : -1, sz = d.z > 0 ? 1 : -1;
    float tdx = d.x != 0 ? fabsf(1.0f / d.x) : BIG, tdy = d.y != 0 ? fabsf(1.0f / d.y) : BIG, tdz = d.z != 0 ? fabsf(1.0f / d.z) : BIG;
    float tx = d.x != 0 ? ((sx > 0 ? (float)(x + 1) - o.x : o.x - (float)x)) * tdx : BIG;
    float ty = d.y != 0 ? ((sy > 0 ? (float)(y + 1) - o.y : o.y - (float)y)) * tdy : BIG;
    float tz = d.z != 0 ? ((sz > 0 ? (float)(z + 1) - o.z : o.z - (float)z)) * tdz : BIG;
    int face = -1;
    float t = 0;
    for (;;) {
        u16 s = world_get_state(x, y, z);
        if (ray_target(s, hit_fluids)) {
            out->hit = true;
            out->x = x; out->y = y; out->z = z;
            out->state = s;
            out->dist = t;
            out->face = face < 0 ? DIR_PY : face;
            out->px = x + (face >= 0 ? DIR_VEC[face][0] : 0);
            out->py = y + (face >= 0 ? DIR_VEC[face][1] : 0);
            out->pz = z + (face >= 0 ? DIR_VEC[face][2] : 0);
            return true;
        }
        if (tx <= ty && tx <= tz) { t = tx; tx += tdx; x += sx; face = sx > 0 ? DIR_NX : DIR_PX; }
        else if (ty <= tz) { t = ty; ty += tdy; y += sy; face = sy > 0 ? DIR_NY : DIR_PY; }
        else { t = tz; tz += tdz; z += sz; face = sz > 0 ? DIR_NZ : DIR_PZ; }
        if (t > max_dist) return false;
    }
}

static V3 view_dir(const Player *p) {
    float cp = cosf(p->pitch);
    return v3(-sinf(p->yaw) * cp, sinf(p->pitch), -cosf(p->yaw) * cp);
}

static void select_hotbar(void) {
    for (int i = 0; i < INV_HOTBAR; i++)
        if (key_pressed(GLFW_KEY_1 + i)) g_inv.selected = i;
    int wheel = (int)g_in.scroll;
    if (wheel) g_inv.selected = floor_mod(g_inv.selected - (wheel > 0 ? 1 : -1), INV_HOTBAR);
}

static float break_time(u16 state) {
    const BlockDef *b = block_of_state(state);
    if (!b || b->hardness < 0) return -1.0f;
    return g_creative ? 0.0f : b->hardness;
}

static void update_breaking(float dt) {
    Interact *it = &g_interact;
    bool held = g_in.mouse_buttons[GLFW_MOUSE_BUTTON_LEFT] && it->target.hit;
    if (!held) { it->breaking = false; it->break_progress = 0; return; }
    const RayHit *h = &it->target;
    if (!it->breaking || h->x != it->break_x || h->y != it->break_y || h->z != it->break_z) {
        it->breaking = true;
        it->break_progress = 0;
        it->break_x = h->x; it->break_y = h->y; it->break_z = h->z;
    }
    float need = break_time(h->state);
    if (need < 0) return; /* unbreakable */
    if (need == 0.0f) { /* instant: repeat at a fixed rate while the button is held */
        if (it->break_cooldown > 0) return;
        it->break_cooldown = INTERACT_REPEAT_S;
        ServerMsg m = {.type = MSG_BREAK, .x = h->x, .y = h->y, .z = h->z};
        server_post(&m);
        return;
    }
    it->break_progress += dt / need;
    if (it->break_progress >= 1.0f) {
        ServerMsg m = {.type = MSG_BREAK, .x = h->x, .y = h->y, .z = h->z};
        server_post(&m);
        it->breaking = false;
        it->break_progress = 0;
    }
}

static void update_placing(void) {
    Interact *it = &g_interact;
    if (!g_in.mouse_buttons[GLFW_MOUSE_BUTTON_RIGHT] || !it->target.hit || it->place_cooldown > 0) return;
    ItemStack *held = &g_inv.slot[g_inv.selected];
    if (!held->count) return;
    it->place_cooldown = INTERACT_REPEAT_S;
    const RayHit *h = &it->target;
    /* Aiming at a replaceable block such as tall grass replaces it instead of building next to it. */
    bool replace = (g_state_flags[h->state] & BF_REPLACEABLE) != 0;
    ServerMsg m = {.type = MSG_PLACE, .x = replace ? h->x : h->px, .y = replace ? h->y : h->py, .z = replace ? h->z : h->pz, .state = held->state};
    server_post(&m);
}

static void pick_block(void) {
    if (!g_in.mouse_pressed[GLFW_MOUSE_BUTTON_MIDDLE] || !g_interact.target.hit) return;
    const BlockDef *b = block_of_state(g_interact.target.state);
    u16 item = b ? item_for_block(b) : STATE_AIR;
    if (item == STATE_AIR) return;
    for (int i = 0; i < INV_HOTBAR; i++)
        if (g_inv.slot[i].count && g_inv.slot[i].state == item) { g_inv.selected = i; return; }
    if (!g_creative) return;
    ItemStack *s = &g_inv.slot[g_inv.selected];
    s->state = item;
    s->count = INV_MAX_STACK;
}

void interact_update(const Player *p, float dt, bool active) {
    Interact *it = &g_interact;
    it->place_cooldown = MAX(it->place_cooldown - dt, 0.0f);
    it->break_cooldown = MAX(it->break_cooldown - dt, 0.0f);
    raycast_blocks(player_eye(p), view_dir(p), REACH_DISTANCE, false, &it->target);
    if (!active) { it->breaking = false; it->break_progress = 0; return; }
    select_hotbar();
    update_breaking(dt);
    update_placing();
    pick_block();
}
