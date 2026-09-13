#pragma once

#include <stdint.h>
#include "voxel_block.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One rendered block face. Block world coords are the cell that owns the face;
 * corners are derived from the per-direction corner/UV tables below. */
typedef struct vox_mesh_face {
    int16_t x, y, z;
    uint8_t dir;   /* VOX_FACE_* */
    uint8_t tex;   /* vox_tex_id */
} vox_mesh_face;

/* Corner offsets (0..1 in each axis) for a face direction, 4 corners each,
 * ordered so that cross(e1,e2) points OUT of the block. */
extern const int8_t vox_mesh_corner[VOX_FACE_COUNT][4][3];

/* UV (0..15) per corner, matching the corner order. */
extern const uint8_t vox_mesh_uv[VOX_FACE_COUNT][4][2];

/* Face normal per direction. */
extern const int8_t vox_mesh_normal[VOX_FACE_COUNT][3];

/* Neighbour cell offset (block coords) on each face side. */
extern const int8_t vox_mesh_delta[VOX_FACE_COUNT][3];

/* The 4 world-space corners of a face, as floats. */
void vox_mesh_face_quad(const vox_mesh_face *f, int bx, int by, int bz,
                        float out[4][3], uint8_t out_uv[4][2]);

#ifdef __cplusplus
}
#endif