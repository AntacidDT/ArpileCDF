#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "voxel_camera.h"
#include "voxel_renderer.h"
#include "voxel_texture.h"
#include "voxel_block.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vox_game vox_game;

/* Inventory hotbar: 8 selectable slots. Slots 0..5 map to the six block
 * types; 6..7 are empty until block placing/collecting arrives. */
#define VOX_HOTBAR_SLOTS 8

typedef struct {
    uint32_t seed;
    int chunks;      /* generated chunk count */
    int blocks;      /* non-AIR blocks in the world */
    int faces;       /* visible faces in the built mesh */
    int tris;        /* triangles rasterized last frame */
    int fps;
    float cam_dist;
    /* Per-frame performance data (last rendered frame). */
    uint32_t faces_culled;   /* rejected by backface/near/frustum this frame */
    uint32_t faces_drawn;    /* faces sent to the rasterizer */
    uint32_t frags_drawn;    /* pixels written (depth-passing) */
    uint32_t frags_tested;   /* pixel depth-test attempts */
    uint32_t begin_us;       /* sky fill + z clear */
    uint32_t geom_us;        /* transforms + culling + projection */
    uint32_t raster_us;      /* triangle rasterization */
    uint32_t frame_us;       /* full vox_game_render */
} vox_game_stats;

vox_game *vox_game_create(void);

void vox_game_destroy(vox_game *g);

vox_renderer *vox_game_renderer(vox_game *g);

/* Procedural texture atlas backing the world (for UI block icons etc.). */
const vox_tex_atlas *vox_game_atlas(vox_game *g);

void vox_game_key(vox_game *g, uint16_t keycode, bool down);

void vox_game_toggle_spin(vox_game *g);

void vox_game_update(vox_game *g, float dt);

void vox_game_render(vox_game *g);

int vox_game_fps(vox_game *g);

float vox_game_cam_distance(vox_game *g);

void vox_game_get_stats(vox_game *g, vox_game_stats *out);

/* ---- hotbar (UI infrastructure for the later block-interaction system) ---- */

/* Current selected hotbar slot index (0..VOX_HOTBAR_SLOTS-1). */
int vox_game_selected_slot(vox_game *g);

/* Select a hotbar slot, clamped into range. */
void vox_game_set_slot(vox_game *g, int slot);

/* Block id held by a hotbar slot (0..VOX_HOTBAR_SLOTS-1); VOX_BLOCK_AIR when
 * the slot is empty. */
vox_block_id vox_game_slot_block(const vox_game *g, int slot);

/* ---- first-person player ------------------------------------------------ */

/* Snapshot of the player for the UI/debug layer. `pos` is the feet-centre of
 * the collision box; the camera sits VOX_PLAYER_EYE above it on the same
 * column (see voxel_player.h). */
typedef struct {
    vox_vec3 pos;
    vox_vec3 vel;
    float yaw;
    float pitch;
    bool grounded;
} vox_game_player_state;

/* Apply a HID mouse movement (dx, dy deltas) to the player's view. The
 * delta->yaw/pitch mapping and sensitivity live here, next to the camera
 * math, so the mouse-look mapping stays isolated from the UI layer. */
void vox_game_mouse(vox_game *g, int dx, int dy);

/* Snapshot the current player state (see vox_game_player_state). */
void vox_game_player_info(const vox_game *g, vox_game_player_state *out);

#ifdef __cplusplus
}
#endif