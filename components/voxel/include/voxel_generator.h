#pragma once

#include <stdint.h>
#include "voxel_world.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Deterministic seeded noise/terrain generation. Same seed, same world. */

/* Value-noise height for a world column (x, z). Range roughly [2, 18]. */
int vox_gen_height(uint32_t seed, int x, int z);

/* Fill the world's chunks with terrain + decorated trees. */
void vox_gen_world(uint32_t seed, vox_world *world);

#ifdef __cplusplus
}
#endif