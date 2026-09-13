#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "usb_host_input.h"
#include "voxel_game.h"
#include "voxel_math.h"
#include "voxel_player.h"
#include "voxel_world.h"
#include "voxel_mesh.h"

#define WORLD_SEED       0xC0DEu
#define CHUNK_X          3
#define CHUNK_Y          1
#define CHUNK_Z          3

static const vox_vec3 WORLD_LIGHT = { 0.4f, 1.0f, -0.35f };

/* World spans 48 x 16 x 48 blocks; centre used for the camera orbit. */
static const vox_vec3 WORLD_CENTER = { 24.0f, 8.0f, 24.0f };

/* Hotbar contents: the six placeable block types, then empty slots. */
static const uint8_t HOTBAR_BLOCKS[VOX_HOTBAR_SLOTS] = {
    VOX_BLOCK_GRASS,
    VOX_BLOCK_DIRT,
    VOX_BLOCK_STONE,
    VOX_BLOCK_SAND,
    VOX_BLOCK_WOOD,
    VOX_BLOCK_LEAVES,
    VOX_BLOCK_AIR,
    VOX_BLOCK_AIR,
};

typedef struct {
    vox_vec3 p;
    float u, v;
} vox_vv;

enum {
    VK_W = 0x0001,
    VK_S = 0x0002,
    VK_A = 0x0004,
    VK_D = 0x0008,
    VK_SPACE = 0x0010,
    VK_Q = 0x0020,   /* look left */
    VK_E = 0x0040,   /* look right */
    VK_O = 0x0080,   /* look up */
    VK_L = 0x0100,   /* look down */
};

struct vox_game {
    vox_renderer renderer;
    vox_tex_atlas atlas;
    uint16_t *shade_cache;          /* [tex][dir] pre-shaded tiles */
    vox_player player;
    vox_camera cam;
    vox_world *world;
    bool spin;
    uint16_t keys;
    int frame_count;
    int tris;
    int fps;
    int chunks;
    int blocks;
    int faces;
    int selected_slot;
    uint32_t faces_culled;
    uint32_t faces_drawn;
    uint32_t begin_us;
    uint32_t geom_us;
    uint32_t raster_us;
    uint32_t frame_us_last;
    float yaw_snap_cos;
    float yaw_snap_sin;
    TickType_t fps_tick;
};

static uint16_t key_to_bit(uint16_t keycode)
{
    switch (keycode) {
    case ARPILE_KEY_W:     return VK_W;
    case ARPILE_KEY_S:     return VK_S;
    case ARPILE_KEY_A:     return VK_A;
    case ARPILE_KEY_D:     return VK_D;
    case ARPILE_KEY_SPACE: return VK_SPACE;
    case ARPILE_KEY_Q:     return VK_Q;
    case ARPILE_KEY_E:     return VK_E;
    case ARPILE_KEY_O:     return VK_O;
    case ARPILE_KEY_L:     return VK_L;
    default: return 0;
    }
}

static int clip_near(const vox_vv in[3], float near_plane, vox_vv out[4])
{
    int n = 0;
    for (int i = 0; i < 3; i++) {
        const vox_vv *cur = &in[i];
        const vox_vv *nxt = &in[(i + 1) % 3];
        bool cur_in = cur->p.z >= near_plane;
        bool nxt_in = nxt->p.z >= near_plane;
        if (cur_in) {
            out[n++] = *cur;
        }
        if (cur_in != nxt_in) {
            float t = (near_plane - cur->p.z) / (nxt->p.z - cur->p.z);
            vox_vv m;
            m.p.x = cur->p.x + (nxt->p.x - cur->p.x) * t;
            m.p.y = cur->p.y + (nxt->p.y - cur->p.y) * t;
            m.p.z = near_plane;
            m.u = cur->u + (nxt->u - cur->u) * t;
            m.v = cur->v + (nxt->v - cur->v) * t;
            out[n++] = m;
        }
    }
    return n;
}

static void to_screen(const vox_camera_proj *proj, vox_vv vv, vox_vtx *out)
{
    float q = 1e-6f;
    float sx = 0.0f;
    float sy = 0.0f;
    vox_camera_project(proj, vv.p, &sx, &sy, &q);
    if (q < 1e-6f) q = 1e-6f;
    out->x = sx;
    out->y = sy;
    out->q = q;
    out->uq = vv.u * q;
    out->vq = vv.v * q;
}

static int project_triangle(vox_game *g, const vox_camera_proj *proj,
                            vox_vv a, vox_vv b, vox_vv c,
                            const uint16_t *tile)
{
    vox_vec3 e1 = vox_v3_sub(b.p, a.p);
    vox_vec3 e2 = vox_v3_sub(c.p, a.p);
    vox_vec3 n = vox_v3_cross(e1, e2);
    /* Corners are wound with cross(e1,e2) pointing OUT of the block
     * (voxel_mesh.c). A visible face therefore has its normal pointing at the
     * camera, i.e. anti-parallel to a.p (camera->vertex), so dot(n,a.p) < 0.
     * Cull only faces whose normal points AWAY from the camera. */
    if (vox_v3_dot(n, a.p) > 0.0f) return 0;

    vox_vv in[3] = { a, b, c };
    vox_vv poly[4];
    int cnt = clip_near(in, g->cam.near_plane, poly);
    if (cnt < 3) return 0;

    for (int k = 1; k < cnt - 1; k++) {
        vox_vtx ta, tb, tc;
        to_screen(proj, poly[0], &ta);
        to_screen(proj, poly[k], &tb);
        to_screen(proj, poly[k + 1], &tc);
        vox_renderer_triangle(&g->renderer, &ta, &tb, &tc, tile);
        g->tris++;
    }
    return 1;
}

static void face_shades(float out[VOX_FACE_COUNT])
{
    vox_vec3 light = vox_v3_normalize(WORLD_LIGHT);
    for (int dir = 0; dir < VOX_FACE_COUNT; dir++) {
        vox_vec3 n = vox_v3((float)vox_mesh_normal[dir][0],
                            (float)vox_mesh_normal[dir][1],
                            (float)vox_mesh_normal[dir][2]);
        float d = vox_v3_dot(n, light);
        if (d < 0.0f) d = 0.0f;
        out[dir] = 0.45f + 0.55f * d;
    }
}

/* Point the camera at the world centre. */
static void aim_at_world_center(vox_game *g)
{
    vox_vec3 d = vox_v3_sub(WORLD_CENTER, g->cam.pos);
    float len = vox_v3_length(d);
    if (len < 1e-4f) return;
    g->cam.yaw = atan2f(d.x, d.z);
    g->cam.pitch = asinf(vox_clampf(d.y / len, -1.0f, 1.0f));
}

/* Surface height at column (x,z): the y of the top face of the topmost solid
 * block, or -1 if the column has no terrain. Returns -1 too if anything solid
 * occupies the 3 blocks above the surface (e.g. a tree trunk) so the player
 * never spawns inside an obstacle. */
static float spawn_surface_y(const vox_world *w, int x, int z)
{
    const int h = vox_world_chunks_y(w) * 16;
    int top = -1;
    for (int y = h - 1; y >= 0; y--) {
        if (vox_world_get(w, x, y, z) != VOX_BLOCK_AIR) {
            top = y;
            break;
        }
    }
    if (top < 0) return -1.0f;
    for (int y = top + 1; y <= top + 3; y++) {
        if (y < h && vox_world_get(w, x, y, z) != VOX_BLOCK_AIR) {
            return -1.0f;
        }
    }
    return (float)(top + 1);
}

/* Pick a spawn point near the world centre: first column with an open surface
 * gets a safe spot one block above the ground (gravity settles the rest).
 * Feet at (block + 0.5, surface + 1, block + 0.5) => never inside terrain. */
static vox_vec3 pick_spawn_feet(const vox_world *w)
{
    const int cx = vox_world_chunks_x(w) * 16 / 2;
    const int cz = vox_world_chunks_z(w) * 16 / 2;
    const int off_x[] = { 0, -2, 2, 0, 0, -4, 4, 0, 0, -6, 6, 0, 0, -8, 8 };
    const int off_z[] = { 0, 0, 0, -2, 2, 0, 0, -4, 4, 0, 0, -6, 6, 0, 0 };

    for (size_t i = 0; i < sizeof(off_x) / sizeof(off_x[0]); i++) {
        int x = cx + off_x[i];
        int z = cz + off_z[i];
        float sy = spawn_surface_y(w, x, z);
        if (sy > 0.0f) {
            return vox_v3((float)x + 0.5f, sy + 1.0f, (float)z + 0.5f);
        }
    }
    /* No open column found (should not happen): hover in the middle. */
    return vox_v3((float)cx + 0.5f, 9.0f, (float)cz + 0.5f);
}

vox_game *vox_game_create(void)
{
    vox_game *g = calloc(1, sizeof(vox_game));
    if (!g) return NULL;

    vox_tex_init(&g->atlas);

    g->world = vox_world_create(WORLD_SEED, CHUNK_X, CHUNK_Y, CHUNK_Z);
    if (!g->world) {
        free(g);
        return NULL;
    }
    vox_world_generate(g->world);
    g->chunks = CHUNK_X * CHUNK_Y * CHUNK_Z;
    g->blocks = vox_world_block_count(g->world);
    g->faces = vox_world_face_count(g->world);

    /* First-person player: spawn safely on the surface near the centre and
     * face back towards it. grounded=false so gravity settles the player. */
    vox_vec3 spawn = pick_spawn_feet(g->world);
    float spawn_yaw = atan2f(24.5f - spawn.x, 24.5f - spawn.z);
    vox_player_init(&g->player, spawn, spawn_yaw);
    g->cam.pos = vox_v3(spawn.x, spawn.y + VOX_PLAYER_EYE, spawn.z);
    g->cam.yaw = spawn_yaw;
    g->cam.pitch = 0.0f;
    g->cam.fov_y = 1.0472f;
    g->cam.near_plane = 0.1f;
    g->cam.far_plane = 150.0f;
    g->spin = false;
    g->selected_slot = 0;
    g->fps_tick = xTaskGetTickCount();

    /* Pre-shade every (texture, face-direction) tile exactly once: light and
     * normals are static, so the 256-pixel shade pass per face is pure waste. */
    float shades[VOX_FACE_COUNT];
    face_shades(shades);
    g->shade_cache = calloc((size_t)VOX_TEX_COUNT * VOX_FACE_COUNT,
                            VOX_TILE * VOX_TILE * sizeof(uint16_t));
    if (g->shade_cache) {
        for (int tex = 0; tex < VOX_TEX_COUNT; tex++) {
            for (int dir = 0; dir < VOX_FACE_COUNT; dir++) {
                vox_tex_shade_tile(&g->atlas, (vox_tex_id)tex, shades[dir],
                                   g->shade_cache +
                                   ((size_t)tex * VOX_FACE_COUNT + dir) *
                                   (VOX_TILE * VOX_TILE));
            }
        }
    }
    return g;
}

void vox_game_destroy(vox_game *g)
{
    if (!g) return;
    vox_world_destroy(g->world);
    vox_renderer_destroy(&g->renderer);
    free(g->shade_cache);
    free(g);
}

vox_renderer *vox_game_renderer(vox_game *g)
{
    return g ? &g->renderer : NULL;
}

const vox_tex_atlas *vox_game_atlas(vox_game *g)
{
    return g ? &g->atlas : NULL;
}

void vox_game_key(vox_game *g, uint16_t keycode, bool down)
{
    if (!g) return;
    uint16_t bit = key_to_bit(keycode);
    if (!bit) return;
    if (down) {
        g->keys |= bit;
    } else {
        g->keys &= (uint16_t)~bit;
    }
}

void vox_game_toggle_spin(vox_game *g)
{
    if (g) g->spin = !g->spin;
}

void vox_game_update(vox_game *g, float dt)
{
    if (!g) return;
    if (dt > 0.1f) dt = 0.1f;
    if (dt < 0.0f) dt = 0.0f;

    if (g->spin) {
        /* Debug-only auto-orbit around the world centre (F2 in the app).
         * Not the default camera: normal play is the first-person player. */
        vox_vec3 d = vox_v3_sub(g->cam.pos, WORLD_CENTER);
        d.y = 0.0f;
        float r = vox_v3_length(d);
        if (r < 8.0f) r = 8.0f;
        if (r > 60.0f) r = 60.0f;

        float a = atan2f(d.x, d.z) + 0.35f * dt;
        g->cam.pos.x = WORLD_CENTER.x + r * sinf(a);
        g->cam.pos.z = WORLD_CENTER.z + r * cosf(a);
        aim_at_world_center(g);
    } else {
        float fwd    = (g->keys & VK_W) ? 1.0f : 0.0f;
        float back   = (g->keys & VK_S) ? 1.0f : 0.0f;
        float sright = (g->keys & VK_D) ? 1.0f : 0.0f;
        float sleft  = (g->keys & VK_A) ? 1.0f : 0.0f;

        /* Keyboard look, continuous while held (Q left / E right / O up / L down). */
        const float turn = 2.5f * dt;
        float dyaw = 0.0f, dpitch = 0.0f;
        if (g->keys & VK_Q) dyaw   -= turn;
        if (g->keys & VK_E) dyaw   += turn;
        if (g->keys & VK_O) dpitch += turn;
        if (g->keys & VK_L) dpitch -= turn;
        if (dyaw != 0.0f || dpitch != 0.0f) vox_player_turn(&g->player, dyaw, dpitch);

        vox_player_input(&g->player, fwd - back, sright - sleft,
                         (g->keys & VK_SPACE) != 0);
        vox_player_update(&g->player, g->world, dt);
        g->keys &= (uint16_t)~VK_SPACE;   /* jump tap: one press = one jump */

        /* Safety net: a fall out of the world respawns the player on terrain. */
        if (g->player.pos.y < -4.0f) {
            vox_player_init(&g->player, pick_spawn_feet(g->world), g->player.yaw);
        }
    }

    TickType_t now = xTaskGetTickCount();
    if (now - g->fps_tick >= pdMS_TO_TICKS(1000)) {
        g->fps = g->frame_count;
        g->frame_count = 0;
        g->tris = 0;
        g->fps_tick = now;
    }
}

void vox_game_render(vox_game *g)
{
    if (!g || !g->renderer.fb || !g->world) return;

    int64_t t_frame = esp_timer_get_time();

    vox_renderer_begin(&g->renderer);

    /* Camera = player eye: feet plus eye height, facing from the player's
     * yaw/pitch. The projection/rendering pipeline is untouched. */
    if (!g->spin) {
        g->cam.pos = vox_v3(g->player.pos.x,
                            g->player.pos.y + g->player.eye_height,
                            g->player.pos.z);
        g->cam.yaw = g->player.yaw;
        g->cam.pitch = g->player.pitch;
    }
    vox_camera_proj proj = vox_camera_make_proj(&g->cam, g->renderer.width,
                                                g->renderer.height);

    /* Camera view basis, computed ONCE per frame (not per vertex). */
    vox_camera_basis b = vox_camera_get_basis(&g->cam);
    const float cx = g->cam.pos.x, cy = g->cam.pos.y, cz = g->cam.pos.z;

    /* Distant-side faces are culled in WORLD space with a single comparison:
     * an axis-aligned face whose outward normal points away from the camera
     * can never be visible, so skip its 4-corner transform entirely. */
    g->tris = 0;
    g->faces_culled = 0;
    g->faces_drawn = 0;
    int n_faces = vox_world_face_count(g->world);
    const vox_mesh_face *faces = vox_world_faces(g->world);
    if (!faces) return;

    int64_t t_geom = esp_timer_get_time();

    for (int fi = 0; fi < n_faces; fi++) {
        const vox_mesh_face *f = &faces[fi];
        const int bx = f->x, by = f->y, bz = f->z;

        /* Plane coordinates for each face direction (the face lies on the
         * bx/by/bz block corner for NEG, and bx+1/etc. for POS sides). */
        float plane = 0.0f;
        bool visible = false;
        switch (f->dir) {
        case VOX_FACE_Y_POS:
            plane = (float)(by + 1); visible = (cy > plane + 1e-4f); break;
        case VOX_FACE_Y_NEG:
            plane = (float)by;       visible = (cy < plane - 1e-4f); break;
        case VOX_FACE_X_POS:
            plane = (float)(bx + 1); visible = (cx > plane + 1e-4f); break;
        case VOX_FACE_X_NEG:
            plane = (float)bx;       visible = (cx < plane - 1e-4f); break;
        case VOX_FACE_Z_POS:
            plane = (float)(bz + 1); visible = (cz > plane + 1e-4f); break;
        case VOX_FACE_Z_NEG:
            plane = (float)bz;       visible = (cz < plane - 1e-4f); break;
        }
        if (!visible) {
            g->faces_culled++;
            continue;
        }

        float corners[4][3];
        uint8_t uv[4][2];
        vox_mesh_face_quad(f, bx, by, bz, corners, uv);

        vox_vv vv[4];
        bool behind = true;
        for (int k = 0; k < 4; k++) {
            vox_vec3 d0 = vox_v3(corners[k][0] - cx,
                                 corners[k][1] - cy,
                                 corners[k][2] - cz);
            vv[k].p.x = vox_v3_dot(d0, b.r);
            vv[k].p.y = vox_v3_dot(d0, b.u);
            vv[k].p.z = vox_v3_dot(d0, b.f);
            vv[k].u = (float)uv[k][0];
            vv[k].v = (float)uv[k][1];
            if (vv[k].p.z >= g->cam.near_plane) behind = false;
        }
        if (behind) {
            g->faces_culled++;
            continue;
        }

        const uint16_t *tile = g->shade_cache ? g->shade_cache +
                             ((size_t)f->tex * VOX_FACE_COUNT + f->dir) *
                             (VOX_TILE * VOX_TILE) : NULL;
        if (!tile) continue;

        g->faces_drawn++;
        project_triangle(g, &proj, vv[0], vv[1], vv[2], tile);
        project_triangle(g, &proj, vv[0], vv[2], vv[3], tile);
    }

    g->geom_us = (uint32_t)(esp_timer_get_time() - t_geom);
    g->begin_us = g->renderer.begin_us;
    g->raster_us = g->renderer.raster_us;
    g->frame_count++;
    g->frame_us_last = (uint32_t)(esp_timer_get_time() - t_frame);
}

int vox_game_fps(vox_game *g)
{
    return g ? g->fps : 0;
}

float vox_game_cam_distance(vox_game *g)
{
    if (!g) return 0.0f;
    return vox_v3_length(vox_v3_sub(g->cam.pos, WORLD_CENTER));
}

void vox_game_get_stats(vox_game *g, vox_game_stats *out)
{
    if (!g || !out) return;
    out->seed = vox_world_seed(g->world);
    out->chunks = g->chunks;
    out->blocks = g->blocks;
    out->faces = g->faces;
    out->tris = g->tris;
    out->fps = g->fps;
    out->cam_dist = vox_game_cam_distance(g);
    out->faces_culled = g->faces_culled;
    out->faces_drawn = g->faces_drawn;
    out->frags_drawn = g->renderer.frags_drawn;
    out->frags_tested = g->renderer.frags_tested;
    out->begin_us = g->begin_us;
    out->geom_us = g->geom_us;
    out->raster_us = g->raster_us;
    out->frame_us = g->frame_us_last;
}

int vox_game_selected_slot(vox_game *g)
{
    if (!g) return 0;
    return g->selected_slot;
}

void vox_game_set_slot(vox_game *g, int slot)
{
    if (!g) return;
    if (slot < 0) slot = 0;
    if (slot >= VOX_HOTBAR_SLOTS) slot = VOX_HOTBAR_SLOTS - 1;
    g->selected_slot = slot;
}

vox_block_id vox_game_slot_block(const vox_game *g, int slot)
{
    if (!g) return VOX_BLOCK_AIR;
    if (slot < 0 || slot >= VOX_HOTBAR_SLOTS) return VOX_BLOCK_AIR;
    return (vox_block_id)HOTBAR_BLOCKS[slot];
}

void vox_game_mouse(vox_game *g, int dx, int dy)
{
    if (!g) return;
    /* HID deltas from the driver (usb_host_input.c sends x/y displacement):
     * mouse right = +X -> yaw increases; mouse down = +Y -> pitch decreases.
     * Kept here, isolated from the UI, so the mapping matches the camera
     * convention (yaw 0 = +Z, positive pitch = up). */
    vox_player_turn(&g->player,
                    (float)dx * VOX_PLAYER_MOUSE_SENS,
                    (float)-dy * VOX_PLAYER_MOUSE_SENS);
}

void vox_game_player_info(const vox_game *g, vox_game_player_state *out)
{
    if (!g || !out) return;
    out->pos = g->player.pos;
    out->vel = g->player.vel;
    out->yaw = g->player.yaw;
    out->pitch = g->player.pitch;
    out->grounded = g->player.grounded;
}