/* Camera feel for the walking player: view bobbing, a dip on landing, a wider view while sprinting and smoothing
 * of the sudden rise when stepping up or down a block. Everything here only moves the camera; the player's body,
 * collision and physics are untouched, and each effect has an off switch in the controls options. */
#include "dfe.h"

#define BOB_STRIDE 0.62f        /* phase advanced per block walked; two steps per full cycle of the sway */
#define BOB_VERTICAL 0.032f
#define BOB_SIDE 0.022f
#define BOB_REFERENCE_SPEED 4.3f
#define BOB_BLEND_RATE 8.0f
#define LAND_DIP_PER_SPEED 0.011f
#define LAND_DIP_MAX 0.28f
#define LAND_MIN_SPEED 6.0f
#define LAND_RECOVER_RATE 9.0f
#define STEP_MIN 0.05f
#define STEP_MAX 1.05f
#define STEP_RECOVER_RATE 11.0f
#define SPRINT_FOV_SCALE 1.09f
#define SPRINT_SPEED_START 4.8f
#define FOV_BLEND_RATE 7.0f

static struct {
    float phase, bob_amount, land_dip, step_offset, fov_scale, prev_y, air_vy;
    bool prev_ground, ready;
} J = {.fov_scale = 1.0f};

float juice_fov_scale(void) { return J.fov_scale; }

static float blend(float *v, float target, float rate, float dt) {
    *v += (target - *v) * (1.0f - expf(-rate * dt));
    return *v;
}

void juice_update(Camera *cam, const Player *p, float dt, bool walking_view) {
    bool fx = !g_settings.motion_fx_off, bob = !g_settings.view_bob_off;
    float hspeed = sqrtf(p->vel.x * p->vel.x + p->vel.z * p->vel.z);
    bool grounded_walk = walking_view && p->on_ground && !p->flying && !p->in_water && !p->dead;

    if (!J.ready || fabsf(p->pos.y - J.prev_y) > 3.0f) { J.prev_y = p->pos.y; J.step_offset = J.land_dip = 0; J.ready = true; }
    float dy = p->pos.y - J.prev_y;
    if (fx && grounded_walk && J.prev_ground && fabsf(dy) > STEP_MIN && fabsf(dy) < STEP_MAX) J.step_offset = CLAMP(J.step_offset - dy, -1.0f, 1.0f);
    if (fx && grounded_walk && !J.prev_ground && J.air_vy < -LAND_MIN_SPEED) J.land_dip = MIN(-J.air_vy * LAND_DIP_PER_SPEED, LAND_DIP_MAX);
    if (!p->on_ground) J.air_vy = p->vel.y;
    J.prev_y = p->pos.y;
    J.prev_ground = p->on_ground;

    blend(&J.step_offset, 0.0f, STEP_RECOVER_RATE, dt);
    blend(&J.land_dip, 0.0f, LAND_RECOVER_RATE, dt);
    if (!fx) J.step_offset = J.land_dip = 0.0f;

    float bob_target = bob && grounded_walk ? CLAMP(hspeed / BOB_REFERENCE_SPEED, 0.0f, 1.4f) : 0.0f;
    blend(&J.bob_amount, bob_target, BOB_BLEND_RATE, dt);
    if (grounded_walk) J.phase += hspeed * dt * BOB_STRIDE;
    float vertical = fabsf(sinf(J.phase)) * BOB_VERTICAL * J.bob_amount - BOB_VERTICAL * 0.5f * J.bob_amount;
    float side = sinf(J.phase) * BOB_SIDE * J.bob_amount;

    float fov_target = fx && walking_view && !p->dead && hspeed > SPRINT_SPEED_START ? SPRINT_FOV_SCALE : 1.0f;
    blend(&J.fov_scale, fov_target, FOV_BLEND_RATE, dt);

    cam->pos.y += vertical + J.step_offset - J.land_dip;
    cam->pos.x += cosf(cam->yaw) * side;
    cam->pos.z -= sinf(cam->yaw) * side;
}
