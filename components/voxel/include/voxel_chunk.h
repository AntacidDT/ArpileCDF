#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include "voxel_block.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VOX_CHUNK 16
#define VOX_CHUNK_BLOCKS (VOX_CHUNK * VOX_CHUNK * VOX_CHUNK)

/* A 16^3 block column, stored as a flat uint8 array.
 * Layout: index = x + z*VOX_CHUNK + y*VOX_CHUNK*VOX_CHUNK. */
typedef struct {
    uint8_t blocks[VOX_CHUNK_BLOCKS];
} vox_chunk;

/* Allocate a chunk (PSRAM preferred) or NULL on failure. */
vox_chunk *vox_chunk_alloc(void);

void vox_chunk_free(vox_chunk *c);

/* Local block access, coordinates 0..VOX_CHUNK-1. OOB reads return AIR. */
uint8_t vox_chunk_get(const vox_chunk *c, int x, int y, int z);

void vox_chunk_set(vox_chunk *c, int x, int y, int z, uint8_t block);

/* Fill every cell with `block`. */
void vox_chunk_fill(vox_chunk *c, uint8_t block);

/* Count non-AIR blocks. */
int vox_chunk_block_count(const vox_chunk *c);

#ifdef __cplusplus
}
#endif