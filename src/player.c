/* Player controller: an axis-aligned box moved one axis at a time against the voxel grid.
 *
 * Decisions:
 *  - Axis-separated resolution with a snap to the blocking face is exact for a box and cheap. A step never exceeds
 *    one block (terminal speed times the physics step is below one), so the box cannot tunnel through a wall.
 *  - Unloaded columns count as solid, so a player who outruns streaming stops at the edge instead of falling
 *    through the world.
 *  - Time is split into fixed physics steps so the result does not depend on the frame rate. */
#include "dfe.h"

#define PHYSICS_STEP (1.0f / 60.0f)
#define HALF_WIDTH (PLAYER_WIDTH * 0.5f)
#define SKIN 0.001f            /* the box is shrunk by this much so resting contact is not an overlap */
#define GRAVITY 30.0f
#define JUMP_SPEED 9.2f
#define TERMINAL_SPEED 50.0f
#define WALK_SPEED 4.3f
#define SPRINT_SPEED 5.8f
#define FLY_SPEED 11.0f
#define FLY_SPRINT_SPEED 32.0f
#define SWIM_SPEED 2.6f
#define SWIM_UP_SPEED 3.6f
#define WATER_SINK_SPEED 2.2f
#define LAVA_SPEED_SCALE 0.4f
#define AIR_ACCEL 2.0f
#define AIR_DRAG 0.2f
#define SWIM_RESPONSE 7.0f
#define CLIMB_RESPONSE 14.0f
#define FLY_RESPONSE 8.0f
#define STEP_HEIGHT 1.0f
#define CLIMB_SPEED 3.0f
#define SPAWN_SEARCH_RADIUS 640
#define SPAWN_SEARCH_STEP 16
#define SPAWN_MIN_HEIGHT_ABOVE_SEA 2
#define FALL_SAFE_DISTANCE 3.0f
#define FALL_DAMAGE_PER_BLOCK 1.5f
#define HURT_INVULNERABILITY 0.65f
#define HURT_FLASH_TIME 0.8f
#define LAVA_DAMAGE_INTERVAL 1.0f

Player g_player;

void player_init(Player *p, V3 feet) {
    memset(p, 0, sizeof *p);
    p->pos = feet;
    p->half_width = HALF_WIDTH;
    p->height = PLAYER_HEIGHT;
    p->render_eye_height = PLAYER_EYE;
    p->health = PLAYER_MAX_HEALTH;
    p->fall_peak_y = feet.y;
}

V3 player_eye(const Player *p) { return v3(p->pos.x, p->pos.y + (p->crouched ? PLAYER_CROUCH_EYE : PLAYER_EYE), p->pos.z); }
V3 player_eye_render(const Player *p) { return v3(p->pos.x, p->pos.y + p->render_eye_height, p->pos.z); }

void player_hurt(Player *p, float damage) {
    if (damage <= 0.0f || p->dead || p->invulnerability_timer > 0.0f || (p == &g_player && g_creative)) return;
    p->health = MAX(p->health - damage, 0.0f);
    p->hurt_timer = HURT_FLASH_TIME;
    p->invulnerability_timer = HURT_INVULNERABILITY;
    if (p->health <= 0.0f) p->dead = true;
}

bool player_teleport(Player *p, V3 feet) {
    if (!isfinite(feet.x) || !isfinite(feet.y) || !isfinite(feet.z) || box_blocked(feet, p->half_width, p->height)) return false;
    p->pos = feet;
    p->vel = v3(0, 0, 0);
    p->on_ground = false;
    p->fall_peak_y = feet.y;
    p->jump_buffer = 0.0f;
    return true;
}

void player_respawn(Player *p, V3 feet) {
    float yaw = p->yaw, pitch = p->pitch;
    player_init(p, feet);
    p->yaw = yaw;
    p->pitch = pitch;
}

static bool cell_blocks(int x, int y, int z) {
    u16 s = world_get_state(x, y, z);
    return s == STATE_UNLOADED || state_solid(s);
}

bool box_blocked(V3 f, float half_width, float height) {
    int x0 = ifloor(f.x - half_width + SKIN), x1 = ifloor(f.x + half_width - SKIN);
    int y0 = ifloor(f.y + SKIN), y1 = ifloor(f.y + height - SKIN);
    int z0 = ifloor(f.z - half_width + SKIN), z1 = ifloor(f.z + half_width - SKIN);
    for (int y = y0; y <= y1; y++)
        for (int z = z0; z <= z1; z++)
            for (int x = x0; x <= x1; x++)
                if (cell_blocks(x, y, z)) return true;
    return false;
}

bool player_box_blocked(V3 f) { return box_blocked(f, HALF_WIDTH, PLAYER_HEIGHT); }

/* Moves along one axis and snaps to the face that stops the box. Returns true when something was hit. */
static bool move_axis(Player *p, int axis, float delta) {
    if (delta == 0.0f) return false;
    float *c = axis == 0 ? &p->pos.x : axis == 1 ? &p->pos.y : &p->pos.z;
    float old = *c;
    *c = old + delta;
    if (!box_blocked(p->pos, p->half_width, p->height)) return false;
    float lo_extent = axis == 1 ? 0.0f : p->half_width, hi_extent = axis == 1 ? p->height : p->half_width;
    if (delta > 0) *c = (float)ifloor(old + hi_extent + delta) - hi_extent - SKIN * 2.0f;
    else *c = (float)(ifloor(old - lo_extent + delta) + 1) + lo_extent + SKIN * 2.0f;
    /* The snap can still overlap when the box starts inside a block, such as after a block is placed on it. */
    if (box_blocked(p->pos, p->half_width, p->height)) *c = old;
    return true;
}

static void sample_medium(Player *p) {
    int x = ifloor(p->pos.x), z = ifloor(p->pos.z);
    u16 feet = world_get_state(x, ifloor(p->pos.y + 0.2f), z), body = world_get_state(x, ifloor(p->pos.y + 1.0f), z);
    u16 head = world_get_state(x, ifloor(p->pos.y + p->height * (PLAYER_EYE / PLAYER_HEIGHT)), z);
    bool feet_f = feet != STATE_UNLOADED && (g_state_flags[feet] & BF_FLUID);
    bool body_f = body != STATE_UNLOADED && (g_state_flags[body] & BF_FLUID);
    p->in_water = feet_f || body_f;
    p->head_in_water = head != STATE_UNLOADED && (g_state_flags[head] & BF_FLUID);
    p->in_lava = false;
    if (p->in_water) {
        const BlockDef *b = block_of_state(feet_f ? feet : body);
        p->in_lava = b && b->emit[0] > 0; /* lava glows; the only fluid that does today */
    }
}

static bool on_climbable(const Player *p) {
    int x = ifloor(p->pos.x), z = ifloor(p->pos.z);
    for (int dy = 0; dy < 2; dy++) {
        u16 s = world_get_state(x, ifloor(p->pos.y) + dy, z);
        if (s != STATE_UNLOADED && (g_state_flags[s] & BF_CLIMBABLE)) return true;
    }
    return false;
}

static void approach(float *v, float target, float rate, float dt) {
    float k = 1.0f - expf(-rate * dt);
    *v += (target - *v) * k;
}

static void approach_accel(float *v, float target, float acceleration, float dt) {
    float delta = target - *v;
    *v += CLAMP(delta, -acceleration * dt, acceleration * dt);
}

static float ground_friction(const Player *p) {
    u16 state = world_get_state(ifloor(p->pos.x), ifloor(p->pos.y - 0.05f), ifloor(p->pos.z));
    const BlockDef *b = state == STATE_UNLOADED ? NULL : block_of_state(state);
    return b ? CLAMP(b->friction, 0.05f, 2.0f) : 1.0f;
}

static void horizontal_wish(const Player *p, const PlayerInput *in, float speed, float *wx, float *wz) {
    float fx = -sinf(p->yaw), fz = -cosf(p->yaw); /* yaw 0 looks down -Z, see Camera */
    float rx = cosf(p->yaw), rz = -sinf(p->yaw);
    float x = fx * in->forward + rx * in->strafe, z = fz * in->forward + rz * in->strafe;
    float len = sqrtf(x * x + z * z);
    if (len > 1.0f) { x /= len; z /= len; }
    *wx = x * speed;
    *wz = z * speed;
}

/* Tries to climb a low ledge: lift the box, repeat the blocked horizontal move, and settle back down. */
static bool try_step_up(Player *p, int axis, float delta) {
    if (g_settings.auto_jump_off) return false;
    Player t = *p;
    if (move_axis(&t, 1, STEP_HEIGHT)) return false;
    if (move_axis(&t, axis, delta)) return false;
    move_axis(&t, 1, -STEP_HEIGHT);
    float gained = axis == 0 ? fabsf(t.pos.x - p->pos.x) : fabsf(t.pos.z - p->pos.z);
    if (gained < fabsf(delta) * 0.5f) return false;
    *p = t;
    return true;
}

static bool try_step_down(Player *p) {
    Player t = *p;
    if (!move_axis(&t, 1, -(STEP_HEIGHT + 0.01f))) return false;
    *p = t;
    return true;
}

static void collide_and_move(Player *p, float dt) {
    bool was_ground = p->on_ground;
    float dx = p->vel.x * dt, dy = p->vel.y * dt, dz = p->vel.z * dt;
    if (move_axis(p, 0, dx)) {
        if (!(was_ground && !p->flying && try_step_up(p, 0, dx))) p->vel.x = 0;
    } else if (dx != 0.0f && was_ground && !p->flying) try_step_down(p);
    if (move_axis(p, 2, dz)) {
        if (!(was_ground && !p->flying && try_step_up(p, 2, dz))) p->vel.z = 0;
    } else if (dz != 0.0f && was_ground && !p->flying) try_step_down(p);
    bool hit = move_axis(p, 1, dy);
    p->on_ground = hit && dy < 0;
    if (hit) p->vel.y = 0;
}

static void step_fly(Player *p, const PlayerInput *in, float dt) {
    float wx, wz, speed = in->sprint ? FLY_SPRINT_SPEED : FLY_SPEED;
    horizontal_wish(p, in, speed, &wx, &wz);
    float wy = ((in->jump ? 1.0f : 0.0f) - (in->descend ? 1.0f : 0.0f)) * speed;
    approach(&p->vel.x, wx, FLY_RESPONSE, dt);
    approach(&p->vel.y, wy, FLY_RESPONSE, dt);
    approach(&p->vel.z, wz, FLY_RESPONSE, dt);
}

static void step_swim(Player *p, const PlayerInput *in, float dt) {
    float wx, wz, scale = p->in_lava ? LAVA_SPEED_SCALE : 1.0f;
    horizontal_wish(p, in, SWIM_SPEED * scale, &wx, &wz);
    approach(&p->vel.x, wx, SWIM_RESPONSE, dt);
    approach(&p->vel.z, wz, SWIM_RESPONSE, dt);
    float wy = in->jump ? SWIM_UP_SPEED * scale : (in->descend ? -SWIM_UP_SPEED * scale : -WATER_SINK_SPEED * scale);
    approach(&p->vel.y, wy, 5.0f, dt);
}

static void step_climb(Player *p, const PlayerInput *in, float dt) {
    float wx, wz;
    horizontal_wish(p, in, WALK_SPEED * 0.5f, &wx, &wz);
    approach(&p->vel.x, wx, CLIMB_RESPONSE, dt);
    approach(&p->vel.z, wz, CLIMB_RESPONSE, dt);
    p->vel.y = in->jump || in->forward > 0 ? CLIMB_SPEED : (in->descend ? -CLIMB_SPEED : 0.0f);
}

static void step_walk(Player *p, const PlayerInput *in, float dt) {
    float wx, wz, speed = (in->sprint && in->forward > 0 ? SPRINT_SPEED : WALK_SPEED) * (in->speed_scale > 0 ? in->speed_scale : 1.0f);
    horizontal_wish(p, in, speed, &wx, &wz);
    if (p->on_ground) {
        float grip = CLAMP(ground_friction(p), 0.5f, 1.1f);
        p->vel.x = wx * grip;
        p->vel.z = wz * grip;
    } else {
        bool has_wish = fabsf(wx) + fabsf(wz) > 1e-4f;
        float accel = has_wish ? AIR_ACCEL : AIR_DRAG;
        approach_accel(&p->vel.x, wx, accel, dt);
        approach_accel(&p->vel.z, wz, accel, dt);
    }
    if (p->jump_buffer > 0.0f && p->on_ground) {
        p->vel.y = JUMP_SPEED;
        p->on_ground = false;
        p->jump_buffer = 0.0f;
    }
    p->vel.y = MAX(p->vel.y - GRAVITY * dt, -TERMINAL_SPEED);
}

static void physics_step(Player *p, const PlayerInput *in, float dt) {
    if (p->dead) return;
    bool was_ground = p->on_ground;
    if (in->crouch) {
        p->crouched = true;
        p->height = PLAYER_CROUCH_HEIGHT;
    } else if (p->crouched && !box_blocked(p->pos, p->half_width, PLAYER_HEIGHT)) {
        p->crouched = false;
        p->height = PLAYER_HEIGHT;
    }
    approach(&p->render_eye_height, p->crouched ? PLAYER_CROUCH_EYE : PLAYER_EYE, 10.0f, dt);
    sample_medium(p);
    if (p->flying) step_fly(p, in, dt);
    else if (p->in_water) step_swim(p, in, dt);
    else if (on_climbable(p)) step_climb(p, in, dt);
    else step_walk(p, in, dt);
    collide_and_move(p, dt);
    if (p->on_ground) {
        if (!was_ground) player_hurt(p, MAX(p->fall_peak_y - p->pos.y - FALL_SAFE_DISTANCE, 0.0f) * FALL_DAMAGE_PER_BLOCK);
        p->fall_peak_y = p->pos.y;
    } else p->fall_peak_y = MAX(p->fall_peak_y, p->pos.y);
    p->hurt_timer = MAX(p->hurt_timer - dt, 0.0f);
    p->invulnerability_timer = MAX(p->invulnerability_timer - dt, 0.0f);
    if (p->in_lava) {
        p->lava_damage_timer -= dt;
        if (p->lava_damage_timer <= 0.0f) {
            player_hurt(p, 4.0f);
            p->lava_damage_timer = LAVA_DAMAGE_INTERVAL;
        }
    } else p->lava_damage_timer = 0.0f;
    if (p->flying && p->on_ground) p->flying = false; /* landing ends flight, as the player expects */
    p->jump_buffer = MAX(p->jump_buffer - dt, 0.0f);
}

void player_step(Player *p, const PlayerInput *in, float dt) {
    if (p->dead) { p->vel = v3(0, 0, 0); return; }
    if (in->toggle_fly) {
        p->flying = !p->flying;
        p->vel.y = 0;
    }
    if (in->jump_pressed) p->jump_buffer = 0.12f;
    while (dt > 1e-6f) {
        float h = MIN(dt, PHYSICS_STEP);
        physics_step(p, in, h);
        dt -= h;
    }
}

V3 player_find_spawn(void) {
    int sea = gen_sea_level();
    for (int r = 0; r <= SPAWN_SEARCH_RADIUS; r += SPAWN_SEARCH_STEP) {
        for (int k = 0; k < (r == 0 ? 1 : 8); k++) {
            float a = (float)k * (TAU_F / 8.0f);
            float x = cosf(a) * (float)r, z = sinf(a) * (float)r;
            float sx = floorf(x) + 0.5f, sz = floorf(z) + 0.5f;
            float h = gen_height_at(sx, sz);
            if (h >= (float)(sea + SPAWN_MIN_HEIGHT_ABOVE_SEA)) return v3(sx, floorf(h) + 1.0f + 0.01f, sz);
        }
    }
    LOGW("no land within %d blocks of origin; spawning over water", SPAWN_SEARCH_RADIUS);
    return v3(0.5f, (float)sea + 3.0f, 0.5f); /* an archipelago-free world: stand on the sea's surface level */
}
