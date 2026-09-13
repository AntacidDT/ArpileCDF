#include <math.h>
#include "voxel_player.h"
#include "voxel_block.h"

/* Snug margin keeping the AABB 1mm out of a neighbour voxel after a collision
 * snap (wall face, ground, ceiling) without leaving a visible gap. */
#define COLL_EPS 1e-3f

void vox_player_init(vox_player *p, vox_vec3 feet_pos, float yaw)
{
    if (!p) return;
    p->pos = feet_pos;
    p->vel = vox_v3(0.0f, 0.0f, 0.0f);
    p->yaw = yaw;
    p->pitch = 0.0f;
    p->grounded = false;
    p->width = VOX_PLAYER_WIDTH;
    p->height = VOX_PLAYER_HEIGHT;
    p->eye_height = VOX_PLAYER_EYE;
    p->move_fwd = 0.0f;
    p->move_strafe = 0.0f;
    p->jump_req = false;
}

void vox_player_input(vox_player *p, float fwd, float strafe, bool jump)
{
    if (!p) return;
    p->move_fwd = vox_clampf(fwd, -1.0f, 1.0f);
    p->move_strafe = vox_clampf(strafe, -1.0f, 1.0f);
    if (jump) p->jump_req = true;
}

void vox_player_turn(vox_player *p, float dyaw, float dpitch)
{
    if (!p) return;
    p->yaw += dyaw;
    p->pitch += dpitch;
    if (p->pitch > VOX_PLAYER_PITCH_MAX)  p->pitch = VOX_PLAYER_PITCH_MAX;
    if (p->pitch < -VOX_PLAYER_PITCH_MAX) p->pitch = -VOX_PLAYER_PITCH_MAX;
}

/* Does the AABB [x0,x1)x[y0,y1)x[z0,z1) touch any solid voxel? Only the cells
 * overlapped by the (tiny) player box are probed, so collision cost is
 * O(AABB footprint), never O(world). AIR is non-solid; anything else is. */
static bool aabb_touches_solid(const vox_world *w,
                               float x0, float y0, float z0,
                               float x1, float y1, float z1)
{
    int ix0 = (int)floorf(x0);
    int iy0 = (int)floorf(y0);
    int iz0 = (int)floorf(z0);
    int ix1 = (int)ceilf(x1 - 1e-4f);
    int iy1 = (int)ceilf(y1 - 1e-4f);
    int iz1 = (int)ceilf(z1 - 1e-4f);
    for (int ix = ix0; ix < ix1; ix++) {
        for (int iy = iy0; iy < iy1; iy++) {
            for (int iz = iz0; iz < iz1; iz++) {
                if (vox_world_get(w, ix, iy, iz) != VOX_BLOCK_AIR) {
                    return true;
                }
            }
        }
    }
    return false;
}

/* One fixed-dt physics step. Movement resolves X, then Y, then Z on its own,
 * so the player slides along walls, lands on floors and bumps ceilings. */
static void player_step(vox_player *p, const vox_world *w, float dt)
{
    const float hw = p->width * 0.5f;
    const float hh = p->height;

    /* Gravity: velocity grows downward every step. */
    p->vel.y -= VOX_PLAYER_GRAVITY * dt;

    /* Jump: only from the ground, one tap = one jump. */
    if (p->jump_req) {
        p->jump_req = false;
        if (p->grounded) {
            p->vel.y = VOX_PLAYER_JUMP_SPEED;
            p->grounded = false;
        }
    }

    /* Horizontal velocity follows the horizontal yaw ONLY (pitch never enters
     * it). Diagonals are normalised so W+D is no faster than W. */
    {
        float sy = sinf(p->yaw);
        float cy = cosf(p->yaw);
        float mx = sy * p->move_fwd + cy * p->move_strafe;
        float mz = cy * p->move_fwd - sy * p->move_strafe;
        float ml = sqrtf(mx * mx + mz * mz);
        if (ml > 1.0f) {
            mx /= ml;
            mz /= ml;
        }
        p->vel.x = mx * VOX_PLAYER_WALK_SPEED;
        p->vel.z = mz * VOX_PLAYER_WALK_SPEED;
    }

    /* X axis: enter a wall -> stop at its face. */
    {
        float nx = p->pos.x + p->vel.x * dt;
        if (aabb_touches_solid(w, nx - hw, p->pos.y, p->pos.z - hw,
                                  nx + hw, p->pos.y + hh, p->pos.z + hw)) {
            if (p->vel.x > 0.0f) {
                nx = (float)(int)floorf(nx + hw) - hw - COLL_EPS;
            } else if (p->vel.x < 0.0f) {
                nx = (float)(int)floorf(nx - hw) + 1.0f + hw + COLL_EPS;
            }
            p->vel.x = 0.0f;
        }
        p->pos.x = nx;
    }

    /* Z axis: same, mirrored. */
    {
        float nz = p->pos.z + p->vel.z * dt;
        if (aabb_touches_solid(w, p->pos.x - hw, p->pos.y, nz - hw,
                                  p->pos.x + hw, p->pos.y + hh, nz + hw)) {
            if (p->vel.z > 0.0f) {
                nz = (float)(int)floorf(nz + hw) - hw - COLL_EPS;
            } else if (p->vel.z < 0.0f) {
                nz = (float)(int)floorf(nz - hw) + 1.0f + hw + COLL_EPS;
            }
            p->vel.z = 0.0f;
        }
        p->pos.z = nz;
    }

    /* Y axis: downward impact lands (grounded), upward stops at a ceiling. */
    {
        float ny = p->pos.y + p->vel.y * dt;
        if (aabb_touches_solid(w, p->pos.x - hw, ny, p->pos.z - hw,
                                  p->pos.x + hw, ny + hh, p->pos.z + hw)) {
            bool going_down = (p->vel.y <= 0.0f);
            if (p->vel.y > 0.0f) {
                ny = (float)(int)floorf(ny + hh) - hh - COLL_EPS;
            } else {
                ny = (float)(int)floorf(ny) + 1.0f + COLL_EPS;
            }
            p->vel.y = 0.0f;
            p->grounded = going_down;
        }
        p->pos.y = ny;

        /* Ground state is re-derived from the support just below the feet:
         * - while rising, the player is never grounded (blocks airborne jumps);
         * - a tiny downward probe clears `grounded` the moment the player
         *   steps off a ledge or falls, so mid-air jumps stay impossible. */
        if (p->vel.y > 0.0f) {
            p->grounded = false;
        } else {
            /* Support probe, NOT a repostioning snap: the Y collision above
             * already placed the feet exactly on the floor's top face. */
            p->grounded = aabb_touches_solid(w,
                p->pos.x - hw, p->pos.y - COLL_EPS * 2.0f, p->pos.z - hw,
                p->pos.x + hw, p->pos.y + COLL_EPS * 2.0f, p->pos.z + hw);
        }
    }

    /* The player cannot leave the world footprint. */
    {
        float limit = p->width * 0.5f + 0.5f;
        float wx = (float)(vox_world_chunks_x(w) * 16) - limit;
        float wz = (float)(vox_world_chunks_z(w) * 16) - limit;
        p->pos.x = vox_clampf(p->pos.x, limit, wx);
        p->pos.z = vox_clampf(p->pos.z, limit, wz);
    }
}

void vox_player_update(vox_player *p, const vox_world *w, float dt)
{
    if (!p || !w) return;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.1f) dt = 0.1f;   /* a slow 4-FPS frame must not cause a huge jump */

    const float step = 1.0f / VOX_PLAYER_SUBSTEP_HZ;
    while (dt > 1e-6f) {
        float s = (dt > step) ? step : dt;
        player_step(p, w, s);
        dt -= s;
    }
}