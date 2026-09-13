#include "voxel_mesh.h"

/* Corner order chosen so cross(c1-c0, c2-c0) points OUT of the block. */
const int8_t vox_mesh_corner[VOX_FACE_COUNT][4][3] = {
    /* Y_POS (top) */
    { { 0, 1, 0 }, { 0, 1, 1 }, { 1, 1, 1 }, { 1, 1, 0 } },
    /* Y_NEG (bottom) */
    { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 0, 1 }, { 0, 0, 1 } },
    /* X_POS */
    { { 1, 0, 0 }, { 1, 1, 0 }, { 1, 1, 1 }, { 1, 0, 1 } },
    /* X_NEG */
    { { 0, 0, 0 }, { 0, 0, 1 }, { 0, 1, 1 }, { 0, 1, 0 } },
    /* Z_POS */
    { { 0, 0, 1 }, { 1, 0, 1 }, { 1, 1, 1 }, { 0, 1, 1 } },
    /* Z_NEG */
    { { 0, 0, 0 }, { 0, 1, 0 }, { 1, 1, 0 }, { 1, 0, 0 } },
};

const uint8_t vox_mesh_uv[VOX_FACE_COUNT][4][2] = {
    /* Y_POS: u across x, top-left of GRASS top */
    { { 0, 15 }, { 15, 15 }, { 15, 0 }, { 0, 0 } },
    /* Y_NEG */
    { { 0, 0 }, { 15, 0 }, { 15, 15 }, { 0, 15 } },
    /* X_POS: v=0 at top (+y) so the grass strip faces up */
    { { 0, 15 }, { 0, 0 }, { 15, 0 }, { 15, 15 } },
    /* X_NEG */
    { { 0, 15 }, { 15, 15 }, { 15, 0 }, { 0, 0 } },
    /* Z_POS */
    { { 0, 15 }, { 15, 15 }, { 15, 0 }, { 0, 0 } },
    /* Z_NEG */
    { { 0, 15 }, { 0, 0 }, { 15, 0 }, { 15, 15 } },
};

const int8_t vox_mesh_normal[VOX_FACE_COUNT][3] = {
    { 0, 1, 0 }, { 0, -1, 0 },
    { 1, 0, 0 }, { -1, 0, 0 },
    { 0, 0, 1 }, { 0, 0, -1 },
};

const int8_t vox_mesh_delta[VOX_FACE_COUNT][3] = {
    { 0, 1, 0 }, { 0, -1, 0 },
    { 1, 0, 0 }, { -1, 0, 0 },
    { 0, 0, 1 }, { 0, 0, -1 },
};

void vox_mesh_face_quad(const vox_mesh_face *f, int bx, int by, int bz,
                        float out[4][3], uint8_t out_uv[4][2])
{
    const int8_t (*corners)[3] = vox_mesh_corner[f->dir];
    for (int k = 0; k < 4; k++) {
        out[k][0] = (float)(bx + corners[k][0]);
        out[k][1] = (float)(by + corners[k][1]);
        out[k][2] = (float)(bz + corners[k][2]);
        out_uv[k][0] = vox_mesh_uv[f->dir][k][0];
        out_uv[k][1] = vox_mesh_uv[f->dir][k][1];
    }
}