#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "voxel_texture.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Block IDs stored in a chunk (8-bit). AIR is the empty block. */
typedef enum {
    VOX_BLOCK_AIR = 0,
    VOX_BLOCK_GRASS,
    VOX_BLOCK_DIRT,
    VOX_BLOCK_STONE,
    VOX_BLOCK_SAND,
    VOX_BLOCK_WOOD,
    VOX_BLOCK_LEAVES,
    VOX_BLOCK_COUNT
} vox_block_id;

/* Face directions. Order must match vox_mesh_shade order + face table in the
 * renderer (see vox_mesh.h). */
enum {
    VOX_FACE_Y_POS = 0,
    VOX_FACE_Y_NEG = 1,
    VOX_FACE_X_POS = 2,
    VOX_FACE_X_NEG = 3,
    VOX_FACE_Z_POS = 4,
    VOX_FACE_Z_NEG = 5,
    VOX_FACE_COUNT
};

/* Texture used for a given block face. */
vox_tex_id vox_block_face_tex(vox_block_id block, int face);

/* True if the block is fully opaque (all faces can be culled vs. neighbours). */
bool vox_block_is_opaque(vox_block_id block);

#ifdef __cplusplus
}
#endif