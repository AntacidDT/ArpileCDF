#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "voxel_chunk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A finite grid of chunks (3x1x3 by default). Chunks are 16^3, so the world
 * spans chunk_x*16 by chunk_y*16 by chunk_z*16 blocks. */

typedef struct vox_world vox_world;

/* Allocate a world with the given chunk layout and seed. */
vox_world *vox_world_create(uint32_t seed, int chunk_x, int chunk_y, int chunk_z);

void vox_world_destroy(vox_world *w);

/* Generate terrain + trees for every chunk (deterministic per seed). */
void vox_world_generate(vox_world *w);

/* Build the persistent face mesh (hidden faces culled). */
void vox_world_build_mesh(vox_world *w);

/* World-space block access (OOB -> AIR). */
uint8_t vox_world_get(const vox_world *w, int wx, int wy, int wz);

void vox_world_set(vox_world *w, int wx, int wy, int wz, uint8_t block);

uint32_t vox_world_seed(const vox_world *w);

int vox_world_chunks_x(const vox_world *w);

int vox_world_chunks_y(const vox_world *w);

int vox_world_chunks_z(const vox_world *w);

int vox_world_block_count(const vox_world *w);

/* Face mesh access. */
int vox_world_face_count(const vox_world *w);

const struct vox_mesh_face *vox_world_faces(const vox_world *w);

#ifdef __cplusplus
}
#endif