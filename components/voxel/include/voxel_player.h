#pragma once

#include <stdbool.h>
#include "voxel_math.h"
#include "voxel_world.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- first-person player (easy-change tuning) -------------------------- */

/* Collision volume: a slab `width` x `height` standing on the feet. `pos` is
 * the FEET-centre of the AABB, not the camera; the camera is placed
 * `eye_height` above the feet. Units are world blocks (1.0 = one voxel). */
#define VOX_PLAYER_WIDTH      0.6f
#define VOX_PLAYER_HEIGHT     1.8f
#define VOX_PLAYER_EYE        1.62f

/* Movement / physics, in world units per second. */
#define VOX_PLAYER_WALK_SPEED   5.0f
#define VOX_PLAYER_JUMP_SPEED   6.5f
#define VOX_PLAYER_GRAVITY      20.0f

/* Look. Pitch is clamped so the view can never flip past vertical. */
#define VOX_PLAYER_PITCH_MAX    1.55f        /* ~89 deg */
#define VOX_PLAYER_MOUSE_SENS   0.0022f      /* rad per HID mouse delta unit */

/* Physics advances at a fixed substep rate even on slow frames (e.g. the
 * ~4 FPS renderer): a 250 ms frame is split into small steps, so gravity and
 * movement stay stable and the AABB (0.6 wide, drops < 1 block/step) cannot
 * tunnel through terrain. */
#define VOX_PLAYER_SUBSTEP_HZ   60.0f

typedef struct {
    vox_vec3 pos;                 /* feet-centre of the AABB, world units */
    vox_vec3 vel;                 /* velocity, world units/second */
    float yaw;                    /* horizontal heading, rad; 0 = +Z (camera conv.) */
    float pitch;                  /* vertical look, rad; +up, clamped */
    bool grounded;                /* standing on solid terrain */

    float width, height, eye_height;   /* AABB + camera, from the constants */
    float move_fwd, move_strafe;       /* input intent set by vox_player_input */
    bool jump_req;                     /* tap, consumed once by the next step */
} vox_player;

void vox_player_init(vox_player *p, vox_vec3 feet_pos, float yaw);

/* Feed per-frame input. fwd/strafe in [-1,1] (W/D positive); `jump` is a tap
 * that only takes effect once and only while grounded. */
void vox_player_input(vox_player *p, float fwd, float strafe, bool jump);

/* Apply a mouse movement of (dx, dy) HID deltas to yaw/pitch (clamped). */
void vox_player_turn(vox_player *p, float dyaw, float dpitch);

/* Advance the player: gravity, jump, then axis-separated AABB collision
 * against the world. `dt` is clamped and substepped. */
void vox_player_update(vox_player *p, const vox_world *w, float dt);

#ifdef __cplusplus
}
#endif