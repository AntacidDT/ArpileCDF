#include <string.h>
#include "voxel_chunk.h"
#include "esp_heap_caps.h"

#define IDX(x, y, z) (((y) * VOX_CHUNK + (z)) * VOX_CHUNK + (x))

vox_chunk *vox_chunk_alloc(void)
{
    vox_chunk *c = heap_caps_malloc(sizeof(vox_chunk), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!c) {
        c = calloc(1, sizeof(vox_chunk));
    }
    if (c) {
        memset((void *)c->blocks, VOX_BLOCK_AIR, sizeof(c->blocks));
    }
    return c;
}

void vox_chunk_free(vox_chunk *c)
{
    if (!c) return;
    heap_caps_free(c);
}

uint8_t vox_chunk_get(const vox_chunk *c, int x, int y, int z)
{
    if (!c) return VOX_BLOCK_AIR;
    if (x < 0 || x >= VOX_CHUNK || y < 0 || y >= VOX_CHUNK ||
        z < 0 || z >= VOX_CHUNK) {
        return VOX_BLOCK_AIR;
    }
    return c->blocks[IDX(x, y, z)];
}

void vox_chunk_set(vox_chunk *c, int x, int y, int z, uint8_t block)
{
    if (!c) return;
    if (x < 0 || x >= VOX_CHUNK || y < 0 || y >= VOX_CHUNK ||
        z < 0 || z >= VOX_CHUNK) {
        return;
    }
    c->blocks[IDX(x, y, z)] = block;
}

void vox_chunk_fill(vox_chunk *c, uint8_t block)
{
    if (!c) return;
    memset((void *)c->blocks, block, sizeof(c->blocks));
}

int vox_chunk_block_count(const vox_chunk *c)
{
    int n = 0;
    if (!c) return 0;
    for (int i = 0; i < VOX_CHUNK_BLOCKS; i++) {
        if (c->blocks[i] != VOX_BLOCK_AIR) n++;
    }
    return n;
}