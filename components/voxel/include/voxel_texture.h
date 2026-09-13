#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VOX_TILE 16
#define VOX_ATLAS_W (4 * VOX_TILE)
#define VOX_ATLAS_H (2 * VOX_TILE)

typedef enum {
    VOX_TEX_GRASS,
    VOX_TEX_DIRT,
    VOX_TEX_STONE,
    VOX_TEX_GRASS_SIDE,
    VOX_TEX_SAND,
    VOX_TEX_WOOD_TOP,
    VOX_TEX_WOOD_SIDE,
    VOX_TEX_LEAVES,
    VOX_TEX_COUNT
} vox_tex_id;

typedef struct {
    uint16_t atlas[VOX_ATLAS_W * VOX_ATLAS_H];
} vox_tex_atlas;

void vox_tex_init(vox_tex_atlas *atlas);

uint16_t vox_tex_sample(const vox_tex_atlas *atlas, vox_tex_id id, int tx, int ty);

void vox_tex_shade_tile(const vox_tex_atlas *atlas, vox_tex_id id, float shade,
                        uint16_t out[VOX_TILE * VOX_TILE]);

#ifdef __cplusplus
}
#endif